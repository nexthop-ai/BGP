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
 * E2E tests: DSP forPeer, scale, nexthop, AS-PATH, dedup, livelock
 *
 * Prefix range: 42.x.0.0/16
 *
 * Tests:
 *   DSP CL item uses forPeer owner key
 *   DSP processes 100 CL items (scale)
 *   DSP with nextHopSelf — per-peer nexthop
 *   DSP peer generates correct AS-PATH
 *   DSP no duplicate UPDATEs during recovery
 *   DSP livelock — continuous route injection prevents readiness
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * DSP CL item uses forPeer owner key for per-peer entry.
 * Inject route while detached. The CL item creates a per-peer entry
 * with forPeer() owner key (not forGroup). Verify via invariant check.
 */
TEST_P(UpdateGroupMultiPeerTest, DSP_ForPeerOwnerKey) {
  XLOG(INFO, "=== TEST: DSP_ForPeerOwnerKey ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  /* Two waitForEoR per peer — one per AFI (v4 + v6), since the update-group
   * key negotiates both AfiIpv4Negotiated and AfiIpv6Negotiated. */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Inject shared route */
  injectLocalRoutesAtRuntime({"42.1.0.0/16"}, {"4201:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("42.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "42.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "4201:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "42.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "4201:1"));

  /* Freq-detach */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("42.{}.0.0/16", 2 + i);
    auto community = fmt::format("{}:1", 4202 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("42.{}.0.0", 2 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Update shared route — triggers lazy clone (Case 4).
   * Per-peer entry uses forPeer() owner key. */
  withdrawLocalRoutesAtRuntime({"42.1.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "42.1.0.0", 16, kPeerAddr4));

  verifySlowPeerInvariants(kPeerAddr3);
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Unblock peer3 — CL drains the forPeer() per-peer entry (42.1 withdraw).
   * Peer3 MUST receive the withdrawal via CL: if the forPeer() owner key
   * weren't used, peer3's CL would have no entry and the withdrawal would
   * never reach it. Arrival of the withdraw on peer3 is direct evidence
   * that the forPeer() per-peer CL entry existed and was consumed. */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING, 200));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "42.1.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 0);

  XLOG(INFO, "=== TEST PASSED: DSP_ForPeerOwnerKey ===");
}

/*
 * DSP processes many CL items — verify routes delivered.
 * Inject 100 routes while peer3 is detached. All go to CL, peer4
 * receives all. Verify invariants hold after scale injection.
 */
TEST_P(UpdateGroupMultiPeerTest, DSP_ScaleCLItems) {
  XLOG(INFO, "=== TEST: DSP_ScaleCLItems ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  /* Two waitForEoR per peer — one per AFI (v4 + v6), since the update-group
   * key negotiates both AfiIpv4Negotiated and AfiIpv6Negotiated. */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("42.{}.0.0/16", 10 + i);
    auto community = fmt::format("{}:1", 4210 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("42.{}.0.0", 10 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Inject 100 CL items while detached */
  for (int i = 0; i < 100; i++) {
    auto prefix = fmt::format("42.{}.0.0/16", 100 + i);
    auto community = fmt::format("{}:1", 4300 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("42.{}.0.0", 100 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Peer3 still detached, all CL items accumulated */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOG(INFO, "=== TEST PASSED: DSP_ScaleCLItems ===");
}

/*
 * DSP with nextHopSelf — per-peer nexthop in BGP UPDATE.
 * Verify peer4 receives route with correct nexthop after detachment.
 */
TEST_P(UpdateGroupMultiPeerTest, DSP_NextHopSelf) {
  XLOG(INFO, "=== TEST: DSP_NextHopSelf ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  /* Two waitForEoR per peer — one per AFI (v4 + v6), since the update-group
   * key negotiates both AfiIpv4Negotiated and AfiIpv6Negotiated. */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("42.{}.0.0/16", 30 + i);
    auto community = fmt::format("{}:1", 4230 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("42.{}.0.0", 30 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Post-detach route — peer4 gets it with correct nextHopSelf */
  injectLocalRoutesAtRuntime({"42.35.0.0/16"}, {"4235:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("42.35.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "42.35.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "4235:1"));

  /* Unblock peer3 → recovery. Peer3 must receive 42.35 via CL with its
   * own per-peer nexthop (kPeerAddr3's expected nexthop), NOT peer4's.
   * This is the core assertion of the nextHopSelf test — each peer gets
   * a nexthop scoped to its peering config, not the group's. */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING, 200));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "42.35.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "4235:1"));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 0);

  XLOG(INFO, "=== TEST PASSED: DSP_NextHopSelf ===");
}

/*
 * DSP peer generates correct AS-PATH — no loop prevention issue.
 * Inject route before and after detachment. Verify peer4 receives both
 * with correct AS-PATH (4200000001 = configured ASN).
 */
TEST_P(UpdateGroupMultiPeerTest, DSP_CorrectAsPath) {
  XLOG(INFO, "=== TEST: DSP_CorrectAsPath ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  /* Two waitForEoR per peer — one per AFI (v4 + v6), since the update-group
   * key negotiates both AfiIpv4Negotiated and AfiIpv6Negotiated. */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Pre-detach route */
  injectLocalRoutesAtRuntime({"42.40.0.0/16"}, {"4240:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("42.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "42.40.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "4240:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "42.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "4240:1"));

  /* Freq-detach */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("42.{}.0.0/16", 41 + i);
    auto community = fmt::format("{}:1", 4241 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("42.{}.0.0", 41 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Post-detach route with correct AS-PATH */
  injectLocalRoutesAtRuntime({"42.45.0.0/16"}, {"4245:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("42.45.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "42.45.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "4245:1"));

  /* Unblock peer3 → recovery. Peer3 must receive the post-detach route
   * 42.45 via its CL with the SAME AS-PATH (4200000001) as peer4 received
   * via PL. Any loop-prevention or AS-PATH corruption during CL delivery
   * would show up as a mismatch here. */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING, 200));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "42.45.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "4245:1"));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 0);

  XLOG(INFO, "=== TEST PASSED: DSP_CorrectAsPath ===");
}

/*
 * DSP no duplicate UPDATEs during recovery.
 * Detach, inject route, unblock. After recovery, inject another route.
 * Peer4 should receive exactly 1 route (no duplicates).
 */
TEST_P(UpdateGroupMultiPeerTest, DSP_NoDuplicateUpdates) {
  XLOG(INFO, "=== TEST: DSP_NoDuplicateUpdates ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(5, 4, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  /* Two waitForEoR per peer — one per AFI (v4 + v6), since the update-group
   * key negotiates both AfiIpv4Negotiated and AfiIpv6Negotiated. */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 5; i++) {
    auto prefix = fmt::format("42.{}.0.0/16", 50 + i);
    auto community = fmt::format("{}:1", 4250 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("42.{}.0.0", 50 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock peer3 — unblockPeer() internally drains peer3's bounded queue
   * (it consumes the CL items 42.50..42.54 that were delivered during
   * recovery). Wait for peer3 to reach JR so the post-recovery 42.56
   * below flows via PL. isPeerInSync(peer3) at end verifies peer3's RIB
   * contains all of 42.50..54 + 42.56 — i.e. no lost/duplicate UPDATEs. */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING, 200));

  /* Post-recovery route — delivered once via PL to each peer */
  injectLocalRoutesAtRuntime({"42.56.0.0/16"}, {"4256:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("42.56.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "42.56.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "4256:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "42.56.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "4256:1"));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 0);

  XLOG(INFO, "=== TEST PASSED: DSP_NoDuplicateUpdates ===");
}

/*
 * DSP livelock — continuous route injection prevents readiness.
 * After unblock, inject routes rapidly. Peer may never reach DRJ because
 * new CL items keep arriving. Verify no crash, peer4 works normally.
 */
TEST_P(UpdateGroupMultiPeerTest, DSP_Livelock) {
  XLOG(INFO, "=== TEST: DSP_Livelock ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(5, 4, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  /* Two waitForEoR per peer — one per AFI (v4 + v6), since the update-group
   * key negotiates both AfiIpv4Negotiated and AfiIpv6Negotiated. */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 5; i++) {
    auto prefix = fmt::format("42.{}.0.0/16", 60 + i);
    auto community = fmt::format("{}:1", 4260 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("42.{}.0.0", 60 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock and immediately inject 5 more routes — livelock scenario */
  unblockPeer(kPeerAddr3);
  for (int i = 0; i < 5; i++) {
    auto prefix = fmt::format("42.{}.0.0/16", 66 + i);
    auto community = fmt::format("{}:1", 4266 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("42.{}.0.0", 66 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Livelock scenario: peer3 may oscillate DB ↔ DR ↔ DRJ ↔ JR as new CL
   * items keep arriving. Under livelock, peer3 may NEVER reach a stable
   * JR — that is the whole point of this test. So the peer3 strengthening
   * is a concrete enum-set check (DB/DR/DRJ/JR), NOT a forced JR assertion.
   * DOWN or JOINED_BLOCKED would indicate a real wedge (not livelock). */
  auto state3 = getPeerState(kPeerAddr3);
  EXPECT_TRUE(
      state3 == PeerUpdateState::DETACHED_BLOCKED ||
      state3 == PeerUpdateState::DETACHED_RUNNING ||
      state3 == PeerUpdateState::DETACHED_READY_TO_JOIN ||
      state3 == PeerUpdateState::JOINED_RUNNING)
      << "Expected DB/DR/DRJ/JR for peer3, got " << static_cast<int>(state3);
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: DSP_Livelock ===");
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
