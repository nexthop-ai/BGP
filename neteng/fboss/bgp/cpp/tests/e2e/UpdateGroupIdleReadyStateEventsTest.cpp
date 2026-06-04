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

/* E2E tests: G-IDLE and G-READY state event coverage.
 * Prefix range: 23.x.0.0/16.
 *
 * G-IDLE × E-ROUTE-REFRESH (simulated with route burst)
 * G-IDLE × E-EOR (N/A — EoR on running peer is no-op)
 * G-READY × E-ROUTE-WD (withdrawal during CL processing)
 * G-READY × E-UNBLOCK (N/A — no blocked peers)
 * G-READY × E-SLOW-FREQ (freq threshold during READY → detach)
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {
/* G-IDLE × E-EOR — N/A. EoR on a running peer is harmless no-op. */
TEST_P(UpdateGroupMultiPeerTest, GIdle_EorNoop) {
  XLOG(INFO, "=== TEST: GIdle_EorNoop ===");

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

  /* Send extra EoR to running peer — should be harmless */
  sendEoRToPeer(peerId3);

  /* Both peers still JOINED_RUNNING */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Verify route delivery still works */
  injectLocalRoutesAtRuntime({"23.10.0.0/16"}, {"2310:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("23.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "23.10.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2310:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "23.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2310:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOG(INFO, "=== TEST PASSED: GIdle_EorNoop ===");
}

/* G-READY × E-ROUTE-WD — Withdrawal during CL processing.
 * Inject a route, then withdraw it. Both operations go through CL.
 * Must use different prefix for withdrawal to avoid CL suppression. */
TEST_P(UpdateGroupMultiPeerTest, GReady_RouteWithdrawal) {
  XLOG(INFO, "=== TEST: GReady_RouteWithdrawal ===");

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

  /* Inject a route first */
  injectLocalRoutesAtRuntime({"23.20.0.0/16"}, {"2320:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("23.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "23.20.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2320:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "23.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2320:1"));

  /* Withdraw the route — exercises CL withdrawal during READY/IDLE */
  withdrawLocalRoutesAtRuntime({"23.20.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "23.20.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "23.20.0.0", 16, kPeerAddr4));

  /* Inject another route to confirm group still works */
  injectLocalRoutesAtRuntime({"23.21.0.0/16"}, {"2321:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("23.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "23.21.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2321:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "23.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2321:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOG(INFO, "=== TEST PASSED: GReady_RouteWithdrawal ===");
}

/* G-READY × E-UNBLOCK — N/A. Unblock on a running peer is no-op. */
TEST_P(UpdateGroupMultiPeerTest, GReady_UnblockNoop) {
  XLOG(INFO, "=== TEST: GReady_UnblockNoop ===");

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

  /* Inject a route to make group process CL (READY/active) */
  injectLocalRoutesAtRuntime({"23.30.0.0/16"}, {"2330:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("23.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "23.30.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2330:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "23.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2330:1"));

  /* Unblock a peer that is not blocked — should be harmless no-op */
  EXPECT_FALSE(isPeerQueueBlocked(peerId3));
  unblockPeer(kPeerAddr3);

  /* Both peers still JOINED_RUNNING */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Verify route delivery still works after spurious unblock */
  injectLocalRoutesAtRuntime({"23.31.0.0/16"}, {"2331:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("23.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "23.31.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2331:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "23.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2331:1"));

  verifySlowPeerInvariants(kPeerAddr3);

  XLOG(INFO, "=== TEST PASSED: GReady_UnblockNoop ===");
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
