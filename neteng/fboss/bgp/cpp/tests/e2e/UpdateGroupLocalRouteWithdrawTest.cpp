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
 * Parameterized E2E tests for BGP Update Group - Part 3
 * Tests are parameterized by:
 *   - IP version (v4/v6)
 *   - Serialization mode (enableSerializeGroupPdu true/false)
 *
 * This file contains: Withdrawal and attribute change tests
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupTestUtils.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Test fixture inheriting from the shared UpdateGroupTestBase.
 */
class UpdateGroupWithdrawTest : public UpdateGroupTestBase {};

/*
 * =============================================================================
 * TEST CASES - Withdrawal and attribute change tests
 * =============================================================================
 */

/*
 * Test: Single peer blocks/unblocks with withdrawals
 */
TEST_P(UpdateGroupWithdrawTest, SinglePeerBlockUnblock_Withdraw) {
  SCOPED_TRACE("Protocol: " + params().name);
  XLOGF(
      INFO,
      "=== TEST: SinglePeerBlockUnblock_Withdraw ({}) ===",
      params().name);

  setDefaultQueueSizes(2, 1, 0);
  addPeer(getPeerSpec3());
  setupComponents();
  bringUpPeer(kPeerAddr3);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));

  /* Add 5 routes */
  auto routePrefixes = prefixes(0, 5);
  injectRoutes(routePrefixes, 700);

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(
      verifyRoutesWithFix(kPeerAddr3, makeRouteSpecs(routePrefixes, 700)));
  EXPECT_FALSE(isPeerQueueBlocked(peerId3));

  /* Add 3 more routes to block again */
  auto morePrefixes = prefixes(5, 3);
  injectRoutes(morePrefixes, 700);

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  XLOG(INFO, "Peer blocked again after more routes");

  /* Drain the additional routes before withdrawing */
  EXPECT_TRUE(
      verifyRoutesWithFix(kPeerAddr3, makeRouteSpecs(morePrefixes, 700)));

  /* Withdraw all 8 routes - withdrawals get batched */
  auto allPrefixes = prefixes(0, 8);
  withdrawRoutes(allPrefixes);
  EXPECT_TRUE(verifyWithdraws(kPeerAddr3, allPrefixes));
  XLOG(INFO, "All withdrawals verified");
}

/*
 * Test: Single peer with add, withdraw, re-add cycle
 */
TEST_P(UpdateGroupWithdrawTest, SinglePeerBlockUnblock_AddWithdrawReAdd) {
  SCOPED_TRACE("Protocol: " + params().name);
  XLOGF(
      INFO,
      "=== TEST: SinglePeerBlockUnblock_AddWithdrawReAdd ({}) ===",
      params().name);

  setDefaultQueueSizes(2, 1, 0);
  addPeer(getPeerSpec3());
  setupComponents();
  bringUpPeer(kPeerAddr3);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));

  /* Phase 1: Add 3 routes, verify blocking and unblocking */
  auto routePrefixes = prefixes(0, 3);
  injectRoutes(routePrefixes, 800);

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(
      verifyRoutesWithFix(kPeerAddr3, makeRouteSpecs(routePrefixes, 800)));
  EXPECT_FALSE(isPeerQueueBlocked(peerId3));

  /*
   * Phase 2: Withdraw routes.
   * Withdrawals get batched into 1 BGP message, so peer won't block.
   * Just verify the withdrawals directly.
   */
  withdrawRoutes(routePrefixes);
  EXPECT_TRUE(verifyWithdraws(kPeerAddr3, routePrefixes));

  /* Phase 3: Re-add routes */
  injectRoutes(routePrefixes, 800);
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(
      verifyRoutesWithFix(kPeerAddr3, makeRouteSpecs(routePrefixes, 800)));
  XLOG(INFO, "Add-withdraw-readd cycle complete");
}

/*
 * Test: Single peer with attribute changes
 */
TEST_P(UpdateGroupWithdrawTest, SinglePeerBlockUnblock_AttrChange) {
  SCOPED_TRACE("Protocol: " + params().name);
  XLOGF(
      INFO,
      "=== TEST: SinglePeerBlockUnblock_AttrChange ({}) ===",
      params().name);

  setDefaultQueueSizes(2, 1, 0);
  addPeer(getPeerSpec3());
  setupComponents();
  bringUpPeer(kPeerAddr3);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));

  /* Add 2 routes */
  auto routePrefixes = prefixes(0, 2);
  injectRoutes(routePrefixes, 900);

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(
      verifyRoutesWithFix(kPeerAddr3, makeRouteSpecs(routePrefixes, 900)));

  /* Change attribute on first route */
  injectLocalRoutesAtRuntime({prefix(0)}, {"900:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix(0))));

  EXPECT_TRUE(
      verifyRoutesWithFix(kPeerAddr3, makeRouteSpec(prefix(0), "900:99")));
  XLOG(INFO, "Attribute change verified");
}

/*
 * Test: Some peers block with attribute changes
 * Both peers use same queue size. We drain peer4 first to unblock it
 * while peer3 remains blocked, then drain peer3.
 */
TEST_P(UpdateGroupWithdrawTest, SomePeersBlock_AttrChange) {
  SCOPED_TRACE("Protocol: " + params().name);
  XLOGF(INFO, "=== TEST: SomePeersBlock_AttrChange ({}) ===", params().name);

  /* Queue (5, 4, 0): highWm=4 allows 4 routes before blocking */
  setDefaultQueueSizes(5, 4, 0);
  addPeer(getPeerSpec3());
  addPeer(getPeerSpec4());
  setupComponents();

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));

  /* Add 4 routes - both should block */
  auto routePrefixes = prefixes(0, 4);
  injectRoutes(routePrefixes, 1000);

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));

  /*
   * Drain peer4 first - this unblocks peer4 while peer3 remains blocked.
   */
  EXPECT_TRUE(
      verifyRoutesWithFix(kPeerAddr4, makeRouteSpecs(routePrefixes, 1000)));
  EXPECT_FALSE(isPeerQueueBlocked(peerId4));
  /*
   * Use waitForPeerQueueBlocked for more robust checking - the point-in-time
   * isPeerQueueBlocked check can be racy due to thread visibility delays.
   */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3, 10));
  XLOG(INFO, "Peer4 unblocked, peer3 still blocked");

  /* Drain peer3 */
  EXPECT_TRUE(
      verifyRoutesWithFix(kPeerAddr3, makeRouteSpecs(routePrefixes, 1000)));
  EXPECT_FALSE(isPeerQueueBlocked(peerId3));

  /* Change attribute on first route */
  injectLocalRoutesAtRuntime({prefix(0)}, {"1000:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix(0))));

  EXPECT_TRUE(
      verifyRoutesWithFix(kPeerAddr3, makeRouteSpec(prefix(0), "1000:99")));
  EXPECT_TRUE(
      verifyRoutesWithFix(kPeerAddr4, makeRouteSpec(prefix(0), "1000:99")));
  XLOG(INFO, "Attribute change verified for both peers");
}

INSTANTIATE_UPDATE_GROUP_TESTS(UpdateGroupWithdrawTest);

} // namespace bgp
} // namespace facebook
