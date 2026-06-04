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
 * E2E tests: Peer Blocked/Detached Route Handling
 * Tests for route add/withdraw events in P-JB and P-DB peer states.
 *
 * Test plan:
 * https://docs.google.com/document/d/11lBp_Q_i6UYocI3meYbI3sUShZzZsu6Qlq8iSCdVXRc
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * P-JB x E-ROUTE-ADD
 * Route arrives while peer is JOINED_BLOCKED — CL grows, group continues
 * processing. Fast peer receives the route; blocked peer stays blocked.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, JoinedBlocked_RouteAdd) {
  XLOG(INFO, "=== TEST: JoinedBlocked_RouteAdd ===");

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

  /* Set high threshold so blocking alone doesn't trigger detachment */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      100,
      std::chrono::milliseconds(60000));

  /* Block peer3 and fill its queue to reach JOINED_BLOCKED */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"11.31.0.0/16"}, {"1131:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1131:1"));
  injectLocalRoutesAtRuntime({"11.32.0.0/16"}, {"1132:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.32.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.32.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1132:1"));
  injectLocalRoutesAtRuntime({"11.33.0.0/16"}, {"1133:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.33.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.33.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1133:1"));

  /* Confirm peer3 is JOINED_BLOCKED */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /*
   * Now inject ANOTHER route while peer3 is blocked.
   * The route goes to the CL. The group is in WAITING state because
   * the current PL has not drained for peer3 (blocked). Peer4 cannot
   * receive this route yet — it will be delivered once the PL drains.
   */
  injectLocalRoutesAtRuntime({"11.34.0.0/16"}, {"1134:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.34.0.0/16")));

  /* Peer3 should still be JOINED_BLOCKED, not detached */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));

  /*
   * Unblock peer3 to drain its queue and let the PL drain complete.
   * After unblocking, the group processes the CL entry (11.34.0.0/16)
   * and pushes it as a new PL to both peers.
   * Note: unblockPeer consumes peer3's queued messages internally.
   */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Now peer4 should receive the route that was queued in the CL */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.34.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1134:1"));

  /* Both peers back to normal */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));

  XLOG(INFO, "=== TEST PASSED: JoinedBlocked_RouteAdd ===");
}

/*
 * P-JB x E-ROUTE-WD
 * Withdrawal while peer is JOINED_BLOCKED — CL grows with withdrawal.
 * Fast peer receives the withdrawal; blocked peer stays blocked.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, JoinedBlocked_RouteWithdraw) {
  XLOG(INFO, "=== TEST: JoinedBlocked_RouteWithdraw ===");

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

  /* Set high threshold so blocking alone doesn't trigger detachment */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      100,
      std::chrono::milliseconds(60000));

  /*
   * Inject a route first (while both peers are running) so we can
   * withdraw it later while peer3 is blocked.
   */
  injectLocalRoutesAtRuntime({"11.35.0.0/16"}, {"1135:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.35.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.35.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1135:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.35.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1135:1"));

  /* Block peer3 and fill its queue to reach JOINED_BLOCKED */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"11.36.0.0/16"}, {"1136:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.36.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.36.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1136:1"));
  injectLocalRoutesAtRuntime({"11.37.0.0/16"}, {"1137:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.37.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.37.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1137:1"));
  injectLocalRoutesAtRuntime({"11.38.0.0/16"}, {"1138:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.38.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.38.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1138:1"));

  /* Confirm peer3 is JOINED_BLOCKED */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /*
   * Now withdraw 11.35.0.0/16 while peer3 is blocked.
   * The withdrawal goes to the CL. The group is in WAITING state
   * (current PL not drained for peer3) so peer4 can't receive it yet.
   */
  withdrawLocalRoutesAtRuntime({"11.35.0.0/16"});

  /* Peer3 should still be JOINED_BLOCKED, not detached */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));

  /*
   * Unblock peer3 to drain its queue and let the PL drain complete.
   * After unblocking, the group processes the CL withdrawal and
   * pushes it as a new PL to both peers.
   * Note: unblockPeer consumes peer3's queued messages internally.
   */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Now peer4 should receive the withdrawal */
  EXPECT_TRUE(verifyRouteWithdraw("v4", "11.35.0.0", 16, kPeerAddr4));

  /* Both peers back to normal */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  XLOG(INFO, "=== TEST PASSED: JoinedBlocked_RouteWithdraw ===");
}

/*
 * P-DB x E-ROUTE-ADD
 * Route arrives while peer is DETACHED_BLOCKED. The detached peer's CL grows;
 * lazy clone preserves the peer's diverged snapshot. Fast peer receives the
 * route normally.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, DetachedBlocked_RouteAdd) {
  XLOG(INFO, "=== TEST: DetachedBlocked_RouteAdd ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Dual-stack EoRs */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Set frequency threshold = 1 to trigger immediate detachment on block */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Block peer3 and fill queue to trigger detachment */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"11.39.0.0/16"}, {"1139:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.39.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.39.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1139:1"));
  injectLocalRoutesAtRuntime({"11.40.0.0/16"}, {"1140:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1140:1"));
  injectLocalRoutesAtRuntime({"11.41.0.0/16"}, {"1141:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1141:1"));

  /* Confirm peer3 reached DETACHED_BLOCKED */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);

  /*
   * Inject a new route while peer3 is DETACHED_BLOCKED.
   * The route should be processed by the group (peer4 gets it).
   * A CL entry accumulates for peer3's eventual recovery.
   */
  injectLocalRoutesAtRuntime({"11.42.0.0/16"}, {"1142:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.42.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.42.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1142:1"));

  /* Peer3 remains DETACHED_BLOCKED — route doesn't change its state */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);

  /* Peer4 still healthy */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: DetachedBlocked_RouteAdd ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupSlowPeerDetectionTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
