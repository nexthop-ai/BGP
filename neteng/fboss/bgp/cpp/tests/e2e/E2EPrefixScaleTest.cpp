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

/*
 * E2E tests for prefix scale scenarios (SEV Pattern 5, 10 SEVs).
 *
 * Tests verify BGP++ handles large batches of routes correctly:
 * - Large batch injection from single/multiple peers
 * - Large batch withdrawal
 * - Peer down with many routes
 * - Route churn (continuous add/withdraw)
 */

#include <gtest/gtest.h>

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

using namespace facebook::bgp;

namespace facebook::bgp {

class E2EPrefixScaleTest : public E2ERibTestFixture {};

/*
 * Inject 100 routes from a single peer → verify all are processed.
 */
TEST_F(E2EPrefixScaleTest, LargeBatchRouteInjection) {
  bringUpAllPeersWithEor();

  constexpr int kRouteCount = 100;
  for (int i = 0; i < kRouteCount; i++) {
    addRoute(
        "v4",
        fmt::format("{}.0.0.0", 10 + i),
        8,
        kPeerAddr3,
        "11.0.0.1",
        "65001");
  }

  auto lastPrefix = folly::IPAddress::createNetwork(
      fmt::format("{}.0.0.0/8", 10 + kRouteCount - 1));
  ASSERT_TRUE(waitForRouteInShadowRib(lastPrefix));
  XLOGF(INFO, "Large batch: {} routes injected", kRouteCount);
}

/*
 * Inject routes from multiple peers simultaneously.
 */
TEST_F(E2EPrefixScaleTest, LargeBatchFromMultiplePeers) {
  bringUpAllPeersWithEor();

  constexpr int kRoutesPerPeer = 50;

  for (int i = 0; i < kRoutesPerPeer; i++) {
    addRoute(
        "v4",
        fmt::format("{}.0.0.0", 10 + i),
        8,
        kPeerAddr3,
        "11.0.0.1",
        "65001");
    addRoute(
        "v4",
        fmt::format("{}.0.0.0", 110 + i),
        8,
        kPeerAddr4,
        "11.0.0.2",
        "65004");
  }

  auto lastPrefixPeer3 = folly::IPAddress::createNetwork(
      fmt::format("{}.0.0.0/8", 10 + kRoutesPerPeer - 1));
  auto lastPrefixPeer4 = folly::IPAddress::createNetwork(
      fmt::format("{}.0.0.0/8", 110 + kRoutesPerPeer - 1));
  ASSERT_TRUE(waitForRouteInShadowRib(lastPrefixPeer3));
  ASSERT_TRUE(waitForRouteInShadowRib(lastPrefixPeer4));
  XLOGF(
      INFO,
      "Multi-peer batch: {} total routes from 2 peers",
      kRoutesPerPeer * 2);
}

/*
 * Withdraw all routes from a peer with many routes.
 */
TEST_F(E2EPrefixScaleTest, LargeBatchWithdrawal) {
  bringUpAllPeersWithEor();

  constexpr int kRouteCount = 50;
  for (int i = 0; i < kRouteCount; i++) {
    addRoute(
        "v4",
        fmt::format("{}.0.0.0", 10 + i),
        8,
        kPeerAddr3,
        "11.0.0.1",
        "65001");
  }

  auto lastPrefix = folly::IPAddress::createNetwork(
      fmt::format("{}.0.0.0/8", 10 + kRouteCount - 1));
  ASSERT_TRUE(waitForRouteInShadowRib(lastPrefix));

  for (int i = 0; i < kRouteCount; i++) {
    deleteRoute("v4", fmt::format("{}.0.0.0", 10 + i), 8, kPeerAddr3);
  }

  ASSERT_TRUE(waitForRouteWithdrawnFromRib("10.0.0.0/8"));
  ASSERT_TRUE(waitForRouteWithdrawnFromRib(
      fmt::format("{}.0.0.0/8", 10 + kRouteCount - 1)));
  XLOGF(INFO, "Large batch withdrawal: {} routes withdrawn", kRouteCount);
}

/*
 * Peer with many routes goes down → all routes withdrawn.
 */
TEST_F(E2EPrefixScaleTest, PeerDownWithLargeRouteCount) {
  bringUpAllPeersWithEor();

  constexpr int kRouteCount = 50;
  for (int i = 0; i < kRouteCount; i++) {
    addRoute(
        "v4",
        fmt::format("{}.0.0.0", 10 + i),
        8,
        kPeerAddr3,
        "11.0.0.1",
        "65001");
  }

  auto lastPrefix = folly::IPAddress::createNetwork(
      fmt::format("{}.0.0.0/8", 10 + kRouteCount - 1));
  ASSERT_TRUE(waitForRouteInShadowRib(lastPrefix));

  bringDownPeer(kPeerAddr3);

  ASSERT_TRUE(waitForRouteWithdrawnFromRib("10.0.0.0/8"));
  ASSERT_TRUE(waitForRouteWithdrawnFromRib(
      fmt::format("{}.0.0.0/8", 10 + kRouteCount - 1)));
  XLOGF(INFO, "Peer down with {} routes: all withdrawn", kRouteCount);
}

/*
 * Route churn: continuous add/withdraw cycle to test stability.
 */
TEST_F(E2EPrefixScaleTest, RouteChurn) {
  bringUpAllPeersWithEor();

  constexpr int kChurnCycles = 20;
  for (int i = 0; i < kChurnCycles; i++) {
    addRoute(
        "v4",
        fmt::format("{}.0.0.0", 10 + i),
        8,
        kPeerAddr3,
        "11.0.0.1",
        "65001");
    if (i > 0) {
      deleteRoute("v4", fmt::format("{}.0.0.0", 10 + i - 1), 8, kPeerAddr3);
    }
  }

  auto lastPrefix = folly::IPAddress::createNetwork(
      fmt::format("{}.0.0.0/8", 10 + kChurnCycles - 1));
  ASSERT_TRUE(waitForRouteInShadowRib(lastPrefix));
  XLOGF(INFO, "Route churn: {} cycles completed", kChurnCycles);
}

} // namespace facebook::bgp
