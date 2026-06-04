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

#include <gtest/gtest.h>

using namespace ::testing;

#include "neteng/fboss/bgp/cpp/watchdog/MonitorableTrace.h"

namespace facebook::bgp {

TEST(MonitorableTraceTest, IsAndMarkMonitoredTest) {
  MonitorableTrace testTrace;

  EXPECT_FALSE(testTrace.isMonitored());

  testTrace.markMonitored();

  EXPECT_TRUE(testTrace.isMonitored());
}

TEST(MonitorableTraceTest, IsAndMarkMonitoredQueueInTest) {
  MonitorableQueueTrace testTrace;

  EXPECT_FALSE(testTrace.isMonitored(MonitorableQueueTrace::Direction::IN));
  EXPECT_FALSE(testTrace.isMonitored(MonitorableQueueTrace::Direction::OUT));
  EXPECT_FALSE(
      testTrace.isMonitored(MonitorableQueueTrace::Direction::INTERNAL));

  testTrace.markMonitored(MonitorableQueueTrace::Direction::IN);

  EXPECT_TRUE(testTrace.isMonitored(MonitorableQueueTrace::Direction::IN));
  EXPECT_FALSE(testTrace.isMonitored(MonitorableQueueTrace::Direction::OUT));
  EXPECT_TRUE(
      testTrace.isMonitored(MonitorableQueueTrace::Direction::INTERNAL));
}

TEST(MonitorableTraceTest, IsAndMarkMonitoredQueueOutTest) {
  MonitorableQueueTrace testTrace;

  EXPECT_FALSE(testTrace.isMonitored(MonitorableQueueTrace::Direction::OUT));
  EXPECT_FALSE(testTrace.isMonitored(MonitorableQueueTrace::Direction::IN));
  EXPECT_FALSE(
      testTrace.isMonitored(MonitorableQueueTrace::Direction::INTERNAL));

  testTrace.markMonitored(MonitorableQueueTrace::Direction::OUT);

  EXPECT_TRUE(testTrace.isMonitored(MonitorableQueueTrace::Direction::OUT));
  EXPECT_FALSE(testTrace.isMonitored(MonitorableQueueTrace::Direction::IN));
  EXPECT_TRUE(
      testTrace.isMonitored(MonitorableQueueTrace::Direction::INTERNAL));
}

TEST(MonitorableTraceTest, IsAndMarkMonitoredQueueInternalTest) {
  MonitorableQueueTrace testTrace;

  EXPECT_FALSE(testTrace.isMonitored(MonitorableQueueTrace::Direction::IN));
  EXPECT_FALSE(testTrace.isMonitored(MonitorableQueueTrace::Direction::OUT));
  EXPECT_FALSE(
      testTrace.isMonitored(MonitorableQueueTrace::Direction::INTERNAL));

  testTrace.markMonitored(MonitorableQueueTrace::Direction::INTERNAL);

  EXPECT_TRUE(testTrace.isMonitored(MonitorableQueueTrace::Direction::IN));
  EXPECT_TRUE(testTrace.isMonitored(MonitorableQueueTrace::Direction::OUT));
  EXPECT_TRUE(
      testTrace.isMonitored(MonitorableQueueTrace::Direction::INTERNAL));
}

} // namespace facebook::bgp
