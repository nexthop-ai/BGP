/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

namespace facebook::bgp {

class MonitorableTrace {
 public:
  bool isMonitored() {
    return monitored_;
  }

  void markMonitored() {
    monitored_ = true;
  }

 private:
  bool monitored_{false};
};

/*
 * The trace to ensure that each direction of a queue is monitored once
 */
class MonitorableQueueTrace {
 public:
  // the direction with respect to the module which monitors the queue
  enum Direction {
    IN = 0,
    OUT = 1,
    INTERNAL = 2,
  };

  bool isMonitored(Direction direction) {
    if (direction == Direction::INTERNAL) {
      return monitored_[0] || monitored_[1];
    }
    return monitored_[direction];
  }

  void markMonitored(Direction direction) {
    if (direction == Direction::INTERNAL) {
      monitored_[0] = true;
      monitored_[1] = true;
    } else {
      monitored_[direction] = true;
    }
  }

 private:
  bool monitored_[2]{false, false};
};

} // namespace facebook::bgp
