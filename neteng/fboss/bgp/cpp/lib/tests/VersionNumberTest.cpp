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

#define VersionNumber_TEST_FRIENDS                         \
  friend class VersionNumberTestFixture;                   \
  FRIEND_TEST(ScopedLockTests, ConcurrentFiberAccessTest); \
  FRIEND_TEST(VersionNumberTests, GrabScopedLockTest);     \
  FRIEND_TEST(VersionNumberTestFixture, ResetGlobalVersionCounterTest);

#include <gtest/gtest.h>

#include <random>

#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/async/EventBase.h>

#include "neteng/fboss/bgp/cpp/lib/fibers/Utils.h"
#include "neteng/fboss/bgp/cpp/lib/fibers/VersionNumber.h"

namespace facebook::nettools::bgplib {

// Test that concurrent access from multiple fibers is properly synchronized
TEST(ScopedLockTests, ConcurrentFiberAccessTest) {
  folly::EventBase evb;
  auto& fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Semaphore semaphore(1);
  int counter = 0;

  // generate random delay
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(1, 20); // Random delay between 1 and 20
                                              // ms
  auto worker = [&]() {
    VersionNumber::ScopedLock lock(semaphore);
    int delay = dis(gen); // Generate a random delay
    fiberSleepFor(std::chrono::milliseconds(delay));
    ++counter;
  };
  for (int i = 0; i < 100; ++i) {
    fm.addTask(worker);
  }
  evb.loop();
  EXPECT_EQ(counter, 100);
}

TEST(VersionNumberTests, GrabScopedLockTest) {
  VersionNumber number;

  EXPECT_EQ(number.semaphore_.getAvailableTokens(), 1);

  {
    auto lock = number.grabScopedLock();
    EXPECT_EQ(number.semaphore_.getAvailableTokens(), 0);
  }

  EXPECT_EQ(number.semaphore_.getAvailableTokens(), 1);
}

TEST(VersionNumberTests, ConstructorWithDefaultValueTest) {
  VersionNumber number{42};

  EXPECT_EQ(number.get(), 42);
}

class VersionNumberTestFixture : public ::testing::Test {
 public:
  VersionNumberTestFixture() = default;
  ~VersionNumberTestFixture() override = default;

  void SetUp() override {
    VersionNumber::resetGlobalVersionCounter();
  }
};

TEST_F(VersionNumberTestFixture, BumpUpTest) {
  VersionNumber number1;
  VersionNumber number2;

  // bump up based on the global version counter
  EXPECT_EQ(number1.bumpUp(), 0);
  EXPECT_EQ(number2.bumpUp(), 1);
}

TEST_F(VersionNumberTestFixture, GetTest) {
  VersionNumber number;

  for (int i = 0; i < 10; i++) {
    number.bumpUp();
  }

  EXPECT_EQ(number.get(), 9);
}

TEST_F(VersionNumberTestFixture, ResetGlobalVersionCounterTest) {
  EXPECT_EQ(VersionNumber::globalVersionCounter, 0);
}

} // namespace facebook::nettools::bgplib
