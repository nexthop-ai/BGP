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

/* E2E tests: Route Refresh with policy change, idle-detach, single-peer,
 * idempotent rapid requests, and full RIB-OUT re-announcement.
 * Prefix range: 31.x.0.0/16.
 *
 * Route refresh followed by policy change — policy takes precedence
 * Route refresh during G-IDLE — standard detach for RR
 * Route refresh for only peer in group — single-peer detach for RR
 * Two route refresh requests in rapid succession — idempotent
 * Route refresh produces full RIB-OUT re-announcement
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Route refresh followed by policy change — policy takes precedence.
 * Simulate route refresh as a burst of routes, then simulate policy change
 * via withdraw+re-inject with different prefix. The policy change supersedes
 * the refresh burst. Both peers receive all operations and stay stable. */
TEST_P(UpdateGroupMultiPeerTest, RouteRefreshThenPolicyChange) {
  XLOG(INFO, "=== TEST: RouteRefreshThenPolicyChange ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(10, 8, 0);

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

  /* Inject initial route */
  injectLocalRoutesAtRuntime({"31.1.0.0/16"}, {"3101:1"}, 150);
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

  /* Simulate route refresh burst — 2 more routes */
  injectLocalRoutesAtRuntime({"31.2.0.0/16"}, {"3102:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.2.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3102:1"));
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
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3103:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3103:1"));

  /* Policy change: withdraw old, inject new with different prefix/attrs */
  withdrawLocalRoutesAtRuntime({"31.1.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "31.1.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "31.1.0.0", 16, kPeerAddr4));

  injectLocalRoutesAtRuntime({"31.4.0.0/16"}, {"3104:1"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.4.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3104:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3104:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOG(INFO, "=== TEST PASSED: RouteRefreshThenPolicyChange ===");
}

/* Route refresh during G-IDLE — standard detach for RR.
 * Route refresh burst during IDLE, then freq-threshold detach. Detached
 * peer is removed cleanly while refresh activity continues for peer4. */
TEST_P(UpdateGroupMultiPeerTest, RouteRefreshIdleDetach) {
  XLOG(INFO, "=== TEST: RouteRefreshIdleDetach ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  /* Per-peer queue sizes: large for fast peer, small for slow peer */
  setDefaultQueueSizes(10, 8, 0);
  setupComponents();

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* Fast peer: large queue — won't block */
  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr4);

  /* Slow peer: small queue — fills naturally when blocked */
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

  /* Set aggressive freq threshold: 1 block = detach */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Block peer3 and fill queue */
  blockPeer(kPeerAddr3);

  injectLocalRoutesAtRuntime({"31.10.0.0/16"}, {"3110:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3110:1"));

  injectLocalRoutesAtRuntime({"31.11.0.0/16"}, {"3111:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.11.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.11.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3111:1"));

  injectLocalRoutesAtRuntime({"31.12.0.0/16"}, {"3112:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.12.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.12.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3112:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Continue refresh activity after detachment — peer4 still receives */
  injectLocalRoutesAtRuntime({"31.13.0.0/16"}, {"3113:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.13.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3113:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: RouteRefreshIdleDetach ===");
}

/* Route refresh for only peer in group — single-peer detach for RR.
 * Single peer test. Inject routes (simulating refresh), then block + trigger
 * freq detach. With only 1 peer, the group preserves it as last synced
 * member (N-1 detachments). */
TEST_P(UpdateGroupSlowPeerDetectionTest, SinglePeerRouteRefreshDetach) {
  XLOG(INFO, "=== TEST: SinglePeerRouteRefreshDetach ===");

  addPeer(kDefaultPeerSpec3);
  addLocalRoute("31.20.0.0/16", {"3120:1"}, 100);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Drain pre-loaded route from init dump */
  drainPeerQueueCompletely(peerId3);

  /* Simulate route refresh */
  injectLocalRoutesAtRuntime({"31.21.0.0/16"}, {"3121:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.21.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3121:1"));

  /* Try to detach — single peer preserved as last synced */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  /* Inject 4 routes to reach stable JOINED_BLOCKED.
   * Queue (3,2,0): routes 1-2 fill past hiWm=2, route 3 triggers
   * markPeerBlocked but deferred push succeeds (queue not full),
   * route 4 triggers markPeerBlocked and deferred push blocks
   * (queue full at capacity=3), keeping JOINED_BLOCKED stable. */
  injectLocalRoutesAtRuntime({"31.22.0.0/16"}, {"3122:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.22.0.0/16")));
  injectLocalRoutesAtRuntime({"31.24.0.0/16"}, {"3124:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.24.0.0/16")));
  injectLocalRoutesAtRuntime({"31.25.0.0/16"}, {"3125:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.25.0.0/16")));
  injectLocalRoutesAtRuntime({"31.26.0.0/16"}, {"3126:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.26.0.0/16")));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Single peer stays JOINED_BLOCKED (last synced member preserved).
   * Use waitForPeerState to poll — PeerUpdateState is set async by EventBase.
   */
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED, 200));

  /* Unblock — drains queued blocked messages automatically */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING, 200));

  /* Drain remaining messages including late-arriving deferred pushes */
  drainPeerQueueCompletely(peerId3);

  /* Post-recovery: verify route pipeline is functional after blocking.
   * Inject a new route and confirm it reaches the shadow RIB, proving
   * the full RIB processing path works post-recovery. */
  injectLocalRoutesAtRuntime({"31.23.0.0/16"}, {"3123:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.23.0.0/16")));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOG(INFO, "=== TEST PASSED: SinglePeerRouteRefreshDetach ===");
}

/* Two route refresh requests in rapid succession — idempotent.
 * Two rapid refresh bursts back-to-back. Both complete without interference.
 * Verify both peers stable throughout. */
TEST_P(UpdateGroupMultiPeerTest, TwoRapidRefreshIdempotent) {
  XLOG(INFO, "=== TEST: TwoRapidRefreshIdempotent ===");

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

  /* First refresh burst — 3 routes, inject-drain one at a time */
  for (int i = 1; i <= 3; ++i) {
    auto prefix = fmt::format("31.{}.0.0/16", 30 + i);
    auto community = fmt::format("31{}:1", 30 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", 30 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", 30 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Second refresh burst — 3 more routes */
  for (int i = 1; i <= 3; ++i) {
    auto prefix = fmt::format("31.{}.0.0/16", 40 + i);
    auto community = fmt::format("31{}:1", 40 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", 40 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", 40 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOG(INFO, "=== TEST PASSED: TwoRapidRefreshIdempotent ===");
}

/* Route refresh produces full RIB-OUT re-announcement.
 * Load 4 routes, drain. Withdraw all, then re-inject with new prefixes
 * simulating a full RIB-OUT refresh. Verify all withdrawals and new
 * announcements arrive at both peers. */
TEST_P(UpdateGroupMultiPeerTest, FullRibOutReannouncement) {
  XLOG(INFO, "=== TEST: FullRibOutReannouncement ===");

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

  /* Load initial RIB-OUT: 4 routes, inject-drain one at a time */
  for (int i = 1; i <= 4; ++i) {
    auto prefix = fmt::format("31.{}.0.0/16", 50 + i);
    auto community = fmt::format("31{}:1", 50 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", 50 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", 50 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Withdraw all 4 routes one at a time */
  for (int i = 1; i <= 4; ++i) {
    auto prefix = fmt::format("31.{}.0.0/16", 50 + i);
    withdrawLocalRoutesAtRuntime({prefix});
    EXPECT_TRUE(verifyRouteWithdraw(
        "v4", fmt::format("31.{}.0.0", 50 + i), 16, kPeerAddr3));
    EXPECT_TRUE(verifyRouteWithdraw(
        "v4", fmt::format("31.{}.0.0", 50 + i), 16, kPeerAddr4));
  }

  /* Re-announce with new prefixes (full RIB-OUT refresh) */
  for (int i = 1; i <= 4; ++i) {
    auto prefix = fmt::format("31.{}.0.0/16", 60 + i);
    auto community = fmt::format("31{}:1", 60 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", 60 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", 60 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOG(INFO, "=== TEST PASSED: FullRibOutReannouncement ===");
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
    UpdateGroupSlowPeerDetectionTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
