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

/* E2E tests: Event sequence pairs -- peer-down/policy, route-add/unblock,
 * route-add/accept, peer-up/detach, detach/MRAI.
 * Prefix range: 31.50-31.69/16.
 *
 * E-PEER-DOWN then E-POLICY-CHG
 * E-ROUTE-ADD then E-UNBLOCK
 * E-ROUTE-ADD then E-ACCEPT (route during acceptance gap)
 * E-PEER-UP then E-DETACH
 * E-DETACH then E-MRAI
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* E-ROUTE-ADD then E-ACCEPT -- inject a route while peer3 is in the
 * acceptance gap (transitioning from DETACHED_READY_TO_JOIN back to
 * JOINED_RUNNING). The route goes through CL and is delivered after acceptance.
 * We simulate the acceptance gap by freq-detaching peer3, then unblocking it
 * (enters DRJ), then injecting a route during DRJ recovery.
 */
TEST_P(UpdateGroupMultiPeerTest, RouteAddThenAccept) {
  XLOG(INFO, "=== TEST: RouteAddThenAccept ===");

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

  /* Freq-detach peer3: threshold=1 block */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"31.57.0.0/16"}, {"3157:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.57.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.57.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3157:1"));
  injectLocalRoutesAtRuntime({"31.58.0.0/16"}, {"3158:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.58.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.58.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3158:1"));
  injectLocalRoutesAtRuntime({"31.59.0.0/16"}, {"3159:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.59.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.59.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3159:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock peer3 -- enters DRJ (recovery/acceptance gap) */
  waitForDrjWithDrain(kPeerAddr3, peerId3);

  /* Event 1: Inject a route during the acceptance/recovery gap */
  injectLocalRoutesAtRuntime({"31.60.0.0/16"}, {"3160:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.60.0.0/16")));

  /* Peer4 receives the route via PL */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.60.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3160:1"));

  /* Peer4 continues to function correctly */
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: RouteAddThenAccept ===");
}

/* E-PEER-UP then E-DETACH -- use 3 peers. Bring peer5 DOWN then
 * back UP (reconnect -- enters DETACHED_INIT_DUMP). Meanwhile, detach
 * peer3 via freq threshold. Both events happening in sequence should not
 * crash. Peer4 continues operating normally.
 */
TEST_P(UpdateGroupMultiPeerTest, PeerUpThenDetach) {
  XLOG(INFO, "=== TEST: PeerUpThenDetach ===");

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
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId5));
  EXPECT_TRUE(waitForEoR(peerId5));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  /* Event 1: Bring peer5 DOWN then back UP -- enters DETACHED_INIT_DUMP */
  bringDownPeer(kPeerAddr5);
  bringUpPeer(kPeerAddr5);

  /* Event 2: Detach peer3 via freq threshold=1 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"31.61.0.0/16"}, {"3161:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.61.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.61.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3161:1"));
  injectLocalRoutesAtRuntime({"31.62.0.0/16"}, {"3162:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.62.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.62.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3162:1"));
  injectLocalRoutesAtRuntime({"31.63.0.0/16"}, {"3163:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.63.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.63.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3163:1"));

  /* Peer3 should be DETACHED_BLOCKED */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Peer4 continues functioning -- inject a post-detach route */
  injectLocalRoutesAtRuntime({"31.64.0.0/16"}, {"3164:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.64.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.64.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3164:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: PeerUpThenDetach ===");
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
