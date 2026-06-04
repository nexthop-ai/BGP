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

// E2E test utilities: TestFib, TestRib, and helper functions
// Minimal mocking (FIB only), real RIB/PeerManager/AdjRib components

#pragma once

#include <folly/coro/BlockingWait.h>
#include <folly/logging/xlog.h>
#include <gtest/gtest.h>
#include <chrono>

#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/common/RibMessage.h"
#include "neteng/fboss/bgp/cpp/config/Config.h"
#include "neteng/fboss/bgp/cpp/rib/Fib.h"
#include "neteng/fboss/bgp/cpp/rib/RibBB.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/watchdog/MonitoredQueue.h"

namespace facebook {
namespace bgp {

// Mock FIB that stages route updates and acknowledges them to RIB
class TestFib : public Fib {
 public:
  using FibMessageQueue = Fib::FibMessageQueue;
  using FibProgrammedPfxs = Fib::FibProgrammedPfxs;

  explicit TestFib(FibMessageQueue& toRibQ);

  // Override Fib methods
  void updateUnicastRoute(
      const folly::CIDRNetwork& prefix,
      std::shared_ptr<const BgpPath> attrsToBeAdvertised,
      std::shared_ptr<const WeightedNexthopMap> weightedNexthops,
      const bool isLocalRouteBest,
      const bool installToFib,
      const folly::F14NodeMap<folly::IPAddress, NexthopInfo>& nextHopInfoMap,
      const std::optional<uint32_t>& classId = std::nullopt,
      std::shared_ptr<const NexthopTopoInfoMap> nhtTopo = nullptr,
      const BgpRouteType routeType = BgpRouteType::UNKNOWN) override;

  bool isConnected() const override;

  bool isFullSynced() const override;

  /* Control FIB connection state for testing disconnect/reconnect scenarios */
  void setConnected(bool connected) {
    connected_ = connected;
  }

  /* Add artificial delay before program() sends FibProgrammedMessage */
  void setProgramDelay(std::chrono::milliseconds delay) {
    programDelay_ = delay;
  }

  /*
   * Make the next program() call fail (not send ack to RIB).
   * One-shot: resets to false after one failure.
   */
  void setFailNextProgram(bool fail) {
    failNextProgram_ = fail;
  }

  bool getFailNextProgram() const {
    return failNextProgram_;
  }

  size_t getFailedProgramCount() const {
    return failedProgramCount_;
  }

  /*
   * Simulate FIB disconnect followed by reconnect.
   * On reconnect, pushes FibSyncReq to RIB to trigger full route resync.
   */
  void simulateDisconnect() {
    connected_ = false;
    XLOG(INFO, "[TestFib] Simulated disconnect");
  }

  void simulateReconnect() {
    connected_ = true;
    fullSynced_ = false;
    toRibQ_.push(Fib::FibSyncReq{});
    XLOG(INFO, "[TestFib] Simulated reconnect, sent FibSyncReq");
  }

  folly::coro::Task<void> program(bool isSync = false) override;

  void stop() override;

  // Test helper methods
  size_t getProgramCallCount() const {
    return programCallCount_;
  }

  const FibProgrammedPfxs& getStagedRoutes() const {
    return staged_;
  }

  /*
   * Get all programmed routes (persists across program() calls).
   * Unlike staged_ which is cleared after each program() call,
   * this map retains all routes that have been programmed to FIB.
   */
  const folly::
      F14NodeMap<folly::CIDRNetwork, std::shared_ptr<const WeightedNexthopMap>>&
      getProgrammedRoutes() const {
    return programmedRoutes_;
  }

 private:
  FibProgrammedPfxs staged_;
  /*
   * Persistent map of all programmed routes (prefix -> weighted nexthops).
   * Updated on each updateUnicastRoute() call, not cleared on program().
   */
  folly::
      F14NodeMap<folly::CIDRNetwork, std::shared_ptr<const WeightedNexthopMap>>
          programmedRoutes_;
  bool connected_{true};
  bool fullSynced_{false};
  bool failNextProgram_{false};
  size_t failedProgramCount_{0};
  std::chrono::milliseconds programDelay_{0};
  FibMessageQueue& toRibQ_;
  size_t programCallCount_{0};
};

// RIB subclass that injects TestFib
class TestRib : public RibBB {
 public:
  using RibBB::RibBB;

  void createFib() override;

  TestFib* getTestFib() {
    return testFib_;
  }

 private:
  TestFib* testFib_{nullptr};
};

// Build a BgpPath for testing with standard attributes
std::shared_ptr<BgpPath> buildBgpPath(const folly::IPAddress& nexthop);

} // namespace bgp
} // namespace facebook
