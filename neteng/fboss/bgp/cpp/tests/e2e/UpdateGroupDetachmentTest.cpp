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
 * E2E tests for Update Group detachment mechanics.
 * Tests state transitions, PL clone, CL consumer, and atomicity.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Test: Detach peer, verify state transitions and bitmaps.
 * 3 peers in group, one gets detached via frequency threshold.
 * Verify: JOINED_BLOCKED -> DETACHED_BLOCKED transition,
 * sync bitmap cleared, detachedPeerBits updated, other peers unaffected.
 */
TEST_P(UpdateGroupDetachmentTest, DetachVerifyStateTransition) {
  XLOG(INFO, "=== TEST: DetachVerifyStateTransition ===");

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

  /* Verify group has 3 members, all in sync */
  EXPECT_EQ(getGroupMemberCount(kPeerAddr3), 3);
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 0);

  /* Set frequency threshold = 1 block for immediate trigger */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Block peer3, drain peer4 and peer5 to prevent them blocking */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"70.0.0.0/8"}, {"700:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("70.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "70.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "700:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "70.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "700:1"));

  injectLocalRoutesAtRuntime({"71.0.0.0/8"}, {"710:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("71.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "71.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "710:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "71.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "710:1"));

  injectLocalRoutesAtRuntime({"72.0.0.0/8"}, {"720:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("72.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "72.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "720:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "72.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "720:1"));

  /* Wait for peer3 to be detached */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Verify detachment state */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1);

  /* Verify other peers are unaffected */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  /* Group still has 3 members (detached peer is still a member) */
  EXPECT_EQ(getGroupMemberCount(kPeerAddr3), 3);

  /* Verify invariants */
  verifySlowPeerInvariants(kPeerAddr3);

  /* New routes should go only to peer4 and peer5 */
  injectLocalRoutesAtRuntime({"73.0.0.0/8"}, {"730:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("73.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "73.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "730:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "73.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "730:1"));

  XLOG(INFO, "=== TEST PASSED: DetachVerifyStateTransition ===");
}

/*
 * Test: Detached peer going down is cleaned up properly.
 * Verify: DETACHED_BLOCKED -> DOWN, detachedPeerBits cleared,
 * group member count decremented.
 */
TEST_P(UpdateGroupDetachmentTest, DetachedPeerDown_Cleanup) {
  XLOG(INFO, "=== TEST: DetachedPeerDown_Cleanup ===");

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

  /* Set frequency threshold = 1 for immediate trigger */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"80.0.0.0/8"}, {"800:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("80.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "80.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "800:1"));
  injectLocalRoutesAtRuntime({"81.0.0.0/8"}, {"810:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("81.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "81.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "810:1"));
  injectLocalRoutesAtRuntime({"82.0.0.0/8"}, {"820:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("82.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "82.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "820:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1);

  /* Bring down the detached peer */
  bringDownPeer(kPeerAddr3);

  /* Verify peer3 is DOWN */
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Verify detached bits are cleaned up */
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 0);

  /* Peer4 is still running and functional */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Verify peer4 can still receive routes */
  injectLocalRoutesAtRuntime({"83.0.0.0/8"}, {"830:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("83.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "83.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "830:1"));

  XLOG(INFO, "=== TEST PASSED: DetachedPeerDown_Cleanup ===");
}

/*
 * Test: Multiple routes injected then withdrawn while peer is detached.
 * Verify in-sync peer sees all adds and withdrawals correctly,
 * group member count stays stable.
 */
TEST_P(UpdateGroupDetachmentTest, BulkRouteChangesWhileDetached) {
  XLOG(INFO, "=== TEST: BulkRouteChangesWhileDetached ===");

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

  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Detach peer3 */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"84.0.0.0/8"}, {"840:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("84.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "84.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "840:1"));
  injectLocalRoutesAtRuntime({"85.0.0.0/8"}, {"850:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("85.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "85.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "850:1"));
  injectLocalRoutesAtRuntime({"86.0.0.0/8"}, {"860:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("86.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "86.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "860:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Inject 2 more routes while detached */
  injectLocalRoutesAtRuntime({"87.0.0.0/8"}, {"870:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("87.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "87.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "870:1"));

  injectLocalRoutesAtRuntime({"88.0.0.0/8"}, {"880:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("88.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "88.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "880:1"));

  /* Withdraw 2 routes while detached */
  withdrawLocalRoutesAtRuntime({"85.0.0.0/8"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "85.0.0.0", 8, kPeerAddr4));

  withdrawLocalRoutesAtRuntime({"87.0.0.0/8"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "87.0.0.0", 8, kPeerAddr4));

  /* Group should still have 2 members, 1 detached */
  EXPECT_EQ(getGroupMemberCount(kPeerAddr3), 2);
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1);
  verifySlowPeerInvariants(kPeerAddr3);

  XLOG(INFO, "=== TEST PASSED: BulkRouteChangesWhileDetached ===");
}

/*
 * Test: Verify group member count doesn't change during detach/down cycle.
 * Detach doesn't remove peer from group, only peer down does.
 */
TEST_P(UpdateGroupDetachmentTest, MemberCountStableAfterDetach) {
  XLOG(INFO, "=== TEST: MemberCountStableAfterDetach ===");

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
  EXPECT_EQ(getGroupMemberCount(kPeerAddr3), 2);

  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Detach peer3 */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"89.0.0.0/8"}, {"890:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("89.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "89.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "890:1"));
  injectLocalRoutesAtRuntime({"89.1.0.0/16"}, {"891:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("89.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "89.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "891:1"));
  injectLocalRoutesAtRuntime({"89.2.0.0/16"}, {"892:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("89.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "89.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "892:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Member count stays at 2 after detachment */
  EXPECT_EQ(getGroupMemberCount(kPeerAddr3), 2);

  /* Bring down detached peer */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Member count decreases to 1 after peer down */
  EXPECT_EQ(getGroupMemberCount(kPeerAddr4), 1);

  XLOG(INFO, "=== TEST PASSED: MemberCountStableAfterDetach ===");
}

/*
 * Test: Verify blocked bitmap is cleared on detachment.
 * After detachment, the group's blocked bitmap should NOT have the
 * detached peer's bit set (detachSlowPeer clears it).
 */
TEST_P(UpdateGroupDetachmentTest, BlockedBitmapClearedOnDetach) {
  XLOG(INFO, "=== TEST: BlockedBitmapClearedOnDetach ===");

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

  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Detach peer3 */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"95.0.0.0/8"}, {"950:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("95.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "95.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "950:1"));
  injectLocalRoutesAtRuntime({"96.0.0.0/8"}, {"960:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("96.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "96.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "960:1"));
  injectLocalRoutesAtRuntime({"97.0.0.0/8"}, {"970:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("97.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "97.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "970:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Verify the group no longer reports blocked peers */
  auto group = getUpdateGroupForPeer(kPeerAddr3);
  ASSERT_NE(group, nullptr);
  /*
   * hasBlockedPeers() checks the adjRibBlockedBitmap_. After detachment,
   * the detached peer's bit should be cleared from the blocked bitmap,
   * so the group should NOT report having blocked peers (unless peer4
   * is also blocked, which it shouldn't be since we drain it).
   */
  EXPECT_FALSE(group->hasBlockedPeers())
      << "After detachment, blocked bitmap should be clear";

  verifySlowPeerInvariants(kPeerAddr3);

  XLOG(INFO, "=== TEST PASSED: BlockedBitmapClearedOnDetach ===");
}

/*
 * Test: Verify RIB version is set on detached peer.
 * detachSlowPeer() sets lastSeenRibVersion and detachedRibVersion
 * on the detached peer from the group's lastSeenRibVersion.
 */
TEST_P(UpdateGroupDetachmentTest, RibVersionSetOnDetach) {
  XLOG(INFO, "=== TEST: RibVersionSetOnDetach ===");

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

  /* Inject routes to bump RIB version */
  injectLocalRoutesAtRuntime({"98.0.0.0/8"}, {"980:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("98.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "98.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "980:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "98.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "980:1"));

  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"99.0.0.0/8"}, {"990:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("99.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "99.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "990:1"));
  injectLocalRoutesAtRuntime({"99.1.0.0/16"}, {"991:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("99.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "99.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "991:1"));
  injectLocalRoutesAtRuntime({"99.2.0.0/16"}, {"992:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("99.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "99.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "992:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Verify detached peer has non-zero RIB version */
  auto adjRib = getAdjRib(kPeerAddr3);
  ASSERT_NE(adjRib, nullptr);
  EXPECT_GT(adjRib->getLastSeenRibVersion(), 0)
      << "Detached peer should have non-zero lastSeenRibVersion";
  EXPECT_GT(adjRib->getDetachedRibVersion(), 0)
      << "Detached peer should have non-zero detachedRibVersion";

  /* Group should also have non-zero version */
  auto group = getUpdateGroupForPeer(kPeerAddr3);
  ASSERT_NE(group, nullptr);
  EXPECT_GT(group->getLastSeenRibVersion(), 0)
      << "Group should have non-zero lastSeenRibVersion";

  XLOG(INFO, "=== TEST PASSED: RibVersionSetOnDetach ===");
}

/*
 * Test: Group with 4 peers, detach 2, bring 1 down, verify tracking.
 * Exercises detachedPeerBits management with multiple detachments
 * and selective cleanup.
 */
TEST_P(UpdateGroupDetachmentTest, MultiDetachWithSelectiveCleanup) {
  XLOG(INFO, "=== TEST: MultiDetachWithSelectiveCleanup ===");

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

  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Detach peer3 */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"10.10.0.0/16"}, {"1010:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1010:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.10.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1010:1"));
  injectLocalRoutesAtRuntime({"10.11.0.0/16"}, {"1011:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.11.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.11.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1011:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.11.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1011:1"));
  injectLocalRoutesAtRuntime({"10.12.0.0/16"}, {"1012:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.12.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.12.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1012:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.12.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1012:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1);

  /* Now detach peer5 too */
  blockPeer(kPeerAddr5);
  injectLocalRoutesAtRuntime({"10.13.0.0/16"}, {"1013:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.13.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1013:1"));
  injectLocalRoutesAtRuntime({"10.14.0.0/16"}, {"1014:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.14.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.14.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1014:1"));
  injectLocalRoutesAtRuntime({"10.15.0.0/16"}, {"1015:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.15.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.15.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1015:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId5));
  EXPECT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 2);

  /* Bring down ONLY peer3 (leave peer5 detached) */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Detached count should drop to 1 (only peer5 remains detached) */
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);
  EXPECT_TRUE(isPeerDetached(kPeerAddr5));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: MultiDetachWithSelectiveCleanup ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupDetachmentTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
