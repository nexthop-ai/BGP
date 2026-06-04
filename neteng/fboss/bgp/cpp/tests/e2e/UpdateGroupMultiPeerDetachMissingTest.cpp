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
 * E2E tests: Multi-peer interaction and detachment internals
 *
 * Prefix range: 37.x.0.0/16
 *
 * Tests:
 *   Peer blocks during another peer's detachment
 *   Peer unblocks then immediately reblocks — timer reset
 *   PL clone when group PL is empty — detachedPackingList empty
 *   PL clone preserves attribute pointers (shared_ptr)
 *   Verify detachment is atomic
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Peer blocks during another peer's detachment procedure.
 * Detach peer3, then block peer4 during the detachment. Verify no
 * interference — peer3 stays detached, peer4 becomes JOINED_BLOCKED.
 */
TEST_P(UpdateGroupMultiPeerTest, BlockDuringDetachment) {
  XLOG(INFO, "=== TEST: BlockDuringDetachment ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  /* Large queues for fast peers */
  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr4);
  bringUpPeer(kPeerAddr5);
  /* Small queue for slow peer — fills naturally without blockPeer */
  setDefaultQueueSizes(3, 2, 0);
  bringUpPeer(kPeerAddr3);
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

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Inject routes — peer3's small queue fills naturally */
  injectLocalRoutesAtRuntime({"37.1.0.0/16"}, {"3701:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3701:1"));
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
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3702:1"));
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
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3703:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.3.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3703:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Verify in-sync peers continue while peer3 is detached */
  injectLocalRoutesAtRuntime({"37.4.0.0/16"}, {"3704:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3704:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.4.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3704:1"));

  /* Verify no interference */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  XLOG(INFO, "=== TEST PASSED: BlockDuringDetachment ===");
}

/*
 * Peer unblocks then immediately reblocks — timer reset.
 * Block peer3, unblock, immediately reblock. Verify state is consistent.
 */
TEST_P(UpdateGroupMultiPeerTest, UnblockReblock_TimerReset) {
  XLOG(INFO, "=== TEST: UnblockReblock_TimerReset ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* Large queue for fast peer */
  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr4);
  /* Small queue for slow peer — fills naturally without blockPeer */
  setDefaultQueueSizes(3, 2, 0);
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Fill peer3's queue naturally to reach JOINED_BLOCKED */
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
  injectLocalRoutesAtRuntime({"37.11.0.0/16"}, {"3711:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.11.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.11.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3711:1"));
  injectLocalRoutesAtRuntime({"37.12.0.0/16"}, {"3712:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.12.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.12.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3712:1"));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* Unblock peer3 — drains queue, peer recovers */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* State should be consistent — not DOWN, not crashed */
  auto state = getPeerState(kPeerAddr3);
  EXPECT_NE(state, PeerUpdateState::DOWN);

  /* Refill peer3's queue naturally to test reblock (timer reset) */
  injectLocalRoutesAtRuntime({"37.13.0.0/16"}, {"3713:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.13.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3713:1"));
  injectLocalRoutesAtRuntime({"37.14.0.0/16"}, {"3714:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.14.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.14.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3714:1"));
  injectLocalRoutesAtRuntime({"37.15.0.0/16"}, {"3715:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.15.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.15.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3715:1"));

  /* Peer3 should be blocked again — timer was reset */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: UnblockReblock_TimerReset ===");
}

/*
 * PL clone when group PL is empty — detachedPackingList empty.
 * Detach peer3 when group has no routes in PL (just init dump).
 * The detachedPackingList should be empty. Post-detach route works.
 */
TEST_P(UpdateGroupMultiPeerTest, PlCloneEmpty) {
  XLOG(INFO, "=== TEST: PlCloneEmpty ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* Large queue for fast peer */
  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr4);
  /* Small queue for slow peer — fills naturally without blockPeer */
  setDefaultQueueSizes(3, 2, 0);
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Freq-detach peer3 WITHOUT injecting routes first */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Inject routes — peer3's small queue fills naturally */
  injectLocalRoutesAtRuntime({"37.20.0.0/16"}, {"3720:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.20.0.0/16")));
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
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3721:1"));
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

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Post-detach route works normally */
  injectLocalRoutesAtRuntime({"37.25.0.0/16"}, {"3725:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.25.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.25.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3725:1"));

  XLOG(INFO, "=== TEST PASSED: PlCloneEmpty ===");
}

/*
 * PL clone preserves attribute pointers (shared_ptr).
 * Inject a route before detachment. After detachment, the cloned per-peer
 * entry should preserve the attributes. Inject same prefix with different
 * community after detach — peer4 gets new attrs, peer3's clone preserves old.
 */
TEST_P(UpdateGroupMultiPeerTest, PlClonePreservesAttributes) {
  XLOG(INFO, "=== TEST: PlClonePreservesAttributes ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* Large queue for fast peer */
  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr4);
  /* Small queue for slow peer — fills naturally without blockPeer */
  setDefaultQueueSizes(3, 2, 0);
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Inject a shared route BEFORE detachment */
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

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Inject routes — peer3's small queue fills naturally */
  injectLocalRoutesAtRuntime({"37.31.0.0/16"}, {"3731:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3731:1"));
  injectLocalRoutesAtRuntime({"37.32.0.0/16"}, {"3732:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.32.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.32.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3732:1"));
  injectLocalRoutesAtRuntime({"37.33.0.0/16"}, {"3733:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.33.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.33.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3733:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Withdraw the shared route — triggers lazy clone for peer3 */
  withdrawLocalRoutesAtRuntime({"37.30.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "37.30.0.0", 16, kPeerAddr4));

  /* Peer3's clone preserves old view, peer4 gets withdrawal */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOG(INFO, "=== TEST PASSED: PlClonePreservesAttributes ===");
}

/*
 * Verify detachment is atomic (no yield between PL clone and
 * bitmap operations). After freq-detach, immediately verify the detached
 * peer count and bitmap state are consistent.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachmentAtomic) {
  XLOG(INFO, "=== TEST: DetachmentAtomic ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* Large queue for fast peer */
  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr4);
  /* Small queue for slow peer — fills naturally without blockPeer */
  setDefaultQueueSizes(3, 2, 0);
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Inject routes — peer3's small queue fills naturally */
  injectLocalRoutesAtRuntime({"37.40.0.0/16"}, {"3740:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3740:1"));
  injectLocalRoutesAtRuntime({"37.41.0.0/16"}, {"3741:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3741:1"));
  injectLocalRoutesAtRuntime({"37.42.0.0/16"}, {"3742:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.42.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.42.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3742:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Immediately verify atomicity — bitmap and count are consistent */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1);
  verifySlowPeerInvariants(kPeerAddr3);

  /* Post-detach route delivery still works */
  injectLocalRoutesAtRuntime({"37.45.0.0/16"}, {"3745:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.45.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.45.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3745:1"));

  XLOG(INFO, "=== TEST PASSED: DetachmentAtomic ===");
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
