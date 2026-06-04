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
 * E2E tests: G-WAITING advanced event handling tests.
 *
 * Prefix range: 77.x.0.0/16
 *
 * Tests:
 *   G-WAITING x E-PEER-UP - New peer during PL drain
 *   G-WAITING x E-POLICY-CHG - Policy change mid-PL drain
 *   G-WAITING x E-MRAI-FIRE - MRAI during WAITING, deferred
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {
/*
 * G-WAITING x E-PEER-UP - New peer joins during PL drain.
 * 3-peer setup. Block peer3 -> WAITING. Bring peer5 DOWN, then UP.
 * Reconnecting peer enters DETACHED_INIT_DUMP independently.
 * Peers 3,4 continue their PL drain/unblock cycle.
 */
TEST_P(UpdateGroupMultiPeerTest, GWaiting_PeerUp) {
  XLOGF(INFO, "=== TEST: GWaiting_PeerUp ===");

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

  /* Bring peer5 DOWN first */
  bringDownPeer(kPeerAddr5);
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::DOWN));

  /* Block peer3 and fill queue -> WAITING */
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("77.{}.0.0/16", 10 + i);
    auto community = fmt::format("{}:1", 7710 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("77.{}.0.0", 10 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Bring peer5 back UP during WAITING - enters DETACHED_INIT_DUMP */
  bringUpPeer(kPeerAddr5);
  sendEoRToPeer(peerId5);
  EXPECT_TRUE(waitForEoR(peerId5));
  EXPECT_TRUE(waitForEoR(peerId5));

  /* Unblock peer3 to resolve WAITING state */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Drain peer5's init dump */
  drainPeerQueueCompletely(peerId5);

  /* Verify peer3 and peer4 still work */
  injectLocalRoutesAtRuntime({"77.15.0.0/16"}, {"7715:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("77.15.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "77.15.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "7715:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "77.15.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "7715:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOGF(INFO, "=== TEST PASSED: GWaiting_PeerUp ===");
}

/*
 * G-WAITING x E-POLICY-CHG - Policy change mid-PL drain.
 * Block peer3 -> WAITING. Withdraw a route (CL item). After unblock,
 * consume the CL-origin withdrawal with verifyRouteWithdraw on both peers,
 * then inject new prefix (simulating re-evaluation).
 */
TEST_P(UpdateGroupMultiPeerTest, GWaiting_PolicyChange) {
  XLOGF(INFO, "=== TEST: GWaiting_PolicyChange ===");

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

  /* Inject a shared route we will withdraw during WAITING */
  injectLocalRoutesAtRuntime({"77.20.0.0/16"}, {"7720:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("77.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "77.20.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "7720:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "77.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "7720:1"));

  /* Block peer3 and fill queue -> WAITING */
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("77.{}.0.0/16", 21 + i);
    auto community = fmt::format("{}:1", 7721 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("77.{}.0.0", 21 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Withdraw the shared route while WAITING - becomes CL item */
  withdrawLocalRoutesAtRuntime({"77.20.0.0/16"});

  /* Unblock peer3 - PL drains, then CL-origin withdrawal delivered */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Consume CL-origin withdrawal on both peers */
  EXPECT_TRUE(verifyRouteWithdraw("v4", "77.20.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "77.20.0.0", 16, kPeerAddr4));

  /* Inject new prefix (simulating re-evaluation after policy change) */
  injectLocalRoutesAtRuntime({"77.25.0.0/16"}, {"7725:1"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("77.25.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "77.25.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "7725:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "77.25.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "7725:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOGF(INFO, "=== TEST PASSED: GWaiting_PolicyChange ===");
}

/*
 * G-WAITING x E-MRAI-FIRE - MRAI fires during WAITING.
 * Block peer3 -> WAITING. Inject a route (CL item - triggers MRAI cycle).
 * After unblock, consume the CL-origin route with verifyRouteAdd on
 * both peers, then confirm recovery.
 */
TEST_P(UpdateGroupMultiPeerTest, GWaiting_MraiFire) {
  XLOGF(INFO, "=== TEST: GWaiting_MraiFire ===");

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

  /* Block peer3 and fill queue -> WAITING */
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("77.{}.0.0/16", 30 + i);
    auto community = fmt::format("{}:1", 7730 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("77.{}.0.0", 30 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Inject a route during WAITING - CL item, triggers MRAI cycle */
  injectLocalRoutesAtRuntime({"77.35.0.0/16"}, {"7735:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("77.35.0.0/16")));

  /* Unblock peer3 - PL drains, then CL-origin route delivered */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Consume CL-origin route on both peers */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "77.35.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "7735:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "77.35.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "7735:1"));

  /* Confirm recovery */
  injectLocalRoutesAtRuntime({"77.36.0.0/16"}, {"7736:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("77.36.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "77.36.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "7736:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "77.36.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "7736:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOGF(INFO, "=== TEST PASSED: GWaiting_MraiFire ===");
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
