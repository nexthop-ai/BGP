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
 * @file FibProgrammingHolddownTest.cpp
 * @brief Test suite for the classes that backoff FibProgramming
 * in case of back to back failures.
 */

#include <gtest/gtest.h>

using namespace ::testing;

#define FibProgrammingHolddown_TEST_FRIENDS                    \
  FRIEND_TEST(ProgrammingHistory, AddEventTest);               \
  FRIEND_TEST(ProgrammingHistory, MarkProgrammingSuccessTest); \
  FRIEND_TEST(ProgrammingHistory, MarkProgrammingFailTest);    \
  FRIEND_TEST(ProgrammingHistory, GetRecentFailureCountTest);  \
  FRIEND_TEST(HoldDownState, SetHoldDownStateTest);            \
  FRIEND_TEST(HoldDownState, ClearHoldDownStateTest);          \
  FRIEND_TEST(HoldDownState, GetHoldDownStateTest);            \
  FRIEND_TEST(HoldDownState, InvalidNumTest);

#include "neteng/fboss/bgp/cpp/rib/FibProgrammingHolddown.h"

namespace facebook::bgp {

/**
 * Test case for addEvent() method.
 */
TEST(ProgrammingHistory, AddEventTest) {
  // Create an instance of ProgrammingHistory
  ProgrammingHistory history;

  // Add a successful event
  history.addEvent(true);
  EXPECT_EQ(history.history_.size(), 1);
  // Check the contents of the deque
  EXPECT_EQ(history.history_[0], true);

  // Add a failed event
  history.addEvent(false);
  EXPECT_EQ(history.history_.size(), 2);
  // Check the contents of the deque
  EXPECT_EQ(history.history_[0], true);
  EXPECT_EQ(history.history_[1], false);

  // Add another failed event
  history.addEvent(false);
  EXPECT_EQ(history.history_.size(), 3);
  EXPECT_EQ(history.history_[0], true);
  EXPECT_EQ(history.history_[1], false);
  EXPECT_EQ(history.history_[2], false);

  // Add another failed event
  history.addEvent(false);
  EXPECT_EQ(history.history_.size(), 3);
  EXPECT_EQ(history.history_[0], false);
  EXPECT_EQ(history.history_[1], false);
  EXPECT_EQ(history.history_[2], false);

  // Add another failed event
  history.addEvent(false);
  EXPECT_EQ(history.history_.size(), 3);
  EXPECT_EQ(history.history_[0], false);
  EXPECT_EQ(history.history_[1], false);
  EXPECT_EQ(history.history_[2], false);

  // Add another successful event
  history.addEvent(true);
  EXPECT_EQ(history.history_.size(), 3);
  EXPECT_EQ(history.history_[0], false);
  EXPECT_EQ(history.history_[1], false);
  EXPECT_EQ(history.history_[2], true);
}

/**
 * Test case for markProgrammingSuccess() method.
 */
TEST(ProgrammingHistory, MarkProgrammingSuccessTest) {
  // Create an instance of ProgrammingHistory
  ProgrammingHistory history;

  // Mark a successful programming event
  history.markProgrammingSuccess();
  EXPECT_EQ(history.history_.size(), 1);
  // Check the contents of the deque
  EXPECT_EQ(history.history_[0], true);

  // Mark another successful programming event
  history.markProgrammingSuccess();
  EXPECT_EQ(history.history_.size(), 2);
  // Check the contents of the deque
  EXPECT_EQ(history.history_[0], true);
  EXPECT_EQ(history.history_[1], true);

  // Mark another successful programming event
  history.markProgrammingSuccess();
  EXPECT_EQ(history.history_.size(), 3);
  // Check the contents of the deque
  EXPECT_EQ(history.history_[0], true);
  EXPECT_EQ(history.history_[1], true);
  EXPECT_EQ(history.history_[2], true);

  // Mark another successful programming event
  history.markProgrammingSuccess();
  EXPECT_EQ(history.history_.size(), 3);
  // Check the contents of the deque
  EXPECT_EQ(history.history_[0], true);
  EXPECT_EQ(history.history_[1], true);
  EXPECT_EQ(history.history_[2], true);
}

/**
 * Test case for markProgrammingFail() method.
 */
TEST(ProgrammingHistory, MarkProgrammingFailTest) {
  // Create an instance of ProgrammingHistory
  ProgrammingHistory history;

  // Mark a failed programming event
  history.markProgrammingFail();
  EXPECT_EQ(history.history_.size(), 1);
  // Check the contents of the deque
  EXPECT_EQ(history.history_[0], false);

  // Mark another failed programming event
  history.markProgrammingFail();
  EXPECT_EQ(history.history_.size(), 2);
  // Check the contents of the deque
  EXPECT_EQ(history.history_[0], false);
  EXPECT_EQ(history.history_[1], false);

  // Mark another failed programming event
  history.markProgrammingFail();
  EXPECT_EQ(history.history_.size(), 3);
  // Check the contents of the deque
  EXPECT_EQ(history.history_[0], false);
  EXPECT_EQ(history.history_[1], false);
  EXPECT_EQ(history.history_[2], false);

  // Mark another failed programming event
  history.markProgrammingFail();
  EXPECT_EQ(history.history_.size(), 3);
  // Check the contents of the deque
  EXPECT_EQ(history.history_[0], false);
  EXPECT_EQ(history.history_[1], false);
  EXPECT_EQ(history.history_[2], false);
}

/**
 * Test case for getRecentFailureCount() method.
 */
TEST(ProgrammingHistory, GetRecentFailureCountTest) {
  // Create an instance of ProgrammingHistory
  ProgrammingHistory history;

  history.markProgrammingFail();
  EXPECT_EQ(history.getRecentFailureCount(), 1);
  history.markProgrammingFail();
  EXPECT_EQ(history.getRecentFailureCount(), 2);
  history.markProgrammingFail();
  EXPECT_EQ(history.getRecentFailureCount(), 3);
  history.markProgrammingFail();
  EXPECT_EQ(history.getRecentFailureCount(), 3);

  history.markProgrammingSuccess();
  EXPECT_EQ(history.getRecentFailureCount(), 0);

  history.markProgrammingFail();
  EXPECT_EQ(history.getRecentFailureCount(), 1);
  history.markProgrammingFail();
  EXPECT_EQ(history.getRecentFailureCount(), 2);
  history.markProgrammingFail();
  EXPECT_EQ(history.getRecentFailureCount(), 3);
  history.markProgrammingFail();
  EXPECT_EQ(history.getRecentFailureCount(), 3);

  history.markProgrammingSuccess();
  EXPECT_EQ(history.getRecentFailureCount(), 0);
}
/**
 * Test case for setHoldDownState() method.
 */
TEST(HoldDownState, SetHoldDownStateTest) {
  // Test default state of holdDownState
  {
    HoldDownState holdDownState;
    EXPECT_FALSE(holdDownState.getHoldDownState());
    EXPECT_EQ(0, holdDownState.ticksLeft);
  }

  // Test setting hold down state to 0
  {
    HoldDownState holdDownState;
    EXPECT_FALSE(holdDownState.setHoldDownState(0));
    EXPECT_FALSE(holdDownState.getHoldDownState());
    EXPECT_EQ(0, holdDownState.ticksLeft);
  }

  // Test setting hold down state to 1
  {
    HoldDownState holdDownState;
    EXPECT_TRUE(holdDownState.setHoldDownState(1));
    EXPECT_TRUE(holdDownState.getHoldDownState());
    EXPECT_EQ(5, holdDownState.ticksLeft);
  }

  // Test setting hold down state to 2
  {
    HoldDownState holdDownState;
    EXPECT_TRUE(holdDownState.setHoldDownState(2));
    EXPECT_TRUE(holdDownState.getHoldDownState());
    EXPECT_EQ(10, holdDownState.ticksLeft);
  }

  // Test setting hold down state to 3
  {
    HoldDownState holdDownState;
    EXPECT_TRUE(holdDownState.setHoldDownState(3));
    EXPECT_TRUE(holdDownState.getHoldDownState());
    EXPECT_EQ(15, holdDownState.ticksLeft);
  }

  // Test setting hold down state to 13 (ticks are capped at 15)
  {
    HoldDownState holdDownState;
    EXPECT_TRUE(holdDownState.setHoldDownState(13));
    EXPECT_TRUE(holdDownState.getHoldDownState());
    EXPECT_EQ(15, holdDownState.ticksLeft);
  }

  // Test setting hold down state multiple times
  {
    HoldDownState holdDownState;
    EXPECT_FALSE(holdDownState.setHoldDownState(0));
    EXPECT_FALSE(holdDownState.getHoldDownState());
    EXPECT_EQ(0, holdDownState.ticksLeft);
    EXPECT_TRUE(holdDownState.setHoldDownState(1));
    EXPECT_TRUE(holdDownState.getHoldDownState());
    EXPECT_EQ(5, holdDownState.ticksLeft);
    EXPECT_TRUE(holdDownState.setHoldDownState(2));
    EXPECT_TRUE(holdDownState.getHoldDownState());
    EXPECT_EQ(10, holdDownState.ticksLeft);
    EXPECT_TRUE(holdDownState.setHoldDownState(3));
    EXPECT_TRUE(holdDownState.getHoldDownState());
    EXPECT_EQ(15, holdDownState.ticksLeft);
  }
}

/**
 * Test case for clearHoldDownState() method.
 */
TEST(HoldDownState, ClearHoldDownStateTest) {
  // Test default state of holdDownState
  {
    HoldDownState holdDownState;
    EXPECT_FALSE(holdDownState.getHoldDownState());
    EXPECT_TRUE(holdDownState.clearHoldDownState());
  }

  // Test clearing hold down state after incorrect setting
  {
    HoldDownState holdDownState;
    EXPECT_FALSE(holdDownState.setHoldDownState(0));
    EXPECT_TRUE(holdDownState.clearHoldDownState());
  }

  // Test clearing hold down state after setting it
  {
    HoldDownState holdDownState;
    EXPECT_TRUE(holdDownState.setHoldDownState(1));
    for (int i = 0; i < 4; i++) {
      EXPECT_FALSE(holdDownState.clearHoldDownState());
    }
    EXPECT_TRUE(holdDownState.clearHoldDownState());
  }
}

} // namespace facebook::bgp
