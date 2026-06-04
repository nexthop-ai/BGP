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

#include <folly/coro/GtestHelpers.h>
#include <folly/io/async/ScopedEventBaseThread.h>

#include "neteng/fboss/bgp/cpp/common/EvbUtils.h"

using namespace facebook::bgp;
using namespace std::chrono_literals;

CO_TEST(EvbUtilsTest, Success_ReturnsValue) {
  folly::ScopedEventBaseThread evbThread;

  auto result = co_await co_runOnEvbWithTimeout(
      *evbThread.getEventBase(), []() { return 42; }, 5s);

  EXPECT_TRUE(result.hasValue());
  EXPECT_EQ(result.value(), 42);
}

CO_TEST(EvbUtilsTest, Success_ReturnsVector) {
  folly::ScopedEventBaseThread evbThread;

  auto result = co_await co_runOnEvbWithTimeout(
      *evbThread.getEventBase(),
      []() { return std::vector<int>{1, 2, 3}; },
      5s);

  EXPECT_TRUE(result.hasValue());
  EXPECT_EQ(result.value().size(), 3);
}

CO_TEST(EvbUtilsTest, FnThrows_ReturnsException) {
  folly::ScopedEventBaseThread evbThread;

  auto result = co_await co_runOnEvbWithTimeout(
      *evbThread.getEventBase(),
      []() -> int { throw std::runtime_error("test error"); },
      5s);

  EXPECT_TRUE(result.hasException());
}
