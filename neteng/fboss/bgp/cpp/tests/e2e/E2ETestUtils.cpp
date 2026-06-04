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

#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestUtils.h"

#include <folly/coro/Sleep.h>

#include "neteng/fboss/bgp/cpp/lib/BgpUtil.h"

namespace facebook {
namespace bgp {

// TestFib Implementation
TestFib::TestFib(FibMessageQueue& toRibQ) : toRibQ_(toRibQ) {}

void TestFib::updateUnicastRoute(
    const folly::CIDRNetwork& prefix,
    std::shared_ptr<const BgpPath> attrsToBeAdvertised,
    std::shared_ptr<const WeightedNexthopMap> weightedNexthops,
    const bool /*isLocalRouteBest*/,
    const bool /*installToFib*/,
    const folly::F14NodeMap<folly::IPAddress, NexthopInfo>& /*nextHopInfoMap*/,
    const std::optional<uint32_t>& /*classId*/,
    std::shared_ptr<const NexthopTopoInfoMap> /*nhtTopo*/,
    const BgpRouteType /*routeType*/) {
  staged_[attrsToBeAdvertised][prefix] = weightedNexthops;
  /*
   * Also store in programmedRoutes_ for test verification.
   * This persists across program() calls unlike staged_.
   */
  programmedRoutes_[prefix] = weightedNexthops;
  programCallCount_++;
}

bool TestFib::isConnected() const {
  return connected_;
}

bool TestFib::isFullSynced() const {
  return fullSynced_;
}

folly::coro::Task<void> TestFib::program(bool isSync) {
  if (programDelay_.count() > 0) {
    co_await folly::coro::sleep(programDelay_);
  }
  if (failNextProgram_) {
    failNextProgram_ = false;
    failedProgramCount_++;
    XLOGF(INFO, "[TestFib] program() FAILED (simulated), isSync={}", isSync);
    staged_.clear();
    co_return;
  }
  fullSynced_ |= isSync;
  XLOGF(INFO, "[TestFib] program() called, isSync={}", isSync);
  toRibQ_.push(Fib::FibProgrammedMessage(staged_, isSync));
  staged_.clear();
  co_return;
}

void TestFib::stop() {}

// TestRib Implementation
void TestRib::createFib() {
  if (!fib_) {
    auto testFib = std::make_unique<TestFib>(fromFibMessageQ_);
    testFib_ = testFib.get();
    fib_ = std::move(testFib);
  }
}

// Helper Functions
std::shared_ptr<BgpPath> buildBgpPath(const folly::IPAddress& nexthop) {
  auto attrs = std::make_shared<BgpPath>(
      BgpPathFields(*facebook::nettools::bgplib::BgpUpdate2toBgpPathC(
          facebook::bgp::buildBgpUpdateAttributes(nexthop))));
  attrs->publish();
  return attrs;
}

} // namespace bgp
} // namespace facebook
