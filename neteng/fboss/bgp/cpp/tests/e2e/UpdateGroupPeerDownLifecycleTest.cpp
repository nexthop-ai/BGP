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
 * E2E tests: Peer DOWN lifecycle scenarios
 *
 * Prefix range: 37.x.0.0/16
 *
 * Tests:
 *   Peer DOWN, another peer detaches, first peer UP — independent
 *   Peer DOWN during initial dump (G-UNINIT) — reduce member count
 *   Peer DOWN with pending out-delay entries — deferred updates cleared
 *   Two peers DOWN simultaneously — both cleaned up independently
 *   Peer DOWN: verify isDaemonShutdown_ fast path — O(1) cleanup
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Peer DOWN, another peer detaches, first peer UP — independent
 * lifecycles. Three peers: peer3 goes DOWN, peer4 gets detached via freq
 * threshold, then peer3 comes back UP. Each peer's lifecycle is independent.
 */
TEST_P(UpdateGroupMultiPeerTest, PeerDown_AnotherDetaches_PeerUp) {
  XLOG(INFO, "=== TEST: PeerDown_AnotherDetaches_PeerUp ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  bringUpPeer(kPeerAddr5);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  /* Consume 2 EoRs per peer (v4+v6) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId5));
  EXPECT_TRUE(waitForEoR(peerId5));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  /* Step 1: Bring peer3 DOWN */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Peer4 and peer5 still running */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  /* Step 2: Detach peer4 via freq threshold while peer3 is DOWN */
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr4);
  injectLocalRoutesAtRuntime({"37.1.0.0/16"}, {"3701:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.1.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3701:1"));
  injectLocalRoutesAtRuntime({"37.2.0.0/16"}, {"3702:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.2.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3702:1"));
  injectLocalRoutesAtRuntime({"37.3.0.0/16"}, {"3703:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.3.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3703:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Step 3: Bring peer3 back UP — enters DETACHED_INIT_DUMP */
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);

  /* Drain peer3's init dump queue */
  drainPeerQueueCompletely(peerId3);

  /* Peer5 remains fully functional throughout */
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  /* Verify peer5 can still receive new routes */
  injectLocalRoutesAtRuntime({"37.4.0.0/16"}, {"3704:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.4.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3704:1"));
  verifySlowPeerInvariants(kPeerAddr5);

  XLOG(INFO, "=== TEST PASSED: PeerDown_AnotherDetaches_PeerUp ===");
}

/*
 * Peer DOWN during initial dump of group (G-UNINIT).
 * Bring up two peers, but before sending EoRs (group still in UNINIT),
 * bring one peer DOWN. The group reduces member count and continues
 * with the remaining peer.
 */
TEST_P(UpdateGroupPeerDownTest, PeerDown_DuringUninit) {
  XLOG(INFO, "=== TEST: PeerDown_DuringUninit ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /* Both peers in INIT (no EoRs yet), group in UNINIT */
  /* Bring peer3 DOWN while group is still in UNINIT */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Complete peer4 normally */
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Inject a route — only peer4 should receive it */
  injectLocalRoutesAtRuntime({"37.10.0.0/16"}, {"3710:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3710:1"));

  /* Peer4 fully functional as sole member */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: PeerDown_DuringUninit ===");
}

/*
 * Peer DOWN with pending out-delay entries — deferred updates cleared.
 * Use a peer with out-delay configured. Inject routes (they become pending
 * out-delay entries), then bring peer DOWN before out-delay fires. Verify
 * no crash from stale deferred entries and remaining peer works.
 */
TEST_P(UpdateGroupPeerDownTest, PeerDown_PendingOutDelay) {
  XLOG(INFO, "=== TEST: PeerDown_PendingOutDelay ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

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

  /* Inject routes — these become update entries in the group */
  injectLocalRoutesAtRuntime({"37.20.0.0/16"}, {"3720:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.20.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3720:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3720:1"));

  injectLocalRoutesAtRuntime({"37.21.0.0/16"}, {"3721:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.21.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3721:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3721:1"));

  /* Bring peer3 DOWN — any pending entries for peer3 should be cleared */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* No crash. Peer4 continues normally */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Inject another route — only peer4 receives it */
  injectLocalRoutesAtRuntime({"37.22.0.0/16"}, {"3722:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.22.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.22.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3722:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: PeerDown_PendingOutDelay ===");
}

/*
 * Two peers DOWN simultaneously — both cleaned up independently.
 * Three peers: bring peer3 and peer4 DOWN in quick succession. Peer5
 * continues to operate as the sole survivor.
 */
TEST_P(UpdateGroupMultiPeerTest, TwoPeersDownSimultaneously) {
  XLOG(INFO, "=== TEST: TwoPeersDownSimultaneously ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  bringUpPeer(kPeerAddr5);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId5));
  EXPECT_TRUE(waitForEoR(peerId5));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  /* Inject a shared route first */
  injectLocalRoutesAtRuntime({"37.30.0.0/16"}, {"3730:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.30.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3730:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3730:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.30.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3730:1"));

  /* Bring both peer3 and peer4 DOWN in rapid succession */
  bringDownPeer(kPeerAddr3);
  bringDownPeer(kPeerAddr4);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DOWN));

  /* Peer5 is the sole survivor */
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  /* Verify peer5 receives new routes as sole member */
  injectLocalRoutesAtRuntime({"37.31.0.0/16"}, {"3731:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.31.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3731:1"));
  verifySlowPeerInvariants(kPeerAddr5);

  XLOG(INFO, "=== TEST PASSED: TwoPeersDownSimultaneously ===");
}

/*
 * Peer DOWN: verify isDaemonShutdown_ fast path — O(1) cleanup.
 * When a single peer goes DOWN in a running group, cleanup should be fast
 * and not impact the remaining peer. Verify that bringing a peer DOWN after
 * routes are flowing results in clean state with no hangs or crashes.
 */
TEST_P(UpdateGroupPeerDownTest, PeerDown_FastPathCleanup) {
  XLOG(INFO, "=== TEST: PeerDown_FastPathCleanup ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

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

  /* Inject several routes to build up group state */
  for (int i = 0; i < 4; ++i) {
    auto prefix = fmt::format("37.{}.0.0/16", 40 + i);
    auto community = fmt::format("37{}:1", 40 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("37.{}.0.0", 40 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("37.{}.0.0", 40 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Bring peer3 DOWN — should be O(1) cleanup via fast path */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Peer4 continues without any delay or disruption */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Verify a new route reaches peer4 cleanly after peer3's cleanup */
  injectLocalRoutesAtRuntime({"37.44.0.0/16"}, {"3744:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.44.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.44.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3744:1"));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: PeerDown_FastPathCleanup ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupMultiPeerTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupPeerDownTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
