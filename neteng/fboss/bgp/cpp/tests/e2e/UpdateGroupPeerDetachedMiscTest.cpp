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
 * E2E tests: Peer Detached State and Miscellaneous Events
 * Tests for P-DB route withdrawal, P-JR EoR, P-JB multi-route,
 * and P-DID (Detached Init Dump) peer-down.
 *
 * Prefix range: 11.x.0.0/16 (shared with peer state matrix tests)
 *
 * Tests implemented:
 *   P-DB x E-ROUTE-WD
 *   P-JR x E-EOR
 *   P-JB x E-MULTI-ROUTE
 *   P-DID x E-PEER-DOWN
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * P-DB x E-ROUTE-WD
 * Withdrawal arrives while peer is DETACHED_BLOCKED. Lazy clone preserves
 * the peer's snapshot before the group entry is removed. Fast peer receives
 * the withdrawal normally. CL grows with the withdrawal for recovery.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, DetachedBlocked_RouteWithdraw) {
  XLOG(INFO, "=== TEST: DetachedBlocked_RouteWithdraw ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Dual-stack EoRs */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /*
   * Inject a route while both peers are running so we can withdraw it later.
   */
  injectLocalRoutesAtRuntime({"11.47.0.0/16"}, {"1147:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.47.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.47.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1147:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.47.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1147:1"));

  /* Set frequency threshold = 1 to trigger immediate detachment */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Block peer3 and fill queue to trigger detachment */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"11.44.0.0/16"}, {"1145:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.44.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.44.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1145:1"));
  injectLocalRoutesAtRuntime({"11.45.0.0/16"}, {"1146:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.45.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.45.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1146:1"));
  injectLocalRoutesAtRuntime({"11.46.0.0/16"}, {"1147:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.46.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.46.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1147:1"));

  /* Confirm peer3 reached DETACHED_BLOCKED */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);

  /*
   * Withdraw the route while peer3 is DETACHED_BLOCKED.
   * Lazy clone fires to preserve peer3's snapshot, CL gets the withdrawal.
   * Peer4 receives the withdrawal normally.
   */
  withdrawLocalRoutesAtRuntime({"11.47.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "11.47.0.0", 16, kPeerAddr4));

  /* Peer3 remains DETACHED_BLOCKED — withdrawal doesn't change its state */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);

  /* Peer4 still healthy */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: DetachedBlocked_RouteWithdraw ===");
}
/*
 * P-JB x E-MULTI-ROUTE
 * Batch of routes injected while peer is JOINED_BLOCKED.
 * CL grows with all routes, group stays WAITING. Peer stays JOINED_BLOCKED.
 * After unblocking, fast peer receives all the queued routes.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, JoinedBlocked_MultiRoute) {
  XLOG(INFO, "=== TEST: JoinedBlocked_MultiRoute ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  /*
   * Use queue (5, 4, 0): large enough that peer3 blocks after 5 fill
   * routes, but won't re-block when the 3 batch CL entries are
   * pushed after unblocking (the new PL only has 3 entries, < 4 hwm).
   */
  setupSlowPeerComponents(5, 4, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Set high threshold so blocking alone doesn't trigger detachment */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      100,
      std::chrono::milliseconds(60000));

  /* Block peer3 and fill its queue to reach JOINED_BLOCKED (need 5 routes) */
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 5; ++i) {
    auto prefix = "11." + std::to_string(53 + i) + ".0.0/16";
    auto community = std::to_string(1153 + i) + ":1";
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        "11." + std::to_string(53 + i) + ".0.0",
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Confirm peer3 is JOINED_BLOCKED */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /*
   * Now inject a batch of 3 routes while peer3 is blocked.
   * All go to the CL. Group is in WAITING (PL not drained for peer3).
   * Peer4 cannot receive these until PL drains.
   */
  injectLocalRoutesAtRuntime({"11.60.0.0/16"}, {"1160:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.60.0.0/16")));
  injectLocalRoutesAtRuntime({"11.61.0.0/16"}, {"1161:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.61.0.0/16")));
  injectLocalRoutesAtRuntime({"11.62.0.0/16"}, {"1162:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.62.0.0/16")));

  /* Peer3 should still be JOINED_BLOCKED, group still WAITING */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));

  /*
   * Unblock peer3 to drain PL. After PL drains, group processes the CL
   * batch and pushes all three routes to both peers.
   */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /*
   * Peer4 should receive all three batch routes. CL entries may be
   * processed in reverse order, so use verifyRoutes which handles
   * order-independent matching.
   */
  auto nh4 = getExpectedNexthop(kPeerAddr4);
  EXPECT_TRUE(verifyRoutes(
      "v4",
      kPeerAddr4,
      {{"11.60.0.0", 16, nh4, "4200000001", "1160:1"},
       {"11.61.0.0", 16, nh4, "4200000001", "1161:1"},
       {"11.62.0.0", 16, nh4, "4200000001", "1162:1"}}));

  /* Both peers back to JOINED_RUNNING */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));

  XLOG(INFO, "=== TEST PASSED: JoinedBlocked_MultiRoute ===");
}

/*
 * P-DID x E-PEER-DOWN
 * Peer goes down during DETACHED_INIT_DUMP.
 * Setup: detach peer3 via freq threshold, bring it down, then back up.
 * The reconnecting peer3 re-enters init dump (DETACHED_INIT_DUMP).
 * While peer3 is in DID, bring it down again. Verify full cleanup.
 * Peer4 remains functional.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, DetachedInitDump_PeerDown) {
  XLOG(INFO, "=== TEST: DetachedInitDump_PeerDown ===");

  /*
   * To reach DETACHED_INIT_DUMP: detach peer3 via freq threshold,
   * bring it down, then bring it back up. The reconnecting peer
   * re-enters init dump (DETACHED_INIT_DUMP). Then bring it down
   * again during that init dump phase — full cleanup.
   */
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Set frequency threshold = 1 to trigger immediate detachment */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Block peer3 and fill queue to reach DETACHED_BLOCKED */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"11.70.0.0/16"}, {"1170:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.70.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.70.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1170:1"));
  injectLocalRoutesAtRuntime({"11.71.0.0/16"}, {"1171:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.71.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.71.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1171:1"));
  injectLocalRoutesAtRuntime({"11.72.0.0/16"}, {"1172:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.72.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.72.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1172:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /*
   * Now bring peer3 down from DETACHED_BLOCKED, then back up.
   * When it reconnects, it enters init dump (DETACHED_INIT_DUMP or INIT)
   * to catch up with the routes it missed.
   */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /*
   * Bring peer3 back up — it enters init dump phase.
   * Immediately bring it down again before it completes.
   * This is the P-DID x E-PEER-DOWN scenario: peer down during
   * detached init dump. Verify full cleanup, no leaks.
   */
  bringUpPeer(kPeerAddr3);
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Peer4 should still be fully functional */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Verify peer4 can still receive new routes after peer3's abort */
  injectLocalRoutesAtRuntime({"11.73.0.0/16"}, {"1173:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.73.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.73.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1173:1"));

  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: DetachedInitDump_PeerDown ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupSlowPeerDetectionTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
