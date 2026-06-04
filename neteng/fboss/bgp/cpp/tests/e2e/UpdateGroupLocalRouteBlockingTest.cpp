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
 * Parameterized E2E tests for BGP Update Group - Part 1
 * Tests are parameterized by:
 *   - IP version (v4/v6)
 *   - Serialization mode (enableSerializeGroupPdu true/false)
 *
 * This file contains: Basic blocking/unblocking tests
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupTestUtils.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Test fixture inheriting from the shared UpdateGroupTestBase.
 */
class UpdateGroupBlockingTest : public UpdateGroupTestBase {};

/*
 * =============================================================================
 * TEST CASES - Basic blocking/unblocking tests
 * =============================================================================
 */

/*
 * Test: Single peer blocks and unblocks with route adds
 * Phases: Add routes -> verify blocking -> drain queue -> withdraw -> re-add ->
 * attr change
 */
TEST_P(UpdateGroupBlockingTest, SinglePeerBlockUnblock_Add) {
  SCOPED_TRACE("Protocol: " + params().name);
  XLOGF(INFO, "=== TEST: SinglePeerBlockUnblock_Add ({}) ===", params().name);

  setDefaultQueueSizes(2, 1, 0);
  addPeer(getPeerSpec3());
  setupComponents();
  bringUpPeer(kPeerAddr3);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));

  /* Phase 1: Add 5 routes with different communities = 5 BGP UPDATEs */
  auto routePrefixes = prefixes(0, 5);
  injectRoutes(routePrefixes, 100);

  /* Peer should be blocked (queue highWm=1) */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  XLOG(INFO, "Peer blocked as expected");

  /* Unblock by reading all routes */
  EXPECT_TRUE(
      verifyRoutesWithFix(kPeerAddr3, makeRouteSpecs(routePrefixes, 100)));

  /* Peer should be unblocked now */
  EXPECT_FALSE(isPeerQueueBlocked(peerId3));
  XLOG(INFO, "Peer unblocked after draining queue");

  /* Phase 2: Withdraw routes and verify */
  XLOG(INFO, "Phase 2: Withdrawing all 5 routes");
  withdrawRoutes(routePrefixes);
  EXPECT_TRUE(verifyWithdraws(kPeerAddr3, routePrefixes));
  XLOG(INFO, "Withdrawals verified");

  /* Phase 3: Re-add 2 routes and verify */
  XLOG(INFO, "Phase 3: Re-adding 2 routes");
  auto readdPrefixes = prefixes(0, 2);
  injectRoutes(readdPrefixes, 100);
  EXPECT_TRUE(
      verifyRoutesWithFix(kPeerAddr3, makeRouteSpecs(readdPrefixes, 100)));
  XLOG(INFO, "Re-add verified");

  /* Phase 4: Change attributes and verify */
  XLOG(INFO, "Phase 4: Changing route attributes");
  injectLocalRoutesAtRuntime({prefix(0)}, {"100:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix(0))));
  EXPECT_TRUE(
      verifyRoutesWithFix(kPeerAddr3, makeRouteSpec(prefix(0), "100:99")));
  XLOG(INFO, "Attribute change verified - test complete");
}

/*
 * Test: Some peers in group block, controlled by selective draining
 * Both peers use same queue size. We drain peer4 first to unblock it
 * while peer3 remains blocked, then drain peer3.
 */
TEST_P(UpdateGroupBlockingTest, SomePeersBlock_Add) {
  SCOPED_TRACE("Protocol: " + params().name);
  XLOGF(INFO, "=== TEST: SomePeersBlock_Add ({}) ===", params().name);

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

  /* Add 4 routes - both peers should block */
  auto routePrefixes = prefixes(0, 4);
  injectRoutes(routePrefixes, 200);

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  XLOG(INFO, "Both peers blocked as expected");

  /*
   * Drain peer4 first - this unblocks peer4 while peer3 remains blocked.
   */
  EXPECT_TRUE(
      verifyRoutesWithFix(kPeerAddr4, makeRouteSpecs(routePrefixes, 200)));
  EXPECT_FALSE(isPeerQueueBlocked(peerId4));

  /*
   * Verify peer3 is still blocked using waitForPeerQueueBlocked for more
   * robust checking instead of point-in-time isPeerQueueBlocked.
   */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3, 10));
  XLOG(INFO, "Peer4 unblocked, peer3 still blocked");

  /* Add 2 more routes - update group blocked by peer3 */
  auto morePrefixes = prefixes(4, 2);
  injectRoutes(morePrefixes, 200);

  /* Drain peer3 - unblocks it and allows routes 5-6 to flow */
  EXPECT_TRUE(
      verifyRoutesWithFix(kPeerAddr3, makeRouteSpecs(routePrefixes, 200)));
  EXPECT_FALSE(isPeerQueueBlocked(peerId3));

  /* Both peers should receive routes 5-6 */
  EXPECT_TRUE(
      verifyRoutesWithFix(kPeerAddr3, makeRouteSpecs(morePrefixes, 200)));
  EXPECT_TRUE(
      verifyRoutesWithFix(kPeerAddr4, makeRouteSpecs(morePrefixes, 200)));

  XLOG(INFO, "Both peers received all routes correctly");
}

/*
 * Test: All peers in group block
 * With update groups, all peers must be drained before the group can continue.
 * Use larger queue sizes to allow all routes to be queued.
 */
TEST_P(UpdateGroupBlockingTest, AllPeersBlock_Add) {
  SCOPED_TRACE("Protocol: " + params().name);
  XLOGF(INFO, "=== TEST: AllPeersBlock_Add ({}) ===", params().name);

  /* Use queue that can hold all routes but still blocks after */
  setDefaultQueueSizes(8, 7, 0);
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

  /* Add 7 routes - both peers should block */
  auto routePrefixes = prefixes(0, 7);
  injectRoutes(routePrefixes, 300);

  /* Both should be blocked */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  XLOG(INFO, "Both peers blocked as expected");

  /* Drain both queues */
  EXPECT_TRUE(
      verifyRoutesWithFix(kPeerAddr3, makeRouteSpecs(routePrefixes, 300)));
  EXPECT_TRUE(
      verifyRoutesWithFix(kPeerAddr4, makeRouteSpecs(routePrefixes, 300)));

  /* Both should be unblocked */
  EXPECT_FALSE(isPeerQueueBlocked(peerId3));
  EXPECT_FALSE(isPeerQueueBlocked(peerId4));
  XLOG(INFO, "Both peers unblocked after draining");
}

INSTANTIATE_UPDATE_GROUP_TESTS(UpdateGroupBlockingTest);

} // namespace bgp
} // namespace facebook
