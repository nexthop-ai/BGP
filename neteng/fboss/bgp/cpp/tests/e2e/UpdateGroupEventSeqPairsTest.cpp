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

/* E2E tests: Event sequence pair ordering tests.
 * Prefix range: 31.30-31.49/16.
 *
 * E-ROUTE-ADD then E-BLOCK
 * E-ROUTE-WD then E-DETACH
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* E-ROUTE-ADD then E-BLOCK — inject a route that is sent to
 * both peers, then immediately block one peer. The route was already
 * delivered (queue was not full yet). Subsequent routes go to CL for
 * the blocked peer.
 */
TEST_P(UpdateGroupMultiPeerTest, RouteAddThenBlock) {
  XLOG(INFO, "=== TEST: RouteAddThenBlock ===");

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

  /* Inject a route FIRST — both peers receive it */
  injectLocalRoutesAtRuntime({"31.30.0.0/16"}, {"3130:1"}, 150);
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

  /* NOW block peer3 */
  blockPeer(kPeerAddr3);

  /* Inject more routes — peer3 queue fills, peer4 drains inline */
  injectLocalRoutesAtRuntime({"31.31.0.0/16"}, {"3131:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3131:1"));
  injectLocalRoutesAtRuntime({"31.32.0.0/16"}, {"3132:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.32.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.32.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3132:1"));
  injectLocalRoutesAtRuntime({"31.33.0.0/16"}, {"3133:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.33.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.33.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3133:1"));

  /* Peer3 should be JOINED_BLOCKED */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* Unblock peer3 — fill routes consumed by unblockPeer (R2) */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Inject a NEW route post-unblock to verify peer3 is operational */
  injectLocalRoutesAtRuntime({"31.34.0.0/16"}, {"3134:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.34.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.34.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3134:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.34.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3134:1"));

  verifySlowPeerInvariants(kPeerAddr3);

  XLOG(INFO, "=== TEST PASSED: RouteAddThenBlock ===");
}

/* E-ROUTE-WD then E-DETACH — withdraw a route, then trigger
 * freq-based detachment. The withdrawal is processed by both peers
 * BEFORE detachment. Post-detach, verify CL consumer works for the
 * detached peer and fast peer continues.
 */
TEST_P(UpdateGroupMultiPeerTest, RouteWdThenDetach) {
  XLOG(INFO, "=== TEST: RouteWdThenDetach ===");

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

  /* Inject a shared route first — both peers receive it */
  injectLocalRoutesAtRuntime({"31.40.0.0/16"}, {"3140:1"}, 150);
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

  /* Withdraw the route — both peers receive the withdrawal */
  withdrawLocalRoutesAtRuntime({"31.40.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "31.40.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "31.40.0.0", 16, kPeerAddr4));

  /* Now detach peer3 via freq threshold=1 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
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
  injectLocalRoutesAtRuntime({"31.42.0.0/16"}, {"3142:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.42.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.42.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3142:1"));
  injectLocalRoutesAtRuntime({"31.43.0.0/16"}, {"3143:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.43.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.43.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3143:1"));

  /* Peer3 should now be DETACHED_BLOCKED */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Post-detach route — goes to peer4 via PL, CL for peer3 */
  injectLocalRoutesAtRuntime({"31.44.0.0/16"}, {"3144:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.44.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.44.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3144:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: RouteWdThenDetach ===");
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
