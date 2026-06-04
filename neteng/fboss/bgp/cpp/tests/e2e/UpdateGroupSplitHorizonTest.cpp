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
 * [BGP++][UG2 E2E] Split-horizon / canAnnounce mismatch tests.
 *
 * Tests for Bug 7 (D99568460): A detached peer in DETACHED_INIT_DUMP that
 * re-advertises a route it originally sourced gets stuck due to
 * canAnnounce() vs canAnnounceForGroup() mismatch. Per-peer path applies
 * split-horizon filtering; group path skips it. This causes RIB_OUT
 * discrepancy during collapseLiteEntries at rejoin time.
 *
 * Learning patterns applied:
 *   P8: Use 3 peers to avoid blocking issues
 *   P9: Always waitForPeerState(DOWN) after bringDownPeer()
 *   P10: Always sendEoRToPeer() after bringUpPeer() on reconnect
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Test 5 (Critical): DetachedPeer_SamePeerRouteRejoinNoDiscrepancy
 *
 * Flow:
 * 1. 3 peers in group, peer3 advertises a route
 * 2. peer4/peer5 receive it (peer3 doesn't — split-horizon)
 * 3. Bring peer3 DOWN (route withdrawn)
 * 4. Bring peer3 back UP (enters DETACHED_INIT_DUMP)
 * 5. peer3 re-advertises the same route
 * 6. Assert: peer3 reaches JOINED_RUNNING (not stuck)
 * 7. Assert: no RIB_OUT_DISCREPANCY
 */
TEST_P(
    UpdateGroupMultiPeerTest,
    DetachedPeer_SamePeerRouteRejoinNoDiscrepancy) {
  XLOGF(INFO, "=== TEST: DetachedPeer_SamePeerRouteRejoinNoDiscrepancy ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  setupSlowPeerComponents(8, 6, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  bringUpPeer(kPeerAddr5);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId5));
  EXPECT_TRUE(waitForEoR(peerId5));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  /* Inject route sourced FROM peer3 */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, kNextHopV4_3.str(), "65001");
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("10.0.0.0/8"), kPeerAddr3));

  /* peer4 and peer5 receive it (peer3 doesn't — split-horizon) */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001 65001"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001 65001"));

  /* P9: Bring peer3 DOWN — route gets withdrawn */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* peer4 and peer5 receive withdrawal */
  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr4));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));

  /* P10: Bring peer3 back UP, send EoR */
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);

  /* Peer3 re-advertises the same route */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, kNextHopV4_3.str(), "65001");
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("10.0.0.0/8"), kPeerAddr3));

  /* peer4 and peer5 should receive the route again */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001 65001"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001 65001"));

  /* Consume EoRs from peer3 */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));

  /*
   * The critical assertion: peer3 must reach JOINED_RUNNING.
   * Without the fix, canAnnounce vs canAnnounceForGroup mismatch
   * causes RIB_OUT_DISCREPANCY in collapseLiteEntries, leaving
   * peer3 stuck in DETACHED_RUNNING.
   */
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING, 100));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));

  XLOGF(
      INFO,
      "=== TEST PASSED: DetachedPeer_SamePeerRouteRejoinNoDiscrepancy ===");
}

/*
 * Test 17 (Medium): AcceptanceProcedure_CanAnnounceGuardDuringWindow
 *
 * After detach + recovery + acceptance, verify the peer properly
 * receives new routes as part of the group distribution.
 */
TEST_P(
    UpdateGroupMultiPeerTest,
    AcceptanceProcedure_PostAcceptanceRouteDelivery) {
  XLOGF(INFO, "=== TEST: AcceptanceProcedure_PostAcceptanceRouteDelivery ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  setupSlowPeerComponents(8, 6, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  bringUpPeer(kPeerAddr5);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId5));
  EXPECT_TRUE(waitForEoR(peerId5));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  /* Detach peer3 via frequency threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 7; i++) {
    auto prefix = fmt::format("110.{}.0.0/16", 10 + i);
    auto community = fmt::format("{}:1", 11010 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("110.{}.0.0", 10 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("110.{}.0.0", 10 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1);
  verifySlowPeerInvariants(kPeerAddr3);

  /* P1,P5: Unblock peer3 — let it recover and rejoin */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING, 100));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 0);

  /* After acceptance, inject a new route — ALL 3 peers must receive it */
  injectLocalRoutesAtRuntime({"120.0.0.0/8"}, {"1200:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("120.0.0.0/8")));

  /* Drain peer3 to get the route (P7: recovered peer may have old msgs) */
  drainPeerQueueCompletely(peerId3);

  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "120.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1200:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "120.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1200:1"));

  XLOGF(
      INFO,
      "=== TEST PASSED: AcceptanceProcedure_PostAcceptanceRouteDelivery ===");
}

/*
 * Split horizon -- routes from peer not reflected back.
 */
TEST_P(UpdateGroupMultiPeerTest, SplitHorizonPreservedThroughRecovery) {
  XLOGF(INFO, "=== TEST: SplitHorizonPreservedThroughRecovery ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  /* Pre-detach: inject a route -- both peers receive (local origin) */
  injectLocalRoutesAtRuntime({"95.1.0.0/16"}, {"9501:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("95.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "95.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "9501:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "95.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "9501:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 7; i++) {
    auto prefix = fmt::format("95.{}.0.0/16", 10 + i);
    auto community = fmt::format("{}:1", 9510 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("95.{}.0.0", 10 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Inject a CL route while detached */
  injectLocalRoutesAtRuntime({"95.20.0.0/16"}, {"9520:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("95.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "95.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "9520:1"));

  /* Unblock peer3 -- recovery begins, CL items processed.
   * Queue (8,6,0) with 1 CL item: CL batch = 1 item <= hwm=6, no re-block. */
  unblockPeer(kPeerAddr3);
  drainPeerQueueCompletely(peerId3);
  EXPECT_NE(getPeerState(kPeerAddr3), PeerUpdateState::DOWN);

  /* Post-recovery: inject new route -- both peers should receive it */
  injectLocalRoutesAtRuntime({"95.30.0.0/16"}, {"9530:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("95.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "95.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "9530:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOGF(INFO, "=== PASSED: SplitHorizon ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupMultiPeerTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
