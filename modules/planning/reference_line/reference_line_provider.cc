/******************************************************************************
 * Copyright 2017 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

/**
 * @file
 *
 * @brief Implementation of the class ReferenceLineProvider.
 */

#include <algorithm>
#include <utility>

#include "modules/map/pnc_map/path.h"
#include "modules/planning/common/planning_gflags.h"
#include "modules/planning/reference_line/reference_line_provider.h"
#include "modules/routing/common/routing_gflags.h"

/**
 * @namespace apollo::planning
 * @brief apollo::planning
 */
namespace apollo {
namespace planning {

using apollo::common::VehicleState;
using apollo::common::math::Vec2d;
using apollo::hdmap::RouteSegments;
using apollo::hdmap::LaneWaypoint;

ReferenceLineProvider::ReferenceLineProvider() {}

ReferenceLineProvider::~ReferenceLineProvider() {
  if (thread_ && thread_->joinable()) {
    thread_->join();
  }
}

void ReferenceLineProvider::Init(
    const hdmap::HDMap *hdmap_,
    const QpSplineReferenceLineSmootherConfig &smoother_config) {
  pnc_map_.reset(new hdmap::PncMap(hdmap_));
  smoother_config_ = smoother_config;
  segment_history_.clear();
  std::vector<double> init_t_knots;
  spline_solver_.reset(new Spline2dSolver(init_t_knots, 1));
  if (FLAGS_enable_spiral_reference_line) {
    smoother_.reset(
        new SpiralReferenceLineSmoother(FLAGS_spiral_smoother_max_deviation));
  } else {
    smoother_.reset(new QpSplineReferenceLineSmoother(smoother_config_,
                                                      spline_solver_.get()));
  }
  is_initialized_ = true;
}

bool ReferenceLineProvider::IsAllowChangeLane(
    const common::math::Vec2d &point,
    const std::list<RouteSegments> &route_segments) {
  if (FLAGS_reckless_change_lane) {
    ADEBUG << "enabled reckless change lane enabled";
    return true;
  }
  if (route_segments.size() <= 1) {
    return false;
  }
  auto forward_segment = route_segments.begin();
  while (forward_segment != route_segments.end() &&
         !forward_segment->IsOnSegment()) {
    ++forward_segment;
  }
  if (forward_segment == route_segments.end()) {
    return true;
  }
  double s = 0.0;
  double l = 0.0;
  LaneWaypoint waypoint;
  if (!forward_segment->GetProjection(point, &s, &l, &waypoint)) {
    AERROR << "Failed to project to forward segment from point: "
           << point.DebugString();
    return false;
  }
  auto history_iter = segment_history_.find(forward_segment->Id());
  if (history_iter == segment_history_.end()) {
    auto &inserter = segment_history_[forward_segment->Id()];
    inserter.min_l = std::fabs(l);
    inserter.last_point = point;
    inserter.accumulate_s = 0.0;
    return false;
  } else {
    history_iter->second.min_l =
        std::min(history_iter->second.min_l, std::fabs(l));
    double dist =
        common::util::DistanceXY(history_iter->second.last_point, point);
    history_iter->second.last_point = point;
    history_iter->second.accumulate_s += dist;
    constexpr double kChangeLaneMinL = 0.25;
    constexpr double kChangeLaneMinLengthFactor = 0.6;
    if (history_iter->second.min_l < kChangeLaneMinL &&
        history_iter->second.accumulate_s >=
            kChangeLaneMinLengthFactor * FLAGS_min_length_for_lane_change) {
      return true;
    }
  }
  return false;
}

bool ReferenceLineProvider::UpdateRoutingResponse(
    const routing::RoutingResponse &routing) {
  std::lock_guard<std::mutex> lock(pnc_map_mutex_);
  if (!pnc_map_->UpdateRoutingResponse(routing)) {
    AERROR << "Failed to update routing in pnc map";
    return false;
  }
  if (!pnc_map_->IsSameRouting()) {
    segment_history_.clear();
  }
  has_routing_ = true;
  return true;
}

void ReferenceLineProvider::UpdateVehicleState(
    const VehicleState &vehicle_state) {
  std::lock_guard<std::mutex> lock(pnc_map_mutex_);
  vehicle_state_ = vehicle_state;
}

bool ReferenceLineProvider::Start() {
  if (!is_initialized_) {
    AERROR << "ReferenceLineProvider has NOT been initiated.";
    return false;
  }
  if (FLAGS_enable_reference_line_provider_thread) {
    thread_.reset(
        new std::thread(&ReferenceLineProvider::GenerateThread, this));
  }
  return true;
}

void ReferenceLineProvider::Stop() {
  is_stop_ = true;
  if (FLAGS_enable_reference_line_provider_thread && thread_ &&
      thread_->joinable()) {
    thread_->join();
  }
}

void ReferenceLineProvider::GenerateThread() {
  constexpr int32_t kSleepTime = 200;  // milliseconds
  while (!is_stop_) {
    std::this_thread::sleep_for(
        std::chrono::duration<double, std::milli>(kSleepTime));
    if (!has_routing_) {
      AERROR << "Routing is not ready.";
      continue;
    }
    std::list<ReferenceLine> reference_lines;
    std::list<hdmap::RouteSegments> segments;
    if (!CreateReferenceLineFromRouting(&reference_lines, &segments)) {
      AERROR << "Fail to get reference line";
      continue;
    }
    std::unique_lock<std::mutex> lock(reference_lines_mutex__);
    reference_lines_ = reference_lines;
    route_segments_ = segments;
    lock.unlock();
    cv_has_reference_line_.notify_one();
  }
}

bool ReferenceLineProvider::GetReferenceLines(
    std::list<ReferenceLine> *reference_lines,
    std::list<hdmap::RouteSegments> *segments) {
  CHECK_NOTNULL(reference_lines);
  CHECK_NOTNULL(segments);
  if (FLAGS_enable_reference_line_provider_thread) {
    std::unique_lock<std::mutex> lock(reference_lines_mutex__);
    cv_has_reference_line_.wait(lock,
                                [this]() { return !reference_lines_.empty(); });
    reference_lines->assign(reference_lines_.begin(), reference_lines_.end());
    segments->assign(route_segments_.begin(), route_segments_.end());
    lock.unlock();
    return true;
  } else {
    return CreateReferenceLineFromRouting(reference_lines, segments);
  }
}

void ReferenceLineProvider::PrioritzeChangeLane(
    std::list<hdmap::RouteSegments> *route_segments) {
  CHECK_NOTNULL(route_segments);
  auto iter = route_segments->begin();
  while (iter != route_segments->end()) {
    if (!iter->IsOnSegment()) {
      route_segments->splice(route_segments->begin(), *route_segments, iter);
      break;
    }
    ++iter;
  }
}

bool ReferenceLineProvider::CreateReferenceLineFromRouting(
    std::list<ReferenceLine> *reference_lines,
    std::list<hdmap::RouteSegments> *segments) {
  std::list<hdmap::RouteSegments> route_segments;
  double look_forward_distance =
      (vehicle_state_.linear_velocity() * FLAGS_look_forward_time_sec >
       FLAGS_look_forward_min_distance)
          ? FLAGS_look_forward_distance
          : FLAGS_look_forward_min_distance;
  common::math::Vec2d point;
  {
    std::lock_guard<std::mutex> lock(pnc_map_mutex_);
    point.set_x(vehicle_state_.x());
    point.set_y(vehicle_state_.y());
    if (!pnc_map_->GetRouteSegments(vehicle_state_,
                                    FLAGS_look_backward_distance,
                                    look_forward_distance, &route_segments)) {
      AERROR << "Failed to extract segments from routing";
      return false;
    }
  }
  if (FLAGS_prioritize_change_lane) {
    PrioritzeChangeLane(&route_segments);
  }
  bool is_allow_change_lane = IsAllowChangeLane(point, route_segments);
  for (const auto &lanes : route_segments) {
    if (!is_allow_change_lane && !lanes.IsOnSegment()) {
      continue;
    }
    ReferenceLine reference_line;
    if (!SmoothReferenceLine(lanes, &reference_line)) {
      AERROR << "Failed to smooth reference line";
      continue;
    }
    reference_lines->emplace_back(reference_line);
    segments->emplace_back(lanes);
  }

  if (reference_lines->empty()) {
    AERROR << "No smooth reference lines available";
    return false;
  }

  return true;
}

bool ReferenceLineProvider::IsReferenceLineSmoothValid(
    const ReferenceLine &raw, const ReferenceLine &smoothed) const {
  const double kReferenceLineDiffCheckResolution = 5.0;
  for (int s = 0.0; s < raw.Length(); s += kReferenceLineDiffCheckResolution) {
    auto xy_old = raw.GetReferencePoint(s);
    auto xy_new = smoothed.GetReferencePoint(s);
    const double diff = xy_old.DistanceTo(xy_new);
    if (diff > kReferenceLineDiffCheckResolution) {
      AERROR << "Fail to provide reference line because too large diff "
                "between smoothed and raw reference lines. diff: "
             << diff;
      return false;
    }
  }
  return true;
}

bool ReferenceLineProvider::SmoothReferenceLine(
    const hdmap::RouteSegments &lanes, ReferenceLine *reference_line) {
  hdmap::Path path;
  hdmap::PncMap::CreatePathFromLaneSegments(lanes, &path);
  if (!FLAGS_enable_smooth_reference_line) {
    *reference_line = ReferenceLine(path);
    return true;
  }
  ReferenceLine raw_reference_line(path);
  if (!smoother_->Smooth(raw_reference_line, reference_line)) {
    AERROR << "Failed to smooth reference line";
    return false;
  }
  if (!IsReferenceLineSmoothValid(raw_reference_line, *reference_line)) {
    AERROR << "The smoothed reference line error is too large";
    return false;
  }
  return true;
}
}  // namespace planning
}  // namespace apollo
