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

/**
 * @file FibProgrammingHolddown.cpp
 * @brief This file contains implementation of classes that backoff
 * FibProgramming in case of back to backfailures.
 */

#include "neteng/fboss/bgp/cpp/rib/FibProgrammingHolddown.h"
#include <folly/logging/xlog.h>

namespace facebook::bgp {

/**
 * Adds an event to the programming history.
 *
 * If the history is full, the oldest event is removed before adding the new
 * one.
 *
 * @param isSuccess Whether the programming was successful or not.
 */
void ProgrammingHistory::addEvent(bool isSuccess) {
  if (history_.size() >= max_tracked_events_) {
    history_.pop_front();
  }
  history_.push_back(isSuccess);
}

/**
 * Marks a successful programming event in the history.
 */
void ProgrammingHistory::markProgrammingSuccess() {
  addEvent(true);
}

/**
 * Marks a failed programming event in the history.
 */
void ProgrammingHistory::markProgrammingFail() {
  addEvent(false);
}

/**
 * Returns the count of recent back to back FIB programming failures.
 *
 * @return The number of back to back failures.
 */
uint32_t ProgrammingHistory::getRecentFailureCount() const {
  uint32_t failures = 0;
  for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
    if (!*it) {
      failures++;
    } else {
      break;
    }
  }
  return failures;
}

/**
 * Clears the hold down state by decrementing the ticks left.
 * If ticks left reaches 0, sets the hold down state to false.
 *
 * @return True if the hold down state is cleared, false otherwise.
 */
bool HoldDownState::clearHoldDownState() {
  if (isInHoldDown) {
    ticksLeft--;
    if (ticksLeft == 0) {
      isInHoldDown = false;
    } else {
      XLOGF(INFO, "Hold-down active. Ticks left: {}", ticksLeft);
    }
  }
  return !isInHoldDown;
}

/**
 * Sets the hold down state to true and initializes the ticks left.
 *
 * @param numFailures The count of most recent FIB programming failures.
 *
 * @return True if the hold down state was set successfully, false otherwise.
 */
bool HoldDownState::setHoldDownState(uint32_t numFailures) {
  if (numFailures == 0) {
    XLOG(
        ERR,
        "Holddown state can NOT be set without any prior FIB programming failures");
    return false;
  }
  isInHoldDown = true;
  switch (numFailures) {
    case 1:
      ticksLeft = kLowBackoffTicks;
      break;
    case 2:
      ticksLeft = kMediumBackoffTicks;
      break;
    default:
      ticksLeft = kHighBackoffTicks;
      break;
  }
  return true;
}

/**
 * Gets the current hold down state.
 *
 * @return The current hold down state.
 */
bool HoldDownState::getHoldDownState() {
  return isInHoldDown;
}
} // namespace facebook::bgp
