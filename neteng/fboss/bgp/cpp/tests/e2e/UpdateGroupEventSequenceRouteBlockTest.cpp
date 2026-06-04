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
 * E2E tests for event sequence pair interleavings.
 * E-ROUTE-ADD then E-BLOCK
 * E-ROUTE-WD then E-DETACH
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * E-ROUTE-ADD then E-BLOCK
 * Inject a route (both peers receive it), then block the slow peer.
 * Subsequent routes accumulate while blocked. Unblock and verify delivery.
 */
TEST_P(UpdateGroupMultiPeerTest, RouteAddThenBlock) {
  XLOG(INFO, "=== TEST: RouteAddThenBlock ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* Large queue for fast peer */
  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr4);

  /* Small queue for slow peer — fills naturally when blocked */
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

  /* Inject a route while both peers are running — both receive it */
  injectLocalRoutesAtRuntime({"30.31.0.0/16"}, {"3310:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.31.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3310:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3310:1"));

  /* Block peer3 AFTER first route was delivered */
  blockPeer(kPeerAddr3);

  /* Inject more routes — peer4 gets them, peer3 queues up */
  injectLocalRoutesAtRuntime({"30.32.0.0/16"}, {"3320:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.32.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.32.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3320:1"));

  injectLocalRoutesAtRuntime({"30.33.0.0/16"}, {"3330:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.33.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.33.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3330:1"));

  injectLocalRoutesAtRuntime({"30.34.0.0/16"}, {"3340:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.34.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.34.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3340:1"));

  /* Peer3's small queue fills past hwm=2 → JOINED_BLOCKED */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* Unblock peer3 — internally drains queued messages */
  unblockPeer(kPeerAddr3);

  /* Verify peer3 recovers to JOINED_RUNNING */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Inject a verification route — both peers should receive it */
  injectLocalRoutesAtRuntime({"30.35.0.0/16"}, {"3350:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.35.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.35.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3350:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.35.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3350:1"));

  verifySlowPeerInvariants(kPeerAddr3);
  XLOG(INFO, "=== TEST PASSED: RouteAddThenBlock ===");
}

/*
 * E-ROUTE-WD then E-DETACH
 * Inject a route, withdraw it (both peers get both). Then trigger
 * detachment via freq threshold. Withdrawal was fully processed
 * before detachment — no stale state.
 */
TEST_P(UpdateGroupMultiPeerTest, RouteWithdrawThenDetach) {
  XLOG(INFO, "=== TEST: RouteWithdrawThenDetach ===");

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

  /* Inject a route — both receive it */
  injectLocalRoutesAtRuntime({"30.41.0.0/16"}, {"3410:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.41.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3410:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3410:1"));

  /* Withdraw — both receive withdrawal */
  withdrawLocalRoutesAtRuntime({"30.41.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "30.41.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "30.41.0.0", 16, kPeerAddr4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Trigger detachment on peer3 via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);

  injectLocalRoutesAtRuntime({"30.42.0.0/16"}, {"3420:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.42.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.42.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3420:1"));

  injectLocalRoutesAtRuntime({"30.43.0.0/16"}, {"3430:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.43.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.43.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3430:1"));

  injectLocalRoutesAtRuntime({"30.44.0.0/16"}, {"3440:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.44.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.44.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3440:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Peer4 continues normally post-detach */
  injectLocalRoutesAtRuntime({"30.45.0.0/16"}, {"3450:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.45.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.45.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3450:1"));

  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: RouteWithdrawThenDetach ===");
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
