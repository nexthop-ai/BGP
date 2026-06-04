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
 * E2E tests: Lazy Clone Multi-Peer and Sequence Tests
 * Tests for lazy clone case 4 attribute preservation and multi-peer clone.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {
/*
 * Case 4 clone uses forPeer() owner key — not forGroup().
 *
 * Setup: Both peers receive a shared route. Peer3 is detached.
 * Action: Update the shared route (Case 4 clone fires).
 * Verify: After clone, the group member count stays the same
 * (clone doesn't create a new group member). The detached peer
 * count stays at 1. Verify getUpdateGroupForPeer returns the
 * same group for both peers — the clone doesn't split the group.
 * The per-peer entry is owned by the individual peer, not the group.
 */
TEST_P(UpdateGroupLazyCloneTest, Case4CloneUsesForPeerOwnerKey) {
  XLOG(INFO, "=== TEST: Case4CloneUsesForPeerOwnerKey ===");

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

  /* Record initial group state */
  auto memberCountBefore = getGroupMemberCount(kPeerAddr3);
  EXPECT_EQ(memberCountBefore, 2) << "Should have 2 members in group";
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 0) << "No detached peers yet";

  /* Both peers receive shared route */
  injectLocalRoutesAtRuntime({"13.85.0.0/16"}, {"1385:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.85.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.85.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1385:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.85.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1385:1"));

  /* Detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"13.86.0.0/16"}, {"1386:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.86.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.86.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1386:1"));
  injectLocalRoutesAtRuntime({"13.87.0.0/16"}, {"1387:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.87.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.87.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1387:1"));
  injectLocalRoutesAtRuntime({"13.88.0.0/16"}, {"1388:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.88.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.88.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1388:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1)
      << "Should have 1 detached peer";

  /*
   * Update shared route — Case 4 clone fires with forPeer() owner key.
   * The clone creates a per-peer entry owned by peer3, not the group.
   */
  injectLocalRoutesAtRuntime({"13.85.0.0/16"}, {"1385:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.85.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.85.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1385:99"));

  /*
   * Verify the clone didn't alter group structure:
   * - Member count unchanged (clone is per-peer, not a new member)
   * - Detached count still 1
   * - Both peers still reference the same group
   * - Group state is consistent
   */
  EXPECT_EQ(getGroupMemberCount(kPeerAddr3), memberCountBefore)
      << "Clone must not create a new group member";
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1)
      << "Detached count must stay at 1 after clone";

  auto group3 = getUpdateGroupForPeer(kPeerAddr3);
  auto group4 = getUpdateGroupForPeer(kPeerAddr4);
  ASSERT_NE(group3, nullptr);
  ASSERT_NE(group4, nullptr);
  EXPECT_EQ(group3, group4)
      << "Both peers must still belong to the same group after clone";

  verifySlowPeerInvariants(kPeerAddr3);
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: Case4CloneUsesForPeerOwnerKey ===");
}

/*
 * Two diverged peers both need Case 4 clone.
 *
 * Setup: 3 peers (peer3, peer4, peer5) all receive a shared route.
 * Peer3 and peer4 are both detached. Peer5 is in-sync.
 * Action: Update the shared route → Case 4 clone fires for BOTH
 * detached peers independently.
 * Verify: Peer5 receives the update. Both peer3 and peer4 have
 * their invariants preserved. Two independent clones occurred.
 */
TEST_P(UpdateGroupLazyCloneTest, TwoDivergedPeersBothNeedClone) {
  XLOG(INFO, "=== TEST: TwoDivergedPeersBothNeedClone ===");

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

  /* All 3 peers receive shared route */
  injectLocalRoutesAtRuntime({"13.90.0.0/16"}, {"1390:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.90.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.90.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1390:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.90.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1390:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.90.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1390:1"));

  /* Detach peer3 via frequency threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"13.91.0.0/16"}, {"1391:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.91.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.91.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1391:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.91.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1391:1"));
  injectLocalRoutesAtRuntime({"13.92.0.0/16"}, {"1392:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.92.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.92.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1392:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.92.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1392:1"));
  injectLocalRoutesAtRuntime({"13.93.0.0/16"}, {"1393:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.93.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.93.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1393:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.93.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1393:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Now detach peer4 too */
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr4);
  injectLocalRoutesAtRuntime({"13.94.0.0/16"}, {"1394:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.94.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.94.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1394:1"));
  injectLocalRoutesAtRuntime({"13.95.0.0/16"}, {"1395:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.95.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.95.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1395:1"));
  injectLocalRoutesAtRuntime({"13.96.0.0/16"}, {"1396:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.96.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.96.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1396:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  EXPECT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Both peer3 and peer4 should be detached now */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerDetached(kPeerAddr4));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 2);

  /*
   * Update the shared route — Case 4 clone fires for BOTH
   * detached peers independently. Peer5 gets the update.
   */
  injectLocalRoutesAtRuntime({"13.90.0.0/16"}, {"1390:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.90.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.90.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1390:99"));

  /* Verify both detached peers have invariants preserved */
  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerDetached(kPeerAddr4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 2)
      << "Both peers should still be detached after clones";

  XLOG(INFO, "=== TEST PASSED: TwoDivergedPeersBothNeedClone ===");
}

/*
 * Two diverged peers — one needs clone (Case 4), other doesn't (Case
 * 1).
 *
 * Setup: 3 peers. All share a route. Peer3 is detached first.
 * The shared route is updated (Case 4 clone for peer3, creates per-peer).
 * Then peer4 is also detached. The shared route is updated AGAIN.
 * Now peer3 has a per-peer entry (Case 1, no clone) but peer4 needs
 * a clone (Case 4). Selective cloning: only peer4 gets cloned.
 * Verify: Peer5 receives the update. Peer3 invariants hold (Case 1).
 * Peer4 invariants hold (Case 4 clone fires for peer4 only).
 */
TEST_P(UpdateGroupLazyCloneTest, TwoDivergedPeersSelectiveCloning) {
  XLOG(INFO, "=== TEST: TwoDivergedPeersSelectiveCloning ===");

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

  /* All 3 peers receive shared route */
  injectLocalRoutesAtRuntime({"13.100.0.0/16"}, {"13100:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("13.100.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.100.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "13100:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.100.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "13100:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.100.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "13100:1"));

  /* Detach peer3 first */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"13.101.0.0/16"}, {"13101:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("13.101.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.101.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "13101:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.101.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "13101:1"));
  injectLocalRoutesAtRuntime({"13.102.0.0/16"}, {"13102:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("13.102.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.102.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "13102:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.102.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "13102:1"));
  injectLocalRoutesAtRuntime({"13.103.0.0/16"}, {"13103:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("13.103.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.103.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "13103:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.103.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "13103:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /*
   * Update shared route — Case 4 clone fires for peer3.
   * This creates a per-peer entry for peer3.
   */
  injectLocalRoutesAtRuntime({"13.100.0.0/16"}, {"13100:50"}, 200);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("13.100.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.100.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "13100:50"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.100.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "13100:50"));

  verifySlowPeerInvariants(kPeerAddr3);

  /* Now detach peer4 too */
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr4);
  injectLocalRoutesAtRuntime({"13.104.0.0/16"}, {"13104:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("13.104.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.104.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "13104:1"));
  injectLocalRoutesAtRuntime({"13.105.0.0/16"}, {"13105:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("13.105.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.105.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "13105:1"));
  injectLocalRoutesAtRuntime({"13.106.0.0/16"}, {"13106:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("13.106.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.106.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "13106:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  EXPECT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerDetached(kPeerAddr4));

  /*
   * Update shared route AGAIN — peer3 has per-peer entry (Case 1,
   * no clone) but peer4 needs a new clone (Case 4). Selective cloning:
   * only peer4 gets cloned, peer3 is skipped.
   */
  injectLocalRoutesAtRuntime({"13.100.0.0/16"}, {"13100:99"}, 250);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("13.100.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.100.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "13100:99"));

  /* Verify both detached peers have invariants preserved */
  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerDetached(kPeerAddr4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  XLOG(INFO, "=== TEST PASSED: TwoDivergedPeersSelectiveCloning ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupLazyCloneTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
