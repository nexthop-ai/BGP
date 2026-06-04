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
 * E2E tests for route update ordering (Gap 6).
 *
 * Tests verify correct behavior when announcements and withdrawals
 * arrive in rapid succession or interleaved across multiple prefixes.
 * SEV pattern: AdjRib race conditions during withdrawal/announcement ordering.
 */

#include <gtest/gtest.h>

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

using namespace facebook::bgp;

namespace facebook::bgp {

class E2ERouteUpdateOrderingTest : public E2ERibTestFixture {};

/*
 * Implicit withdrawal via UPDATE: send an UPDATE for the same prefix
 * with different attributes → should be treated as implicit withdrawal
 * + new announcement.
 */
TEST_F(E2ERouteUpdateOrderingTest, ImplicitWithdrawalViaUpdate) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 0, 200, 0);
  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.2", "65001");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  XLOG(INFO, "Implicit withdrawal via UPDATE: route updated in place");
}

/*
 * Withdraw then immediately announce same prefix.
 * Final state should have the route present.
 */
TEST_F(E2ERouteUpdateOrderingTest, WithdrawThenImmediateAnnounce) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr3);
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  XLOG(INFO, "Withdraw then immediate announce: route present");
}

/*
 * Announce then immediately withdraw same prefix.
 * Final state should have no route.
 */
TEST_F(E2ERouteUpdateOrderingTest, AnnounceThenImmediateWithdraw) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr3);
  ASSERT_TRUE(waitForRouteWithdrawnFromRib("10.0.0.0/8"));
  XLOG(INFO, "Announce then immediate withdraw: route gone");
}

/*
 * Interleaved updates across multiple prefixes.
 * Add and withdraw across 4 prefixes in mixed order,
 * verify final state is correct for each.
 */
TEST_F(E2ERouteUpdateOrderingTest, InterleavedUpdatesMultiPrefix) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v4", "20.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v4", "30.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix1 = folly::IPAddress::createNetwork("10.0.0.0/8");
  auto prefix2 = folly::IPAddress::createNetwork("20.0.0.0/8");
  auto prefix3 = folly::IPAddress::createNetwork("30.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix1));
  ASSERT_TRUE(waitForRouteInShadowRib(prefix2));
  ASSERT_TRUE(waitForRouteInShadowRib(prefix3));

  deleteRoute("v4", "20.0.0.0", 8, kPeerAddr3);
  addRoute("v4", "40.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr3);

  auto prefix4 = folly::IPAddress::createNetwork("40.0.0.0/8");
  ASSERT_TRUE(waitForRouteWithdrawnFromRib("10.0.0.0/8"));
  ASSERT_TRUE(waitForRouteWithdrawnFromRib("20.0.0.0/8"));
  ASSERT_TRUE(waitForRouteInShadowRib(prefix3));
  ASSERT_TRUE(waitForRouteInShadowRib(prefix4));
  XLOG(INFO, "Interleaved updates: correct final state for all prefixes");
}

/*
 * Rapid updates for same prefix — multiple updates in quick succession.
 * Only the final state should matter.
 */
TEST_F(E2ERouteUpdateOrderingTest, RapidUpdatesSamePrefix) {
  bringUpAllPeersWithEor();

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");

  for (int i = 1; i <= 10; i++) {
    addRoute(
        "v4",
        "10.0.0.0",
        8,
        kPeerAddr3,
        fmt::format("11.0.0.{}", i),
        "65001",
        "",
        0,
        static_cast<uint32_t>(100 + i),
        0);
  }

  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  XLOG(INFO, "Rapid updates same prefix: route converged");
}

} // namespace facebook::bgp
