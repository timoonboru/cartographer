/*
 * Copyright 2016 The Cartographer Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "cartographer/mapping_2d/sparse_pose_graph.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <string>

#include <fstream>

#include "Eigen/Eigenvalues"
#include "cartographer/common/port.h"
#include "cartographer/mapping_2d/probability_grid.h"
#include "cartographer/common/make_unique.h"
#include "cartographer/common/math.h"
#include "cartographer/mapping/sparse_pose_graph/proto/constraint_builder_options.pb.h"
#include "cartographer/sensor/compressed_point_cloud.h"
#include "cartographer/sensor/voxel_filter.h"
#include "glog/logging.h"

int Frequence = 0;

void WriteProbabilityGridToPgm(const ::cartographer::mapping_2d::ProbabilityGrid& grid,
                             const std::string& filename)
{
  Eigen::Array2i offset;
  ::cartographer::mapping_2d::CellLimits cell_limits;
  grid.ComputeCroppedLimits(&offset, &cell_limits);

  int data[cell_limits.num_x_cells * cell_limits.num_y_cells];

  for (const Eigen::Array2i& xy_index :
       ::cartographer::mapping_2d::XYIndexRangeIterator(cell_limits)) {
    if (grid.IsKnown(xy_index + offset)) {
      const int value = ::cartographer::common::RoundToInt(
          (grid.GetProbability(xy_index + offset) -
           ::cartographer::mapping::kMinProbability) *
          100. /
          (::cartographer::mapping::kMaxProbability - ::cartographer::mapping::kMinProbability));
      CHECK_LE(0, value);
      CHECK_GE(100, value);
      data[(cell_limits.num_x_cells - xy_index.x()) *
                               cell_limits.num_y_cells -
                           xy_index.y() - 1] = value;
    }
  }

  LOG(INFO) << "Saving map to '" << filename << "'...";
  std::ofstream pgm_file(filename, std::ios::out | std::ios::binary);
  const std::string header = "P5\n# Cartographer map; " +
                             std::to_string(grid.limits().resolution()) +
                             " m/pixel\n" + std::to_string(cell_limits.num_y_cells) +
                             " " + std::to_string(cell_limits.num_x_cells) + "\n255\n";

  pgm_file.write(header.data(), header.size());

  for (size_t y = 0; y < cell_limits.num_x_cells; ++y) {
    for (size_t x = 0; x < cell_limits.num_y_cells; ++x) {
      const size_t i = x + (cell_limits.num_x_cells - y - 1) * cell_limits.num_y_cells;
      if (data[i] >= 0 && data[i] <= 100) {
        pgm_file.put((100 - data[i]) * 255 / 100);
      } else {
        // We choose a value between the free and occupied threshold.
        constexpr uint8_t kUnknownValue = 128;
        pgm_file.put(kUnknownValue);
      }
    }
  }
  pgm_file.close();
  CHECK(pgm_file) << "Writing '" << filename << "' failed.";
}

namespace cartographer {
namespace mapping_2d {

SparsePoseGraph::SparsePoseGraph(
    const mapping::proto::SparsePoseGraphOptions& options,
    common::ThreadPool* thread_pool)
    : options_(options),
      optimization_problem_(options_.optimization_problem_options()),
      constraint_builder_(options_.constraint_builder_options(), thread_pool) {}

SparsePoseGraph::~SparsePoseGraph() {
  WaitForAllComputations();
  common::MutexLocker locker(&mutex_);
  CHECK(scan_queue_ == nullptr);
}

std::vector<mapping::SubmapId> SparsePoseGraph::GrowSubmapTransformsAsNeeded(
    const int trajectory_id,
    const std::vector<std::shared_ptr<const Submap>>& insertion_submaps) {
  CHECK(!insertion_submaps.empty());
  const auto& submap_data = optimization_problem_.submap_data();
  if (insertion_submaps.size() == 1) {
    // If we don't already have an entry for the first submap, add one.
    if (static_cast<size_t>(trajectory_id) >= submap_data.size() ||
        submap_data[trajectory_id].empty()) {
      optimization_problem_.AddSubmap(
          trajectory_id,
          sparse_pose_graph::ComputeSubmapPose(*insertion_submaps[0]));
    }
    CHECK_EQ(optimization_problem_.num_trimmed_submaps(trajectory_id), 0);
    CHECK_EQ(submap_data[trajectory_id].size(), 1);
    const mapping::SubmapId submap_id{trajectory_id, 0};
    CHECK(submap_data_.at(submap_id).submap == insertion_submaps.front());
    return {submap_id};
  }
  CHECK_EQ(2, insertion_submaps.size());
  const int num_trimmed_submaps =
      optimization_problem_.num_trimmed_submaps(trajectory_id);
  const mapping::SubmapId last_submap_id{
      trajectory_id, static_cast<int>(submap_data.at(trajectory_id).size() +
                                      num_trimmed_submaps - 1)};
  if (submap_data_.at(last_submap_id).submap == insertion_submaps.front()) {
    // In this case, 'last_submap_id' is the ID of 'insertions_submaps.front()'
    // and 'insertions_submaps.back()' is new.
    const auto& first_submap_pose =
        submap_data.at(trajectory_id)
            .at(last_submap_id.submap_index - num_trimmed_submaps)
            .pose;
    optimization_problem_.AddSubmap(
        trajectory_id,
        first_submap_pose *
            sparse_pose_graph::ComputeSubmapPose(*insertion_submaps[0])
                .inverse() *
            sparse_pose_graph::ComputeSubmapPose(*insertion_submaps[1]));
    return {last_submap_id,
            mapping::SubmapId{trajectory_id, last_submap_id.submap_index + 1}};
  }
  CHECK(submap_data_.at(last_submap_id).submap == insertion_submaps.back());
  const mapping::SubmapId front_submap_id{trajectory_id,
                                          last_submap_id.submap_index - 1};
  CHECK(submap_data_.at(front_submap_id).submap == insertion_submaps.front());
  return {front_submap_id, last_submap_id};
}

void SparsePoseGraph::AddScan(
    common::Time time, const transform::Rigid3d& tracking_to_pose,
    const sensor::RangeData& range_data_in_pose, const transform::Rigid2d& pose,
    const int trajectory_id,
    const std::vector<std::shared_ptr<const Submap>>& insertion_submaps) {
  const transform::Rigid3d optimized_pose(
      GetLocalToGlobalTransform(trajectory_id) * transform::Embed3D(pose));
  //mnf optimized_pose ~ pose ???   optimized_pose = L2G * pose
  // L2G = T2G(n-1) * L2T(n-1)

  common::MutexLocker locker(&mutex_);
  trajectory_nodes_.Append(
      trajectory_id,
      mapping::TrajectoryNode{
          std::make_shared<const mapping::TrajectoryNode::Data>(
              mapping::TrajectoryNode::Data{time, Compress(range_data_in_pose),
                                            tracking_to_pose}),
          optimized_pose});
  // mnf optimized_pose := tracking_2d_to_global
  //                    := TrajectoryNode.pose
  //               pose := pose_estimate_2d := tracking_2d_to_local
  //   tracking_to_pose := tracking_to_tracking_2d
  // range_data_in_pose := range_data_in_tracking_2d
  //                    := range {tracking_to_tracking_2d * tracking}

  // mnf 
  //  trajectory_nodes_ <- tracking_2d_to_global
  //       submap_data_ <- tracking_2d_to_local
  ++num_trajectory_nodes_;
  //double yaw = ::cartographer::transform::GetYaw(optimized_pose.rotation());
  //LOG(INFO)<<"yaw"<<yaw;
  trajectory_connectivity_.Add(trajectory_id);
  // Test if the 'insertion_submap.back()' is one we never saw before.
  if (trajectory_id >= submap_data_.num_trajectories() ||
      submap_data_.num_indices(trajectory_id) == 0 ||
      submap_data_
              .at(mapping::SubmapId{
                  trajectory_id, submap_data_.num_indices(trajectory_id) - 1})
              .submap != insertion_submaps.back()) {
    // We grow 'submap_data_' as needed. This code assumes that the first
    // time we see a new submap is as 'insertion_submaps.back()'.

    // mnf use ptr for !=     and   handle 
    const mapping::SubmapId submap_id =
        submap_data_.Append(trajectory_id, SubmapData());
    submap_data_.at(submap_id).submap = insertion_submaps.back();
  }

  // Make sure we have a sampler for this trajectory.
  if (!global_localization_samplers_[trajectory_id]) {
    global_localization_samplers_[trajectory_id] =
        common::make_unique<common::FixedRatioSampler>(
            options_.global_sampling_ratio());
  }


  // We have to check this here, because it might have changed by the time we
  // execute the lambda.
  const bool newly_finished_submap = insertion_submaps.front()->finished();

 /* Frequence++;
  if(Frequence > 3)
  {
      WriteProbabilityGridToPgm((insertion_submaps.front()->probability_grid()),"/home/nvidia/OUTMAP/front.pgm");
      WriteProbabilityGridToPgm((insertion_submaps.back()->probability_grid()),"/home/nvidia/OUTMAP/back.pgm");
      WriteProbabilityGridToPgm((submap_data_
              .at(mapping::SubmapId{
                  trajectory_id, submap_data_.num_indices(trajectory_id) - 1})
              .submap->probability_grid()),"/home/nvidia/OUTMAP/BFfront.pgm");
      Frequence = 0;
  }
*/

  AddWorkItem([=]() REQUIRES(mutex_) {
    ComputeConstraintsForScan(trajectory_id, insertion_submaps,
                              newly_finished_submap, pose);
  });
}

void SparsePoseGraph::AddWorkItem(std::function<void()> work_item) {
  if (scan_queue_ == nullptr) {
    work_item();
  } else {
    scan_queue_->push_back(work_item);
  }
}

//mnf In 2d cartographer, imu data only be used with optimization_problem_.TrimTrajectoryNode(node_id);
void SparsePoseGraph::AddImuData(const int trajectory_id, common::Time time,
                                 const Eigen::Vector3d& linear_acceleration,
                                 const Eigen::Vector3d& angular_velocity,
                                 const Eigen::Quaterniond& orientiation) {
  common::MutexLocker locker(&mutex_);
  AddWorkItem([=]() REQUIRES(mutex_) {
    optimization_problem_.AddImuData(trajectory_id, time, linear_acceleration,
                                     angular_velocity, orientiation);
  });
}

void SparsePoseGraph::ComputeConstraint(const mapping::NodeId& node_id,
                                        const mapping::SubmapId& submap_id) {
  CHECK(submap_data_.at(submap_id).state == SubmapState::kFinished);

  // Only globally match against submaps not in this trajectory.
  if (node_id.trajectory_id != submap_id.trajectory_id &&
      global_localization_samplers_[node_id.trajectory_id]->Pulse()) {
    constraint_builder_.MaybeAddGlobalConstraint(
        submap_id, submap_data_.at(submap_id).submap.get(), node_id,
        &trajectory_nodes_.at(node_id).constant_data->range_data.returns,
        &trajectory_connectivity_);
  } else {
    const bool scan_and_submap_trajectories_connected =
        reverse_connected_components_.count(node_id.trajectory_id) > 0 &&
        reverse_connected_components_.count(submap_id.trajectory_id) > 0 &&
        reverse_connected_components_.at(node_id.trajectory_id) ==
            reverse_connected_components_.at(submap_id.trajectory_id);
    if (node_id.trajectory_id == submap_id.trajectory_id ||
        scan_and_submap_trajectories_connected) {
      const transform::Rigid2d initial_relative_pose =
          optimization_problem_.submap_data()
              .at(submap_id.trajectory_id)
              .at(submap_id.submap_index -
                  optimization_problem_.num_trimmed_submaps(
                      submap_id.trajectory_id))
              .pose.inverse() *
          optimization_problem_.node_data()
              .at(node_id.trajectory_id)
              .at(node_id.node_index - optimization_problem_.num_trimmed_nodes(
                                           node_id.trajectory_id))
              .point_cloud_pose;
      constraint_builder_.MaybeAddConstraint(
          submap_id, submap_data_.at(submap_id).submap.get(), node_id,
          &trajectory_nodes_.at(node_id).constant_data->range_data.returns,
          initial_relative_pose);
      //mnf initial_relative_pose := Submap(i).G2T * Node(j).T2G
    }
  }
}

void SparsePoseGraph::ComputeConstraintsForOldScans(
    const mapping::SubmapId& submap_id) {
  const auto& submap_data = submap_data_.at(submap_id);
  const auto& node_data = optimization_problem_.node_data();
  for (size_t trajectory_id = 0; trajectory_id != node_data.size();
       ++trajectory_id) {
    for (size_t node_data_index = 0;
         node_data_index != node_data[trajectory_id].size();
         ++node_data_index) {
      const int node_index =
          node_data_index +
          optimization_problem_.num_trimmed_nodes(trajectory_id);
      const mapping::NodeId node_id{static_cast<int>(trajectory_id),
                                    static_cast<int>(node_index)};
      CHECK(!trajectory_nodes_.at(node_id).trimmed());
      if (submap_data.node_ids.count(node_id) == 0) {
        ComputeConstraint(node_id, submap_id);
      }
    }
  }
}

void SparsePoseGraph::ComputeConstraintsForScan(
    const int trajectory_id,
    std::vector<std::shared_ptr<const Submap>> insertion_submaps,
    const bool newly_finished_submap, const transform::Rigid2d& pose) {
  const std::vector<mapping::SubmapId> submap_ids =
      GrowSubmapTransformsAsNeeded(trajectory_id, insertion_submaps);
  //mnf submap_ids is IDS of insertion_submaps , consider with the trimmed_submaps
  //if has new submap , updata the submap_data_ for OptimizationProblem
  CHECK_EQ(submap_ids.size(), insertion_submaps.size());
  const mapping::SubmapId matching_id = submap_ids.front();
  const int num_trimmed_submaps =
      optimization_problem_.num_trimmed_submaps(trajectory_id); //num_trimmed_submaps == 0
  const transform::Rigid2d optimized_pose =
      optimization_problem_.submap_data()
          .at(matching_id.trajectory_id)
          .at(matching_id.submap_index - num_trimmed_submaps)
          .pose *
      sparse_pose_graph::ComputeSubmapPose(*insertion_submaps.front())
          .inverse() *
      pose;
  // mnf T2G = T2G(n-1) * L2T(n-1) * T2L(n) ??? differemt with optimized_pose in "AddScan"
  // mnf  SAME!!!   It seems that :
  // optimization_problem_.submap_data() :=  optimized_submap_transforms_ := T2G(n-1)
  //           insertion_submaps.front() :=  submap_data_                 := T2L(n-1)

  const mapping::NodeId node_id{
      matching_id.trajectory_id,
      static_cast<size_t>(matching_id.trajectory_id) <
              optimization_problem_.node_data().size()
          ? static_cast<int>(optimization_problem_.node_data()
                                 .at(matching_id.trajectory_id)
                                 .size()) +
                optimization_problem_.num_trimmed_nodes(
                    matching_id.trajectory_id)
          : 0};

  const auto& scan_data = trajectory_nodes_.at(node_id).constant_data;
  optimization_problem_.AddTrajectoryNode(
      matching_id.trajectory_id, scan_data->time, pose, optimized_pose);
  //mnf initial_point_cloud_pose := pose
  //            point_cloud_pose := optimized_pose

  for (size_t i = 0; i < insertion_submaps.size(); ++i) {
    const mapping::SubmapId submap_id = submap_ids[i];
    // Even if this was the last scan added to 'submap_id', the submap will only
    // be marked as finished in 'submap_data_' further below.
    CHECK(submap_data_.at(submap_id).state == SubmapState::kActive);
    submap_data_.at(submap_id).node_ids.emplace(node_id);
    //LOG(INFO)<<"AFT node_ids.size()"<< submap_data_.at(submap_id).node_ids.size()<<" submaps id "<< submap_id.submap_index;
    const transform::Rigid2d constraint_transform =
        sparse_pose_graph::ComputeSubmapPose(*insertion_submaps[i]).inverse() *
        pose; //mnf M_L2T * N_T2L
    constraints_.push_back(Constraint{submap_id,
                                      node_id,
                                      {transform::Embed3D(constraint_transform),
                                       options_.matcher_translation_weight(),
                                       options_.matcher_rotation_weight()},
                                      Constraint::INTRA_SUBMAP});
  }

  for (int trajectory_id = 0; trajectory_id < submap_data_.num_trajectories();
       ++trajectory_id) {
    for (int submap_index = 0;
         submap_index < submap_data_.num_indices(trajectory_id);
         ++submap_index) {
      const mapping::SubmapId submap_id{trajectory_id, submap_index};
      if (submap_data_.at(submap_id).state == SubmapState::kFinished) {
        CHECK_EQ(submap_data_.at(submap_id).node_ids.count(node_id), 0);
        ComputeConstraint(node_id, submap_id);
      }
    }
  }

  if (newly_finished_submap) {
    const mapping::SubmapId finished_submap_id = submap_ids.front();
    SubmapData& finished_submap_data = submap_data_.at(finished_submap_id);
    CHECK(finished_submap_data.state == SubmapState::kActive);
    finished_submap_data.state = SubmapState::kFinished;
    // We have a new completed submap, so we look into adding constraints for
    // old scans.
    ComputeConstraintsForOldScans(finished_submap_id);
  }
  constraint_builder_.NotifyEndOfScan(); //Notify tongzhi
  ++num_scans_since_last_loop_closure_;
  if (options_.optimize_every_n_scans() > 0 &&
      num_scans_since_last_loop_closure_ > options_.optimize_every_n_scans()) {
    CHECK(!run_loop_closure_);
    run_loop_closure_ = true;
    // If there is a 'scan_queue_' already, some other thread will take care.
    if (scan_queue_ == nullptr) {
      scan_queue_ = common::make_unique<std::deque<std::function<void()>>>();
      HandleScanQueue();
    }
  }
}

void SparsePoseGraph::HandleScanQueue() {
  constraint_builder_.WhenDone(
      [this](const sparse_pose_graph::ConstraintBuilder::Result& result) {
        {
          common::MutexLocker locker(&mutex_);
          constraints_.insert(constraints_.end(), result.begin(), result.end());
        }
        RunOptimization();

        common::MutexLocker locker(&mutex_);
        num_scans_since_last_loop_closure_ = 0;
        run_loop_closure_ = false;
        while (!run_loop_closure_) {
          if (scan_queue_->empty()) {
            LOG(INFO) << "We caught up. Hooray!";
            scan_queue_.reset();
            return;
          }
          scan_queue_->front()();
          scan_queue_->pop_front();
        }
        // We have to optimize again.
        HandleScanQueue();
      });
}

void SparsePoseGraph::WaitForAllComputations() {
  bool notification = false;
  common::MutexLocker locker(&mutex_);
  const int num_finished_scans_at_start =
      constraint_builder_.GetNumFinishedScans();
  while (!locker.AwaitWithTimeout(
      [this]() REQUIRES(mutex_) {
        return constraint_builder_.GetNumFinishedScans() ==
               num_trajectory_nodes_;
      },
      common::FromSeconds(1.))) {
    std::ostringstream progress_info;
    progress_info << "Optimizing: " << std::fixed << std::setprecision(1)
                  << 100. *
                         (constraint_builder_.GetNumFinishedScans() -
                          num_finished_scans_at_start) /
                         (num_trajectory_nodes_ - num_finished_scans_at_start)
                  << "%...";
    std::cout << "\r\x1b[K" << progress_info.str() << std::flush;
  }
  std::cout << "\r\x1b[KOptimizing: Done.     " << std::endl;
  constraint_builder_.WhenDone(
      [this, &notification](
          const sparse_pose_graph::ConstraintBuilder::Result& result) {
        common::MutexLocker locker(&mutex_);
        constraints_.insert(constraints_.end(), result.begin(), result.end());
        notification = true;
      });
  locker.Await([&notification]() { return notification; });
}

void SparsePoseGraph::FreezeTrajectory(const int trajectory_id) {
  common::MutexLocker locker(&mutex_);
  AddWorkItem([this, trajectory_id]() REQUIRES(mutex_) {
    CHECK_EQ(frozen_trajectories_.count(trajectory_id), 0);
    frozen_trajectories_.insert(trajectory_id);
  });
}

void SparsePoseGraph::AddSubmapFromProto(const int trajectory_id,
                                         const transform::Rigid3d& initial_pose,
                                         const mapping::proto::Submap& submap) {
  if (!submap.has_submap_2d()) {
    return;
  }

  std::shared_ptr<const Submap> submap_ptr =
      std::make_shared<const Submap>(submap.submap_2d());
  const transform::Rigid2d initial_pose_2d = transform::Project2D(initial_pose);

  common::MutexLocker locker(&mutex_);
  const mapping::SubmapId submap_id =
      submap_data_.Append(trajectory_id, SubmapData());
  submap_data_.at(submap_id).submap = submap_ptr;
  // Immediately show the submap at the optimized pose.
  CHECK_GE(static_cast<size_t>(submap_data_.num_trajectories()),
           optimized_submap_transforms_.size());
  optimized_submap_transforms_.resize(submap_data_.num_trajectories());
  CHECK_GE(static_cast<size_t>(submap_data_.num_trajectories()),
           num_trimmed_submaps_at_last_optimization_.size());
  num_trimmed_submaps_at_last_optimization_.resize(
      submap_data_.num_trajectories());
  CHECK_EQ(optimized_submap_transforms_.at(trajectory_id).size(),
           submap_id.submap_index);
  optimized_submap_transforms_.at(trajectory_id)
      .push_back(sparse_pose_graph::SubmapData{initial_pose_2d});
  AddWorkItem([this, submap_id, initial_pose_2d]() REQUIRES(mutex_) {
    CHECK_EQ(frozen_trajectories_.count(submap_id.trajectory_id), 1);
    submap_data_.at(submap_id).state = SubmapState::kFinished;
    optimization_problem_.AddSubmap(submap_id.trajectory_id, initial_pose_2d);
  });
}

void SparsePoseGraph::AddTrimmer(
    std::unique_ptr<mapping::PoseGraphTrimmer> trimmer) {
  common::MutexLocker locker(&mutex_);
  // C++11 does not allow us to move a unique_ptr into a lambda.
  mapping::PoseGraphTrimmer* const trimmer_ptr = trimmer.release();
  AddWorkItem([this, trimmer_ptr]()
                  REQUIRES(mutex_) { trimmers_.emplace_back(trimmer_ptr); });
}

void SparsePoseGraph::RunFinalOptimization() {
  WaitForAllComputations();
  optimization_problem_.SetMaxNumIterations(
      options_.max_num_final_iterations());
  RunOptimization();
  optimization_problem_.SetMaxNumIterations(
      options_.optimization_problem_options()
          .ceres_solver_options()
          .max_num_iterations());
}

void SparsePoseGraph::RunOptimization() {
  if (optimization_problem_.submap_data().empty()) {
    return;
  }
  optimization_problem_.Solve(constraints_, frozen_trajectories_); //mnf here run the true optimization
  common::MutexLocker locker(&mutex_);

  std::vector<int> num_trimmed_submaps; //mnf different trajectory_id with different num of trimmed_submaps
  const auto& submap_data = optimization_problem_.submap_data();
  for (int trajectory_id = 0;
       trajectory_id != static_cast<int>(submap_data.size()); ++trajectory_id) {
    num_trimmed_submaps.push_back(
        optimization_problem_.num_trimmed_submaps(trajectory_id));
  }
  //mnf ? optimization_problem_.num_trimmed_submaps AND optimization_problem_.num_trimmed_nodes

  const auto& node_data = optimization_problem_.node_data();
  for (int trajectory_id = 0;
       trajectory_id != static_cast<int>(node_data.size()); ++trajectory_id) {
    int node_data_index = 0;
    const int num_nodes = trajectory_nodes_.num_indices(trajectory_id);
    int node_index = optimization_problem_.num_trimmed_nodes(trajectory_id);
    for (; node_data_index != static_cast<int>(node_data[trajectory_id].size());
         ++node_data_index, ++node_index) {
      const mapping::NodeId node_id{trajectory_id, node_index};
      trajectory_nodes_.at(node_id).pose = transform::Embed3D(
          node_data[trajectory_id][node_data_index].point_cloud_pose);
    } //mnf In SPG --> node_index --> has num_trimmed_nodes
    //    In OPT --> node_data_index --> without num_trimmed_nodes

    // Extrapolate all point cloud poses that were added later.
    const auto local_to_new_global = ComputeLocalToGlobalTransform(
        submap_data, num_trimmed_submaps, trajectory_id);
    const auto local_to_old_global = ComputeLocalToGlobalTransform(
        optimized_submap_transforms_, num_trimmed_submaps_at_last_optimization_,
        trajectory_id);
    const transform::Rigid3d old_global_to_new_global =
        local_to_new_global * local_to_old_global.inverse();
    for (; node_index < num_nodes; ++node_index) {
      const mapping::NodeId node_id{trajectory_id, node_index};
      trajectory_nodes_.at(node_id).pose =
          old_global_to_new_global * trajectory_nodes_.at(node_id).pose;
    }
  }

  //mnf it seems that
  //  node_data_index != static_cast<int>(node_data[trajectory_id].size())  := OPT
  //  node_index < num_nodes  := old_global_to_new_global   (Extrapolate)

  optimized_submap_transforms_ = submap_data;
  num_trimmed_submaps_at_last_optimization_ = num_trimmed_submaps;
  connected_components_ = trajectory_connectivity_.ConnectedComponents();
  reverse_connected_components_.clear();
  for (size_t i = 0; i != connected_components_.size(); ++i) {
    for (const int trajectory_id : connected_components_[i]) {
      reverse_connected_components_.emplace(trajectory_id, i);
    }
  }

  TrimmingHandle trimming_handle(this);
  for (auto& trimmer : trimmers_) {
    trimmer->Trim(&trimming_handle);
  }
}

std::vector<std::vector<mapping::TrajectoryNode>>
SparsePoseGraph::GetTrajectoryNodes() {
  common::MutexLocker locker(&mutex_);
  return trajectory_nodes_.data();
}

std::vector<SparsePoseGraph::Constraint> SparsePoseGraph::constraints() {
  common::MutexLocker locker(&mutex_);
  return constraints_;
}

transform::Rigid3d SparsePoseGraph::GetLocalToGlobalTransform(
    const int trajectory_id) {
  common::MutexLocker locker(&mutex_);
  return ComputeLocalToGlobalTransform(
      optimized_submap_transforms_, num_trimmed_submaps_at_last_optimization_,
      trajectory_id);
}

std::vector<std::vector<int>> SparsePoseGraph::GetConnectedTrajectories() {
  common::MutexLocker locker(&mutex_);
  return connected_components_;
}

int SparsePoseGraph::num_submaps(const int trajectory_id) {
  common::MutexLocker locker(&mutex_);
  if (trajectory_id >= submap_data_.num_trajectories()) {
    return 0;
  }
  return submap_data_.num_indices(trajectory_id);
}

mapping::SparsePoseGraph::SubmapData SparsePoseGraph::GetSubmapData(
    const mapping::SubmapId& submap_id) {
  common::MutexLocker locker(&mutex_);
  return GetSubmapDataUnderLock(submap_id);
}

std::vector<std::vector<mapping::SparsePoseGraph::SubmapData>>
SparsePoseGraph::GetAllSubmapData() {
  common::MutexLocker locker(&mutex_);
  std::vector<std::vector<mapping::SparsePoseGraph::SubmapData>>
      all_submap_data(submap_data_.num_trajectories());
  for (int trajectory_id = 0; trajectory_id < submap_data_.num_trajectories();
       ++trajectory_id) {
    all_submap_data[trajectory_id].reserve(
        submap_data_.num_indices(trajectory_id));
    for (int submap_index = 0;
         submap_index < submap_data_.num_indices(trajectory_id);
         ++submap_index) {
      all_submap_data[trajectory_id].emplace_back(GetSubmapDataUnderLock(
          mapping::SubmapId{trajectory_id, submap_index}));
    }
  }
  return all_submap_data;
}

transform::Rigid3d SparsePoseGraph::ComputeLocalToGlobalTransform(
    const std::vector<std::deque<sparse_pose_graph::SubmapData>>&
        submap_transforms,
    const std::vector<int>& num_trimmed_submaps,
    const int trajectory_id) const {
  if (trajectory_id >= static_cast<int>(submap_transforms.size()) ||
      submap_transforms.at(trajectory_id).empty()) {
    return transform::Rigid3d::Identity();
  }
  const mapping::SubmapId last_optimized_submap_id{
      trajectory_id,
      static_cast<int>(submap_transforms.at(trajectory_id).size() +
                       num_trimmed_submaps.at(trajectory_id) - 1)};
      // mnf num_trimmed_submaps.at(trajectory_id) == 0
  // Accessing 'local_pose' in Submap is okay, since the member is const.
  return transform::Embed3D(submap_transforms.at(trajectory_id).back().pose) *
         submap_data_.at(last_optimized_submap_id)
             .submap->local_pose()
             .inverse();  //mnf L2G = T2G(n-1) * L2T(n-1)
}

mapping::SparsePoseGraph::SubmapData SparsePoseGraph::GetSubmapDataUnderLock(
    const mapping::SubmapId& submap_id) {
  if (submap_data_.at(submap_id).state == SubmapState::kTrimmed) {
    return {};
  }
  auto submap = submap_data_.at(submap_id).submap;
  if (submap_id.trajectory_id <
      static_cast<int>(optimized_submap_transforms_.size())) {
    const size_t submap_data_index =
        submap_id.submap_index -
        num_trimmed_submaps_at_last_optimization_.at(submap_id.trajectory_id);
    if (submap_data_index <
        optimized_submap_transforms_.at(submap_id.trajectory_id).size()) {
      // We already have an optimized pose.
      return {submap, transform::Embed3D(optimized_submap_transforms_
                                             .at(submap_id.trajectory_id)
                                             .at(submap_data_index)
                                             .pose)};
    }
  }
  // We have to extrapolate.
  return {submap, ComputeLocalToGlobalTransform(
                      optimized_submap_transforms_,
                      num_trimmed_submaps_at_last_optimization_,
                      submap_id.trajectory_id) *
                      submap->local_pose()};
}

SparsePoseGraph::TrimmingHandle::TrimmingHandle(SparsePoseGraph* const parent)
    : parent_(parent) {}

int SparsePoseGraph::TrimmingHandle::num_submaps(
    const int trajectory_id) const {
  return parent_->optimization_problem_.submap_data().at(trajectory_id).size() +
         parent_->optimization_problem_.num_trimmed_submaps(trajectory_id);
}

//mnf total_num_submaps == num_submaps_trimmed_ + num_submaps_to_keep_(3)
void SparsePoseGraph::TrimmingHandle::MarkSubmapAsTrimmed(
    const mapping::SubmapId& submap_id) {
  // TODO(hrapp): We have to make sure that the trajectory has been finished
  // if we want to delete the last submaps.
  CHECK(parent_->submap_data_.at(submap_id).state == SubmapState::kFinished);

  // Compile all nodes that are still INTRA_SUBMAP constrained once the submap
  // with 'submap_id' is gone.
  std::set<mapping::NodeId> nodes_to_retain;
  for (const Constraint& constraint : parent_->constraints_) {
    if (constraint.tag == Constraint::Tag::INTRA_SUBMAP &&
        constraint.submap_id != submap_id) {
      nodes_to_retain.insert(constraint.node_id);
    }
  }
  // Remove all 'constraints_' related to 'submap_id'.
  std::set<mapping::NodeId> nodes_to_remove;
  {
    std::vector<Constraint> constraints;
    for (const Constraint& constraint : parent_->constraints_) {
      if (constraint.submap_id == submap_id) {
        if (constraint.tag == Constraint::Tag::INTRA_SUBMAP &&
            nodes_to_retain.count(constraint.node_id) == 0) {
          // This node will no longer be INTRA_SUBMAP contrained and has to be
          // removed.
          nodes_to_remove.insert(constraint.node_id);
        }
      } else {
        constraints.push_back(constraint);
      }
    }
    parent_->constraints_ = std::move(constraints);
  }
  // Remove all 'constraints_' related to 'nodes_to_remove'.
  {
    std::vector<Constraint> constraints;
    for (const Constraint& constraint : parent_->constraints_) {
      if (nodes_to_remove.count(constraint.node_id) == 0) {
        constraints.push_back(constraint);
      }
    }
    parent_->constraints_ = std::move(constraints);
  }

  // Mark the submap with 'submap_id' as trimmed and remove its data.
  auto& submap_data = parent_->submap_data_.at(submap_id);
  CHECK(submap_data.state == SubmapState::kFinished);
  submap_data.state = SubmapState::kTrimmed;
  CHECK(submap_data.submap != nullptr);
  submap_data.submap.reset();
  parent_->constraint_builder_.DeleteScanMatcher(submap_id);
  parent_->optimization_problem_.TrimSubmap(submap_id);

  // Mark the 'nodes_to_remove' as trimmed and remove their data.
  for (const mapping::NodeId& node_id : nodes_to_remove) {
    CHECK(!parent_->trajectory_nodes_.at(node_id).trimmed());
    parent_->trajectory_nodes_.at(node_id).constant_data.reset();
    parent_->optimization_problem_.TrimTrajectoryNode(node_id);
  }
}

}  // namespace mapping_2d
}  // namespace cartographer
