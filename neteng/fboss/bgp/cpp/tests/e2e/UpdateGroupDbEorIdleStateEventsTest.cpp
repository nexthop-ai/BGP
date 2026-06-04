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

/* E2E tests: P-DB × E-EOR and G-IDLE × Event coverage.
 * Prefix range: 22.x.0.0/16.
 *
 * P-DB × E-EOR (N/A — EoR on detached blocked peer)
 * G-IDLE × E-BLOCK (peer blocks from IDLE → WAITING)
 * G-IDLE × E-SLOW-DUR (N/A — no blocked peers in IDLE)
 * G-IDLE × E-PEER-UP (reconnecting peer joins during IDLE)
 * G-IDLE × E-CL-END (N/A — already at CL end)
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* P-DB × E-EOR — sendEoRToPeer on detached blocked peer is no-op */
TEST_P(UpdateGroupMultiPeerTest, DetachedBlocked_EorNoop) {
  XLOG(INFO, "=== TEST: DetachedBlocked_EorNoop ===");

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

  /* Detach peer3 via freq threshold (1 block = detach) */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"22.1.0.0/16"}, {"2201:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("22.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "22.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2201:1"));
  injectLocalRoutesAtRuntime({"22.2.0.0/16"}, {"2202:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("22.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "22.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2202:1"));
  injectLocalRoutesAtRuntime({"22.3.0.0/16"}, {"2203:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("22.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "22.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2203:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* N/A: send EoR to detached blocked peer — should be harmless no-op */
  sendEoRToPeer(peerId3);

  /* Peer3 stays DETACHED_BLOCKED */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Peer4 still works normally */
  injectLocalRoutesAtRuntime({"22.4.0.0/16"}, {"2204:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("22.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "22.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2204:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: DetachedBlocked_EorNoop ===");
}

/* G-IDLE × E-BLOCK — peer blocks while group is IDLE */
TEST_P(UpdateGroupMultiPeerTest, GIdle_Block) {
  XLOG(INFO, "=== TEST: GIdle_Block ===");

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

  /* Block peer3 and inject 3 routes (different communities) to fill queue */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"22.10.0.0/16"}, {"2210:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("22.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "22.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2210:1"));
  injectLocalRoutesAtRuntime({"22.11.0.0/16"}, {"2211:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("22.11.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "22.11.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2211:1"));
  injectLocalRoutesAtRuntime({"22.12.0.0/16"}, {"2212:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("22.12.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "22.12.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2212:1"));

  /* Peer3 should be JOINED_BLOCKED (queue full, capacity=3 >= hwm=2) */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* Group should be in WAITING (PL pending for blocked peer) */
  auto groupState = getGroupState(kPeerAddr3);
  XLOGF(INFO, "Group state after block: {}", static_cast<int>(groupState));
  EXPECT_EQ(groupState, UpdateGroupState::WAITING);

  /* Unblock and verify recovery */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOG(INFO, "=== TEST PASSED: GIdle_Block ===");
}

/* G-IDLE × E-SLOW-DUR — N/A, no blocked peers in IDLE */
TEST_P(UpdateGroupMultiPeerTest, GIdle_SlowDurNoop) {
  XLOG(INFO, "=== TEST: GIdle_SlowDurNoop ===");

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

  /* Set aggressive duration threshold — should NOT fire since no one is blocked
   */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(1),
      100,
      std::chrono::milliseconds(60000));

  /* Inject a route — both peers receive it normally */
  injectLocalRoutesAtRuntime({"22.20.0.0/16"}, {"2220:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("22.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "22.20.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2220:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "22.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2220:1"));

  /* Both peers still JOINED_RUNNING — no detachment */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: GIdle_SlowDurNoop ===");
}

/* G-IDLE × E-PEER-UP — reconnecting peer joins during IDLE */
TEST_P(UpdateGroupMultiPeerTest, GIdle_PeerUp) {
  XLOG(INFO, "=== TEST: GIdle_PeerUp ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  setupSlowPeerComponents(8, 6, 2);

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

  /* Inject a route so init dump has content when peer5 reconnects */
  injectLocalRoutesAtRuntime({"22.30.0.0/16"}, {"2230:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("22.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "22.30.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2230:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "22.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2230:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "22.30.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "2230:1"));

  /* Bring peer5 DOWN, then back UP during IDLE */
  bringDownPeer(kPeerAddr5);
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::DOWN));

  /* Peer5 reconnects — enters DETACHED_INIT_DUMP for existing group */
  bringUpPeer(kPeerAddr5);
  sendEoRToPeer(peerId5);

  /* Drain peer5's init dump queue */
  drainPeerQueueCompletely(peerId5);

  /* Peer3 and peer4 still JOINED_RUNNING */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Verify peer3+peer4 still receive new routes */
  injectLocalRoutesAtRuntime({"22.31.0.0/16"}, {"2231:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("22.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "22.31.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2231:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "22.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2231:1"));

  verifySlowPeerInvariants(kPeerAddr3);

  XLOG(INFO, "=== TEST PASSED: GIdle_PeerUp ===");
}

/* G-IDLE × E-CL-END — N/A, already at CL end in IDLE */
TEST_P(UpdateGroupMultiPeerTest, GIdle_ClEndNoop) {
  XLOG(INFO, "=== TEST: GIdle_ClEndNoop ===");

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

  /* Group is IDLE — CL is already at end. Verify stable state. */
  auto groupState = getGroupState(kPeerAddr3);
  XLOGF(INFO, "Group state: {}", static_cast<int>(groupState));

  /* Inject a route — both peers receive it, confirming CL processing works */
  injectLocalRoutesAtRuntime({"22.40.0.0/16"}, {"2240:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("22.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "22.40.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2240:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "22.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2240:1"));

  /* Both peers stable */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: GIdle_ClEndNoop ===");
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
