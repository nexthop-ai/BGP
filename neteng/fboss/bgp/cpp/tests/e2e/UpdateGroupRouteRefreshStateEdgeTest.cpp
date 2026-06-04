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

/* E2E tests: Route Refresh across various peer/group states (PART 12)
 * Prefix range: 31.x.0.0/16
 *
 * Route refresh for JOINED_BLOCKED peer — burst while blocked
 * Route refresh for DRJ peer — conflicting state transitions
 * Route refresh for peer during INIT — defer
 * Route refresh for in-sync peer while another is detached
 * Route refresh followed by peer DOWN — clean abort
 *
 * No sendRouteRefresh helper exists in E2E. Route refresh is simulated by
 * injecting bursts of routes with different communities (each triggers a
 * separate MRAI cycle), exercising the same group processing paths.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Route refresh for JOINED_BLOCKED peer
 * Block peer3 (JOINED_BLOCKED), then simulate route refresh by injecting
 * a burst of routes. Peer4 receives all routes inline. Peer3's queue
 * accumulates. After unblock, peer3 drains all queued routes. No crash.
 */
TEST_P(UpdateGroupMultiPeerTest, JoinedBlocked_RouteRefreshBurst) {
  XLOG(INFO, "=== TEST: JoinedBlocked_RouteRefreshBurst ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  /* Large capacity prevents queue-full blocking; low hwm triggers
   * JOINED_BLOCKED quickly after 4 route pushes */
  setupSlowPeerComponents(100, 4, 0);

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

  /* Disable slow peer detachment — we want JOINED_BLOCKED, not DETACHED */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1000,
      std::chrono::milliseconds(600000));

  /* Block peer3 (test-side) and fill queue past hwm=4 */
  blockPeer(kPeerAddr3);
  for (int i = 1; i <= 5; ++i) {
    auto prefix = fmt::format("31.{}.0.0/16", i);
    auto community = fmt::format("31{:02d}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Simulate route refresh burst while peer3 is JOINED_BLOCKED.
   * Group is WAITING (PL pending for blocked peer3) — burst routes
   * go to CL and are NOT pushed to peer4 until PL completes. */
  for (int i = 6; i <= 8; ++i) {
    auto prefix = fmt::format("31.{}.0.0/16", i);
    auto community = fmt::format("31{:02d}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
  }

  /* Peer3 still JOINED_BLOCKED, peer4 still JOINED_RUNNING */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  verifySlowPeerInvariants(kPeerAddr4);

  /* Unblock peer3 — PL completes, CL items delivered to both peers */
  unblockPeer(kPeerAddr3);
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  XLOG(INFO, "=== TEST PASSED: JoinedBlocked_RouteRefreshBurst ===");
}

/*
 * Route refresh for DETACHED_READY_TO_JOIN peer
 * Peer3 reaches DRJ after freq-detach and unblock. Simulate route refresh
 * burst during recovery. Routes delivered to peer4 normally. Peer3 continues
 * recovery without crash.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachedReadyToJoin_RouteRefreshBurst) {
  XLOG(INFO, "=== TEST: DetachedReadyToJoin_RouteRefreshBurst ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* Large queue for fast peer4 */
  setupSlowPeerComponents(10, 8, 0);
  bringUpPeer(kPeerAddr4);

  /* Small queue for slow peer3 — fills naturally to trigger detach */
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

  /* Detach peer3 via freq threshold (1 block = detach) */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  /* Inject 3 routes — peer3's small queue fills, triggers detach */
  injectLocalRoutesAtRuntime({"31.20.0.0/16"}, {"3120:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3120:1"));
  injectLocalRoutesAtRuntime({"31.21.0.0/16"}, {"3121:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3121:1"));
  injectLocalRoutesAtRuntime({"31.22.0.0/16"}, {"3122:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.22.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.22.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3122:1"));
  drainPeerQueueCompletely(peerId4);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock peer3 and drain its queue to let recovery start.
   * Without the unblock, peer3 stays DB forever and cannot transition
   * to DRJ (matches the pattern used in the other 31 DRJ tests). */
  testOnlyDeferDrjAcceptance(kPeerAddr3, true);
  unblockPeer(kPeerAddr3);
  {
    for (int i = 0; i < 20; ++i) {
      if (getPeerState(kPeerAddr3) == PeerUpdateState::DETACHED_READY_TO_JOIN) {
        break;
      }
      drainPeerQueueCompletely(peerId3, 1, 100);
      drainPeerQueueCompletely(peerId4, 1, 100);
      peerManager_->getEventBase().runInEventBaseThreadAndWait([]() {});
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    drainPeerQueueCompletely(peerId3, 1, 100);
    drainPeerQueueCompletely(peerId4, 1, 100);
  }
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_READY_TO_JOIN));
  testOnlyDeferDrjAcceptance(kPeerAddr3, false);

  /* Simulate route refresh burst during recovery.
   * Burst uses distinct prefixes (23-25) to avoid overlap with fill (20-22). */
  for (int i = 23; i <= 25; ++i) {
    auto prefix = fmt::format("31.{}.0.0/16", i);
    auto community = fmt::format("31{:02d}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Peer4 still running after burst */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: DetachedReadyToJoin_RouteRefreshBurst ===");
}

/*
 * Route refresh for peer during INIT
 * Peer3 is in INIT (no EoR sent yet). Simulate route refresh burst. Peer4
 * receives all routes inline. Peer3 receives them via CL after EoR. Verifies
 * route refresh during init is safely deferred.
 */
TEST_P(UpdateGroupMultiPeerTest, InitState_RouteRefreshDefer) {
  XLOG(INFO, "=== TEST: InitState_RouteRefreshDefer ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /* Complete peer4 first so it can receive routes */
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  /* Peer3 still in INIT — no EoR sent yet */

  /* Simulate route refresh burst while peer3 is in INIT */
  for (int i = 30; i <= 32; ++i) {
    auto prefix = fmt::format("31.{}.0.0/16", i);
    auto community = fmt::format("31{:02d}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    /* Only peer4 receives; peer3 in INIT, routes go to CL */
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Now send EoR to peer3 to complete INIT */
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Drain peer3's queue — init dump + CL items */
  drainPeerQueueCompletely(peerId3);

  /* Verify both peers running after CL drain */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOG(INFO, "=== TEST PASSED: InitState_RouteRefreshDefer ===");
}

/*
 * Route refresh for in-sync peer while another is detached
 * Peer3 is detached (DETACHED_BLOCKED). Simulate route refresh burst for
 * peer4 (still in-sync). Routes are delivered to peer4, CL accumulates
 * for peer3. Verify independence — peer4 is not affected by peer3's state.
 */
TEST_P(UpdateGroupMultiPeerTest, InSyncPeer_WhileOtherDetached) {
  XLOG(INFO, "=== TEST: InSyncPeer_WhileOtherDetached ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(3, 2, 0);

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

  /* Detach peer3 via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"31.40.0.0/16"}, {"3140:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3140:1"));
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
  /* 3rd fill route to ensure queue > hwm=2 */
  injectLocalRoutesAtRuntime({"31.39.0.0/16"}, {"3139:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.39.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.39.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3139:1"));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Peer3 is detached. Simulate route refresh burst for peer4 */
  for (int i = 42; i <= 45; ++i) {
    auto prefix = fmt::format("31.{}.0.0/16", i);
    auto community = fmt::format("31{:02d}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    /* Only peer4 receives — peer3 detached, CL accumulates */
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Peer3 still DETACHED_BLOCKED, peer4 still running */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1);
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: InSyncPeer_WhileOtherDetached ===");
}

/*
 * Route refresh followed by peer DOWN
 * Simulate route refresh burst (3 routes), then immediately bring peer3
 * DOWN. Verify no crash from in-flight route processing, peer4 continues
 * to receive routes normally after peer3 goes DOWN.
 */
TEST_P(UpdateGroupMultiPeerTest, RouteRefreshThenPeerDown) {
  XLOG(INFO, "=== TEST: RouteRefreshThenPeerDown ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

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

  /* Simulate route refresh burst: 3 routes, inject-drain one at a time */
  for (int i = 50; i <= 52; ++i) {
    auto prefix = fmt::format("31.{}.0.0/16", i);
    auto community = fmt::format("31{:02d}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Immediately bring peer3 DOWN after burst */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Peer4 continues to receive new routes after peer3 is DOWN */
  injectLocalRoutesAtRuntime({"31.53.0.0/16"}, {"3153:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.53.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.53.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3153:1"));

  /* Peer4 still running, peer3 is DOWN */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: RouteRefreshThenPeerDown ===");
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
