
// Copyright 2023 TIER IV, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef AUTOWARE__BEHAVIOR_PATH_PLANNER_COMMON__INTERFACE__SCENE_MODULE_MANAGER_INTERFACE_HPP_
#define AUTOWARE__BEHAVIOR_PATH_PLANNER_COMMON__INTERFACE__SCENE_MODULE_MANAGER_INTERFACE_HPP_

#include "autoware/behavior_path_planner_common/interface/scene_module_interface.hpp"
#include "autoware/universe_utils/ros/parameter.hpp"

#include <rclcpp/node.hpp>
#include <rclcpp/parameter.hpp>
#include <rclcpp/publisher.hpp>

#include <autoware_adapi_v1_msgs/msg/steering_factor_array.hpp>
#include <autoware_adapi_v1_msgs/msg/velocity_factor_array.hpp>
#include <unique_identifier_msgs/msg/uuid.hpp>

#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace autoware::behavior_path_planner
{

using autoware::motion_utils::createDeadLineVirtualWallMarker;
using autoware::motion_utils::createSlowDownVirtualWallMarker;
using autoware::motion_utils::createStopVirtualWallMarker;
using autoware::universe_utils::toHexString;
using autoware_adapi_v1_msgs::msg::SteeringFactorArray;
using autoware_adapi_v1_msgs::msg::VelocityFactorArray;
using unique_identifier_msgs::msg::UUID;
using SceneModulePtr = std::shared_ptr<SceneModuleInterface>;
using SceneModuleObserver = std::weak_ptr<SceneModuleInterface>;

class SceneModuleManagerInterface
{
public:
  SceneModuleManagerInterface(const SceneModuleManagerInterface &) = delete;
  SceneModuleManagerInterface(SceneModuleManagerInterface &&) = delete;
  SceneModuleManagerInterface & operator=(const SceneModuleManagerInterface &) = delete;
  SceneModuleManagerInterface & operator=(SceneModuleManagerInterface &&) = delete;
  explicit SceneModuleManagerInterface(std::string name) : name_{std::move(name)} {}

  virtual ~SceneModuleManagerInterface() = default;

  virtual void init(rclcpp::Node * node) = 0;

  void updateIdleModuleInstance();

  bool isExecutionRequested(const BehaviorModuleOutput & previous_module_output) const
  {
    idle_module_ptr_->setData(planner_data_);
    idle_module_ptr_->setPreviousModuleOutput(previous_module_output);
    idle_module_ptr_->updateData();

    return idle_module_ptr_->isExecutionRequested();
  }

  void registerNewModule(
    const SceneModuleObserver & observer, const BehaviorModuleOutput & previous_module_output)
  {
    if (observer.expired()) {
      return;
    }

    observer.lock()->setData(planner_data_);
    observer.lock()->setPreviousModuleOutput(previous_module_output);
    observer.lock()->getTimeKeeper()->add_reporter(this->pub_processing_time_);
    observer.lock()->onEntry();

    observers_.push_back(observer);
  }

  void updateObserver()
  {
    const auto itr = std::remove_if(
      observers_.begin(), observers_.end(),
      [](const auto & observer) { return observer.expired(); });

    observers_.erase(itr, observers_.end());
  }

  void publishRTCStatus()
  {
    for (const auto & [module_name, ptr] : rtc_interface_ptr_map_) {
      if (ptr) {
        ptr->removeExpiredCooperateStatus();
        ptr->publishCooperateStatus(rclcpp::Clock(RCL_ROS_TIME).now());
      }
    }
  }

  void publishSteeringFactor()
  {
    SteeringFactorArray steering_factor_array;
    steering_factor_array.header.frame_id = "map";
    steering_factor_array.header.stamp = node_->now();

    for (const auto & m : observers_) {
      if (m.expired()) {
        continue;
      }

      const auto steering_factor = m.lock()->get_steering_factor();
      if (steering_factor.behavior != PlanningBehavior::UNKNOWN) {
        steering_factor_array.factors.emplace_back(steering_factor);
      }
    }

    pub_steering_factors_->publish(steering_factor_array);
  }

  void publishVelocityFactor()
  {
    VelocityFactorArray velocity_factor_array;
    velocity_factor_array.header.frame_id = "map";
    velocity_factor_array.header.stamp = node_->now();

    for (const auto & m : observers_) {
      if (m.expired()) {
        continue;
      }

      const auto velocity_factor = m.lock()->get_velocity_factor();
      if (velocity_factor.behavior != PlanningBehavior::UNKNOWN) {
        velocity_factor_array.factors.emplace_back(velocity_factor);
      }
    }

    pub_velocity_factors_->publish(velocity_factor_array);
  }

  void publishVirtualWall() const
  {
    using autoware::universe_utils::appendMarkerArray;

    MarkerArray markers{};

    const auto marker_offset = std::numeric_limits<uint8_t>::max();

    uint32_t marker_id = marker_offset;
    for (const auto & m : observers_) {
      if (m.expired()) {
        continue;
      }

      const auto opt_stop_pose = m.lock()->getStopPose();
      if (!!opt_stop_pose) {
        const auto virtual_wall = createStopVirtualWallMarker(
          opt_stop_pose.value(), m.lock()->name(), rclcpp::Clock().now(), marker_id);
        appendMarkerArray(virtual_wall, &markers);
      }

      const auto opt_slow_pose = m.lock()->getSlowPose();
      if (!!opt_slow_pose) {
        const auto virtual_wall = createSlowDownVirtualWallMarker(
          opt_slow_pose.value(), m.lock()->name(), rclcpp::Clock().now(), marker_id);
        appendMarkerArray(virtual_wall, &markers);
      }

      const auto opt_dead_pose = m.lock()->getDeadPose();
      if (!!opt_dead_pose) {
        const auto virtual_wall = createDeadLineVirtualWallMarker(
          opt_dead_pose.value(), m.lock()->name(), rclcpp::Clock().now(), marker_id);
        appendMarkerArray(virtual_wall, &markers);
      }

      const auto module_specific_wall = m.lock()->getModuleVirtualWall();
      appendMarkerArray(module_specific_wall, &markers);

      m.lock()->resetWallPoses();
    }

    pub_virtual_wall_->publish(markers);
  }

  void publishMarker() const
  {
    using autoware::universe_utils::appendMarkerArray;

    MarkerArray info_markers{};
    MarkerArray debug_markers{};
    MarkerArray drivable_lanes_markers{};

    const auto marker_offset = std::numeric_limits<uint8_t>::max();

    uint32_t marker_id = marker_offset;
    for (const auto & m : observers_) {
      if (m.expired()) {
        continue;
      }

      for (auto & marker : m.lock()->getInfoMarkers().markers) {
        marker.id += marker_id;
        info_markers.markers.push_back(marker);
      }

      for (auto & marker : m.lock()->getDebugMarkers().markers) {
        marker.id += marker_id;
        debug_markers.markers.push_back(marker);
      }

      for (auto & marker : m.lock()->getDrivableLanesMarkers().markers) {
        marker.id += marker_id;
        drivable_lanes_markers.markers.push_back(marker);
      }

      marker_id += marker_offset;
    }

    if (observers_.empty() && idle_module_ptr_ != nullptr) {
      appendMarkerArray(idle_module_ptr_->getInfoMarkers(), &info_markers);
      appendMarkerArray(idle_module_ptr_->getDebugMarkers(), &debug_markers);
      appendMarkerArray(idle_module_ptr_->getDrivableLanesMarkers(), &drivable_lanes_markers);
    }

    pub_info_marker_->publish(info_markers);
    pub_debug_marker_->publish(debug_markers);
    pub_drivable_lanes_->publish(drivable_lanes_markers);
  }

  bool exist(const SceneModulePtr & module_ptr) const
  {
    return std::any_of(observers_.begin(), observers_.end(), [&](const auto & observer) {
      return !observer.expired() && observer.lock() == module_ptr;
    });
  }

  /**
   * Determine if a new module can be launched. It ensures that only one instance of a particular
   * scene module type is registered at a time.
   *
   * When this returns true:
   * - A new instance of the scene module can be launched.
   * - No other instance of the same name of scene module is currently active or registered.
   *
   */
  bool canLaunchNewModule() const { return observers_.empty(); }

  virtual bool isSimultaneousExecutableAsApprovedModule() const
  {
    return config_.enable_simultaneous_execution_as_approved_module;
  }

  virtual bool isSimultaneousExecutableAsCandidateModule() const
  {
    return config_.enable_simultaneous_execution_as_candidate_module;
  }

  void setData(const std::shared_ptr<PlannerData> & planner_data) { planner_data_ = planner_data; }

  void reset()
  {
    std::for_each(observers_.begin(), observers_.end(), [](const auto & observer) {
      if (!observer.expired()) {
        observer.lock()->onExit();
      }
    });

    observers_.clear();

    if (idle_module_ptr_ != nullptr) {
      idle_module_ptr_->onExit();
      idle_module_ptr_.reset();
    }

    pub_debug_marker_->publish(MarkerArray{});
  }

  std::string name() const { return name_; }

  std::vector<SceneModuleObserver> getSceneModuleObservers() { return observers_; }

  std::shared_ptr<SceneModuleInterface> getIdleModule() { return std::move(idle_module_ptr_); }

  virtual void updateModuleParams(const std::vector<rclcpp::Parameter> & parameters) = 0;

  void initInterface(rclcpp::Node * node, const std::vector<std::string> & rtc_types);

protected:
  virtual std::unique_ptr<SceneModuleInterface> createNewSceneModuleInstance() = 0;

  rclcpp::Node * node_ = nullptr;

  rclcpp::Publisher<MarkerArray>::SharedPtr pub_info_marker_;

  rclcpp::Publisher<MarkerArray>::SharedPtr pub_debug_marker_;

  rclcpp::Publisher<MarkerArray>::SharedPtr pub_virtual_wall_;

  rclcpp::Publisher<MarkerArray>::SharedPtr pub_drivable_lanes_;

  rclcpp::Publisher<SteeringFactorArray>::SharedPtr pub_steering_factors_;

  rclcpp::Publisher<VelocityFactorArray>::SharedPtr pub_velocity_factors_;

  rclcpp::Publisher<universe_utils::ProcessingTimeDetail>::SharedPtr pub_processing_time_;

  std::string name_;

  std::shared_ptr<PlannerData> planner_data_;

  std::vector<SceneModuleObserver> observers_;

  std::unique_ptr<SceneModuleInterface> idle_module_ptr_;

  std::unordered_map<std::string, std::shared_ptr<RTCInterface>> rtc_interface_ptr_map_;

  std::unordered_map<std::string, std::shared_ptr<ObjectsOfInterestMarkerInterface>>
    objects_of_interest_marker_interface_ptr_map_;

  ModuleConfigParameters config_;
};

}  // namespace autoware::behavior_path_planner

#endif  // AUTOWARE__BEHAVIOR_PATH_PLANNER_COMMON__INTERFACE__SCENE_MODULE_MANAGER_INTERFACE_HPP_
