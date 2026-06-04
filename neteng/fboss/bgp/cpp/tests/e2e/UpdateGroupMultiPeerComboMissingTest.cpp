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
 * E2E tests: Multi-peer state combination tests
 *
 * Prefix range: 47.x.0.0/16
 *
 * All tests use per-peer queue sizes with freq-detach pattern.
 * Each test puts peers in different states and verifies interactions.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * (P-DB + P-DRJ) × E-ROUTE-ADD — 0 in-sync.
 * Detach peer3 (DB), drain peer3 (DRJ attempt). Inject route.
 */
TEST_P(UpdateGroupMultiPeerTest, DbDrj_RouteAdd) {
  XLOG(INFO, "=== TEST: DbDrj_RouteAdd ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  /* Large queue for fast peer */
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr4);

  /* Small queue for slow peer */
  setDefaultQueueSizes(3, 2, 0);
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);

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

  /* Inject 3 routes — peer3's small queue fills naturally */
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("47.{}.0.0/16", 1 + i);
    auto community = fmt::format("{}:1", 4701 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("47.{}.0.0", 1 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  drainPeerQueueCompletely(peerId4);

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Drain peer3 to allow transition toward DRJ */
  drainPeerQueueCompletely(peerId3);

  injectLocalRoutesAtRuntime({"47.5.0.0/16"}, {"4705:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("47.5.0.0/16")));
  /* After drain, peer3 should be transitioning through DSP recovery toward
   * rejoin. Require a concrete state in {DB, DR, DRJ, JR} — never DOWN,
   * never JOINED_BLOCKED (JB would mean the peer got wedged in a non-DSP
   * blocked state, which contradicts the DB+DRJ scenario). */
  auto state3 = getPeerState(kPeerAddr3);
  EXPECT_TRUE(
      state3 == PeerUpdateState::DETACHED_BLOCKED ||
      state3 == PeerUpdateState::DETACHED_RUNNING ||
      state3 == PeerUpdateState::DETACHED_READY_TO_JOIN ||
      state3 == PeerUpdateState::JOINED_RUNNING)
      << "Expected DB/DR/DRJ/JR for peer3, got " << static_cast<int>(state3);
  /* Peer4 stayed JR throughout and must still be in sync with local RIB. */
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  XLOG(INFO, "=== TEST PASSED: DbDrj_RouteAdd ===");
}

/* (P-JR + P-DB) × E-ROUTE-WD — withdrawal with lazy clone */
TEST_P(UpdateGroupMultiPeerTest, JrDb_RouteWd) {
  XLOG(INFO, "=== TEST: JrDb_RouteWd ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  /* Large queue for fast peer (peer3 stays JR) */
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);

  /* Small queue for slow peer (peer4 will be detached) */
  setDefaultQueueSizes(3, 2, 0);
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
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

  /* Inject shared route and drain both peers */
  injectLocalRoutesAtRuntime({"47.10.0.0/16"}, {"4710:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("47.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "47.10.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "4710:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "47.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "4710:1"));

  /* Detach peer4 via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Inject 3 fill routes — peer4's small queue fills naturally */
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("47.{}.0.0/16", 11 + i);
    auto community = fmt::format("{}:1", 4711 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("47.{}.0.0", 11 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
  }
  drainPeerQueueCompletely(peerId3);

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Peer3=JR, Peer4=DB. Withdraw shared route */
  withdrawLocalRoutesAtRuntime({"47.10.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "47.10.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  /* Peer4 was detached; slow-peer invariants (detachment tracking, CL
   * ownership, per-peer entry accounting) must hold. Explicitly reassert
   * peer4 is still DB — it wasn't unblocked so it can't have progressed. */
  verifySlowPeerInvariants(kPeerAddr4);
  EXPECT_EQ(getPeerState(kPeerAddr4), PeerUpdateState::DETACHED_BLOCKED);

  XLOG(INFO, "=== TEST PASSED: JrDb_RouteWd ===");
}

/* Various multi-peer state combos */
TEST_P(UpdateGroupMultiPeerTest, MultiPeerCombos) {
  XLOG(INFO, "=== TEST: Multi-peer combos ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);

  /* Large queue for fast peer (peer5 stays JR) */
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr5);

  /* Small queues for slow peers (peer3 + peer4 will be detached) */
  setDefaultQueueSizes(3, 2, 0);
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);
  /* Two waitForEoR per peer — one per AFI (v4 + v6), since the update-group
   * key negotiates both AfiIpv4Negotiated and AfiIpv6Negotiated. */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId5));
  EXPECT_TRUE(waitForEoR(peerId5));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  /* Detach peer3 via freq, peer4 via duration */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(1),
      999999,
      std::chrono::milliseconds(60000));

  /* Inject 3 routes — peer3 and peer4 small queues fill naturally */
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("47.{}.0.0/16", 20 + i);
    auto community = fmt::format("{}:1", 4720 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("47.{}.0.0", 20 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));
  }
  drainPeerQueueCompletely(peerId5);

  /* Both detached */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Routes with 2 detached peers */
  injectLocalRoutesAtRuntime({"47.25.0.0/16"}, {"4725:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("47.25.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "47.25.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "4725:1"));

  /* Peer DOWN with detached peers */
  bringDownPeer(kPeerAddr5);
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::DOWN));

  /* Routes with only detached peers (0 in-sync) */
  injectLocalRoutesAtRuntime({"47.26.0.0/16"}, {"4726:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("47.26.0.0/16")));

  /* Drain peer3 and peer4 to start recovery */
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);
  /* After drain, both peers may be making recovery progress (DB -> DR -> DRJ
   * -> JR) but peer5 is DOWN so routes with no sync peer cannot be accepted
   * yet — peers may stall in DRJ. Concrete state check: both peers must be
   * in one of {DB, DR, DRJ, JR}. Peer5 must still be DOWN. */
  auto state3 = getPeerState(kPeerAddr3);
  auto state4 = getPeerState(kPeerAddr4);
  EXPECT_TRUE(
      state3 == PeerUpdateState::DETACHED_BLOCKED ||
      state3 == PeerUpdateState::DETACHED_RUNNING ||
      state3 == PeerUpdateState::DETACHED_READY_TO_JOIN ||
      state3 == PeerUpdateState::JOINED_RUNNING)
      << "Expected DB/DR/DRJ/JR for peer3, got " << static_cast<int>(state3);
  EXPECT_TRUE(
      state4 == PeerUpdateState::DETACHED_BLOCKED ||
      state4 == PeerUpdateState::DETACHED_RUNNING ||
      state4 == PeerUpdateState::DETACHED_READY_TO_JOIN ||
      state4 == PeerUpdateState::JOINED_RUNNING)
      << "Expected DB/DR/DRJ/JR for peer4, got " << static_cast<int>(state4);
  EXPECT_EQ(getPeerState(kPeerAddr5), PeerUpdateState::DOWN);

  XLOG(INFO, "=== TEST PASSED: Multi-peer combos ===");
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
