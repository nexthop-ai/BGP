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
 * E2E tests: Multi-peer lifecycle — cascading detachment, new peer join
 * during recovery, and mixed detach/down/recovery scenarios.
 *
 * Prefix range: 38.x.0.0/16 (1-49)
 * Fixture: UpdateGroupMultiPeerTest (3 peers)
 *
 * Tests:
 *   New peer joins while recovery in progress — independent paths
 *   Cascading — A detaches, B detaches, B recovers first, A second
 *   A detaches, B goes down, C sole member, B back, A recovers
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Peer3 is detached via freq threshold. While peer3 is detached,
 * peer5 (previously DOWN) rejoins the group via bringUpPeer. Peer3 stays
 * detached throughout. Verify peer4 and reconnected peer5 both receive
 * new routes independently of peer3's detachment state.
 */
TEST_P(UpdateGroupMultiPeerTest, NewPeerJoinsDuringDetachment) {
  XLOG(INFO, "=== TEST: NewPeerJoinsDuringDetachment ===");

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

  /* Bring peer5 DOWN — will rejoin later during recovery */
  bringDownPeer(kPeerAddr5);
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::DOWN));

  /* Detach peer3 via freq threshold (1 block) */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 1; i <= 3; i++) {
    auto prefix = fmt::format("38.{}.0.0/16", i);
    auto community = fmt::format("380{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("38.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Peer5 rejoins while peer3 is detached */
  bringUpPeer(kPeerAddr5);
  sendEoRToPeer(peerId5);
  EXPECT_TRUE(waitForEoR(peerId5));
  EXPECT_TRUE(waitForEoR(peerId5));
  drainPeerQueueCompletely(peerId5);

  /* Inject new route — peer4 and peer5 should both receive */
  injectLocalRoutesAtRuntime({"38.4.0.0/16"}, {"3804:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("38.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "38.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3804:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "38.4.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3804:1"));

  /* Peer3 still detached — clean up by bringing DOWN */
  unblockPeer(kPeerAddr3);
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED ===");
}

/*
 * Cascading detachment. Peer3 detaches first, then peer4 detaches.
 * Peer5 is sole in-sync member. Peer4 recovers first (DOWN + UP), then
 * peer3 recovers second (DOWN). Verify peer5 receives routes throughout,
 * and recovered peer4 receives new routes after rejoining.
 */
TEST_P(UpdateGroupMultiPeerTest, CascadingDetachBRecoversFirstASecond) {
  XLOG(INFO, "=== TEST: CascadingDetachBRecoversFirstASecond ===");

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

  /* Detach peer3 first via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 10; i <= 12; i++) {
    auto prefix = fmt::format("38.{}.0.0/16", i);
    auto community = fmt::format("38{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("38.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("38.{}.0.0", i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  XLOG(INFO, "Checkpoint 1: peer3 detached");

  /* Detach peer4 via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr4);
  for (int i = 13; i <= 15; i++) {
    auto prefix = fmt::format("38.{}.0.0/16", i);
    auto community = fmt::format("38{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("38.{}.0.0", i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));
  XLOG(INFO, "Checkpoint 2: both peer3 and peer4 detached, peer5 sole member");

  /* Peer5 still receives routes */
  injectLocalRoutesAtRuntime({"38.16.0.0/16"}, {"3816:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("38.16.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "38.16.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3816:1"));

  /* Peer4 recovers first (B recovers first): DOWN then UP */
  unblockPeer(kPeerAddr4);
  bringDownPeer(kPeerAddr4);
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DOWN));
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  drainPeerQueueCompletely(peerId4);
  XLOG(INFO, "Checkpoint 3: peer4 recovered via DOWN+UP");

  /* Peer3 recovers second (A recovers second): bring DOWN */
  unblockPeer(kPeerAddr3);
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));
  XLOG(INFO, "Checkpoint 4: peer3 DOWN");

  /* Final: both peer4 and peer5 receive new route */
  injectLocalRoutesAtRuntime({"38.17.0.0/16"}, {"3817:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("38.17.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "38.17.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3817:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "38.17.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3817:1"));

  verifySlowPeerInvariants(kPeerAddr5);
  XLOG(INFO, "=== TEST PASSED ===");
}

/*
 * Peer3 detaches, peer4 goes DOWN, peer5 is sole member.
 * Peer4 comes back (reconnects), peer3 goes DOWN to clean up.
 * Verify peer5 works throughout, peer4 works after reconnection.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachDownSoleMemberReconnect) {
  XLOG(INFO, "=== TEST: DetachDownSoleMemberReconnect ===");

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

  /* Detach peer3 via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 20; i <= 22; i++) {
    auto prefix = fmt::format("38.{}.0.0/16", i);
    auto community = fmt::format("38{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("38.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("38.{}.0.0", i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  XLOG(INFO, "Checkpoint 1: peer3 DETACHED_BLOCKED");

  /* Peer4 goes DOWN — peer5 becomes sole in-sync member */
  bringDownPeer(kPeerAddr4);
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DOWN));
  XLOG(INFO, "Checkpoint 2: peer4 DOWN, peer5 sole member");

  /* Peer5 still works */
  injectLocalRoutesAtRuntime({"38.23.0.0/16"}, {"3823:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("38.23.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "38.23.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3823:1"));

  /* Peer4 comes back — reconnects to existing group */
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  drainPeerQueueCompletely(peerId4);
  XLOG(INFO, "Checkpoint 3: peer4 reconnected");

  /* Peer3 goes DOWN to clean up detachment */
  unblockPeer(kPeerAddr3);
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Both peer4 and peer5 receive new route */
  injectLocalRoutesAtRuntime({"38.24.0.0/16"}, {"3824:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("38.24.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "38.24.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3824:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "38.24.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3824:1"));

  verifySlowPeerInvariants(kPeerAddr5);
  XLOG(INFO, "=== TEST PASSED ===");
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
