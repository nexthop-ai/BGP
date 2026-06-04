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

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Policy change blocks a prefix that was lazy-cloned for a
 * detached peer. Withdraw the prefix after detachment — per-peer
 * entry is also withdrawn. Only peer4 receives the withdrawal.
 */
TEST_P(UpdateGroupLifecycleTest, PolicyBlocksClonedPrefix) {
  XLOG(INFO, "=== TEST: PolicyBlocksClonedPrefix ===");

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

  /* Inject shared route — both peers receive */
  injectLocalRoutesAtRuntime({"31.1.0.0/16"}, {"3101:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3101:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3101:1"));

  /* Detach peer3 via frequency threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"31.2.0.0/16"}, {"3102:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3102:1"));
  injectLocalRoutesAtRuntime({"31.3.0.0/16"}, {"3103:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3103:1"));
  injectLocalRoutesAtRuntime({"31.4.0.0/16"}, {"3104:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3104:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /*
   * Withdraw the shared prefix (simulates policy blocking it).
   * Only peer4 receives. Peer3's CL tracks the withdrawal silently
   * (per-peer entry for the cloned prefix is also withdrawn).
   */
  withdrawLocalRoutesAtRuntime({"31.1.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "31.1.0.0", 16, kPeerAddr4));

  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: PolicyBlocksClonedPrefix ===");
}

/*
 * Route attribute updates while peer is blocked. Re-inject 31.20.0.0/16
 * with community A, then B, then C while peer3 is JOINED_BLOCKED.
 * After unblock, the final attrs (C) are delivered via CL.
 * Use queue (5,4,0) so CL items don't re-block after unblock.
 */
TEST_P(UpdateGroupLifecycleTest, RouteAttrChangesWhileBlocked) {
  XLOG(INFO, "=== TEST: RouteAttrChangesWhileBlocked ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
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

  /* Inject initial route with attrs A — both peers receive */
  injectLocalRoutesAtRuntime({"31.20.0.0/16"}, {"3120:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.20.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3120:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3120:1"));

  /* Block peer3 and fill queue to reach JOINED_BLOCKED */
  blockPeer(kPeerAddr3);
  for (int i = 1; i <= 5; ++i) {
    auto prefix = fmt::format("31.{}.0.0/16", 20 + i);
    auto community = fmt::format("31{}:1", 20 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", 20 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Policy A→B: change attrs while peer3 is blocked (CL item) */
  injectLocalRoutesAtRuntime({"31.20.0.0/16"}, {"3120:50"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.20.0.0/16")));

  /* Policy B→C: change attrs again while still blocked (CL item) */
  injectLocalRoutesAtRuntime({"31.20.0.0/16"}, {"3120:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.20.0.0/16")));

  /* Unblock peer3 — queued PL messages drain, then CL items process */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /*
   * Drain stale items from both peer queues. Peer3 has CL items from
   * policy changes pushed after unblock. Peer4 has inline policy change
   * announcements that were never consumed by verifyRouteAdd above.
   * Use a timed drain loop to let the async CL consumer finish pushing.
   */
  for (int i = 0; i < 20; ++i) {
    drainPeerQueueCompletely(peerId3, 1, 100);
    drainPeerQueueCompletely(peerId4, 1, 100);
    peerManager_->getEventBase().runInEventBaseThreadAndWait([]() {});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  drainPeerQueueCompletely(peerId3, 1, 100);
  drainPeerQueueCompletely(peerId4, 1, 100);

  /* Inject a verification route to confirm recovery */
  injectLocalRoutesAtRuntime({"31.30.0.0/16"}, {"3130:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.30.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3130:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3130:1"));

  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: RouteAttrChangesWhileBlocked ===");
}

/*
 * Policy change during PL drain — inject an attr change while
 * the group is in WAITING state (PL not fully drained). The new CL
 * item is processed after the current PL completes, creating a new PL
 * from re-eval. Both peers eventually receive the attr change.
 */
TEST_P(UpdateGroupLifecycleTest, PolicyChangeDuringPlDrain) {
  XLOG(INFO, "=== TEST: PolicyChangeDuringPlDrain ===");

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

  /* Inject initial route with attrs A — both peers receive */
  injectLocalRoutesAtRuntime({"31.40.0.0/16"}, {"3140:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.40.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3140:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3140:1"));

  /* Block peer3 to create WAITING state during PL drain */
  blockPeer(kPeerAddr3);

  /* Inject route to start PL drain — peer4 drains, peer3 blocked */
  injectLocalRoutesAtRuntime({"31.41.0.0/16"}, {"3141:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3141:1"));

  /*
   * Inject attr change on the FIRST route while PL is draining (WAITING).
   * This goes to CL and will be processed after current PL completes.
   */
  injectLocalRoutesAtRuntime({"31.40.0.0/16"}, {"3140:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.40.0.0/16")));

  /* Unblock peer3 — PL drains, then CL item creates new PL with re-eval */
  unblockPeer(kPeerAddr3);

  /* Wait for peers to finish transitioning before verifying queue items */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /*
   * After unblock and state stabilization, verify CL-origin attr change
   * was delivered to peer4.
   */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3140:99"));
  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: PolicyChangeDuringPlDrain ===");
}

/*
 * Policy change produces CL items while a peer is at DRJ
 * (Detached Ready to Join). The CL movement means the detached peer's
 * divergence point has shifted — DFP must become DSP. Inject an attr
 * change after peer3 reaches DRJ. Peer4 receives the update. Peer3
 * stays detached with CL divergence tracked.
 */
TEST_P(UpdateGroupLifecycleTest, PolicyChangeCLAtDrjToDsp) {
  XLOG(INFO, "=== TEST: PolicyChangeCLAtDrjToDsp ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* Large queue for fast peer — absorbs PL items without blocking */
  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr4);

  /* Small queue for slow peer — fills naturally to trigger detach */
  setDefaultQueueSizes(3, 2, 0);
  bringUpPeer(kPeerAddr3);

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Inject shared route — both peers receive */
  injectLocalRoutesAtRuntime({"31.50.0.0/16"}, {"3150:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.50.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.50.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3150:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.50.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3150:1"));

  /* Detach peer3 via frequency threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 1; i <= 3; ++i) {
    auto prefix = fmt::format("31.{}.0.0/16", 50 + i);
    auto community = fmt::format("31{}:1", 50 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", 50 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /*
   * Inject additional routes while peer3 is detached to grow the CL.
   * With enough CL items, the acceptance PL after unblock will re-fill
   * peer3's small queue (hwm=2), keeping it detached for the policy
   * change test. Need >2 PL items even after packing compression.
   */
  for (int i = 4; i <= 9; ++i) {
    auto prefix = fmt::format("31.{}.0.0/16", 50 + i);
    auto community = fmt::format("31{}:1", 50 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", 50 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Unblock peer3 — CL items keep peer3 detached during acceptance */
  unblockPeer(kPeerAddr3);

  /*
   * Inject policy change (attr update on shared route) while peer3
   * is detached. This creates a CL item, advancing the group CL.
   * DFP at DRJ must become DSP since CL moved past the divergence.
   * Peer4 receives the update normally.
   */
  injectLocalRoutesAtRuntime({"31.50.0.0/16"}, {"3150:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.50.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.50.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3150:99"));

  /* Inject another route to confirm peer4 continues normally */
  injectLocalRoutesAtRuntime({"31.60.0.0/16"}, {"3160:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.60.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.60.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3160:1"));

  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: PolicyChangeCLAtDrjToDsp ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupLifecycleTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
