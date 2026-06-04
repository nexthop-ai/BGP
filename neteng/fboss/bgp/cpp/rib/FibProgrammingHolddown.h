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
 * @file FibProgrammingHolddown.h
 * @brief Class declarations that backoff FibProgramming in case of back to back
 * failures.
 */

#pragma once

#include <cstdint>
#include <deque>

namespace facebook::bgp {

/**
 * @class ProgrammingHistory
 * @brief Manages a history of Fib Programming.
 */
class ProgrammingHistory {
 public:
  /**
   * Marks a successful programming event.
   */
  virtual void markProgrammingSuccess();

  /**
   * Marks a failed programming event.
   */
  virtual void markProgrammingFail();

  /**
   * Returns the count of recent back to back FIB programming failures.
   *
   * @return The number of back to back failures.
   */
  virtual uint32_t getRecentFailureCount() const;

  /**
   * Destructor for the ProgrammingHistory class.
   */
  virtual ~ProgrammingHistory() {}

 private:
  /**
   * Adds an event to the programming history.
   *
   * If the history is full, the oldest event is removed before adding the new
   * one.
   *
   * @param isSuccess Whether the programming was successful or not.
   */
  void addEvent(bool isSuccess);

  static constexpr int max_tracked_events_ = 3; ///< Maximum tracked events.

  std::deque<bool> history_; ///< History of programming events.

#ifdef FibProgrammingHolddown_TEST_FRIENDS
  FibProgrammingHolddown_TEST_FRIENDS
#endif
};

/**
 * @class HoldDownState
 * @brief Manages FIB programming hold down state.
 */
class HoldDownState {
 public:
  /**
   * Constructor to initialize the hold down state.
   *
   * Initializes isInHoldDown to false and ticksLeft to 0.
   */
  HoldDownState() : isInHoldDown(false), ticksLeft(0) {}

  /**
   * Clears the hold down state by decrementing the ticks left.
   * If ticks left reaches 0, sets the hold down state to false.
   *
   * @return True if the hold down state is cleared, false otherwise.
   */
  virtual bool clearHoldDownState();

  /**
   * Sets the hold down state to true and initializes the ticks left.
   *
   * @param numFailures The count of most recent FIB programming failures.
   *
   * @return True if the hold down state was set successfully, false otherwise.
   */
  virtual bool setHoldDownState(uint32_t numFailures);

  /**
   * Gets the current hold down state.
   *
   * @return The current hold down state.
   */
  bool getHoldDownState();

  /**
   * Destructor for the HoldDownState class.
   */
  virtual ~HoldDownState() {}

 private:
  bool isInHoldDown; ///< Flag indicating whether we're in hold down state.
  uint32_t ticksLeft; ///< Number of ticks left in the hold down state.

  ///
  const uint32_t kLowBackoffTicks = 5; ///< Lowest umber of ticks to back-off.
  const uint32_t kMediumBackoffTicks =
      10; ///< Medium number of ticks to back-off.

  const uint32_t kHighBackoffTicks = 15; ///< High number of ticks to back-off.

#ifdef FibProgrammingHolddown_TEST_FRIENDS
  FibProgrammingHolddown_TEST_FRIENDS
#endif
#ifdef FibFboss_TEST_FRIENDS
      FibFboss_TEST_FRIENDS
#endif

      friend class FibEbbFixture;
};

} // namespace facebook::bgp
