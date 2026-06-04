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
 * E2E tests: Peer Running State x Event Matrix
 * Tests for P-DOWN, P-INIT, and P-JR (Joined Running) peer states.
 *
 * Test plan:
 * https://docs.google.com/document/d/11lBp_Q_i6UYocI3meYbI3sUShZzZsu6Qlq8iSCdVXRc
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * P-INIT x E-PEER-DOWN
 * Peer goes down during initial dump (before EoR). Verify clean abort,
 * no leak, remaining peer continues functioning.
 * No pre-loaded routes so both peers start INIT together.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, PeerInit_PeerDown) {
  XLOG(INFO, "=== TEST: PeerInit_PeerDown ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* Bring up both peers together */
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /*
   * Bring peer3 down BEFORE sending EoR - it's in INIT state.
   * The peer has not completed its initial dump yet.
   */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Complete peer4's initial dump normally */
  sendEoRToPeer(peerId4);
  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Verify peer4 still receives new routes after peer3's abort */
  injectLocalRoutesAtRuntime({"11.3.0.0/16"}, {"1103:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1103:1"));

  XLOG(INFO, "=== TEST PASSED: PeerInit_PeerDown ===");
}

/*
 * P-JR x E-ROUTE-WD
 * Normal withdrawal: inject a route, then withdraw it.
 * Both JOINED_RUNNING peers should receive the withdrawal.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, JoinedRunning_RouteWithdraw) {
  XLOG(INFO, "=== TEST: JoinedRunning_RouteWithdraw ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(8, 6, 2);

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

  /* Inject a route first */
  injectLocalRoutesAtRuntime({"11.6.0.0/16"}, {"1106:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.6.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.6.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1106:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.6.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1106:1"));

  /* Withdraw the route */
  withdrawLocalRoutesAtRuntime({"11.6.0.0/16"});

  /* Both peers should receive the withdrawal */
  EXPECT_TRUE(verifyRouteWithdraw("v4", "11.6.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "11.6.0.0", 16, kPeerAddr4));

  /* Both peers still JOINED_RUNNING */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  XLOG(INFO, "=== TEST PASSED: JoinedRunning_RouteWithdraw ===");
}

/*
 * P-JR x E-BLOCK
 * Peer blocks: JOINED_RUNNING -> JOINED_BLOCKED.
 * Block peer3 and inject routes to fill the queue.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, JoinedRunning_Block) {
  XLOG(INFO, "=== TEST: JoinedRunning_Block ===");

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

  /* Block peer3 and fill its queue */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"11.7.0.0/16"}, {"1107:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.7.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.7.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1107:1"));
  injectLocalRoutesAtRuntime({"11.8.0.0/16"}, {"1108:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.8.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.8.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1108:1"));
  injectLocalRoutesAtRuntime({"11.9.0.0/16"}, {"1109:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.9.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.9.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1109:1"));

  /* Verify peer3 transitions to JOINED_BLOCKED */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* Peer3 should still be in sync (joined, not detached) */
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));

  /* Peer4 unaffected */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  XLOG(INFO, "=== TEST PASSED: JoinedRunning_Block ===");
}

/*
 * P-JR x E-MULTI-ROUTE
 * Batch of routes injected, all JOINED_RUNNING peers receive them.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, JoinedRunning_MultiRoute) {
  XLOG(INFO, "=== TEST: JoinedRunning_MultiRoute ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(8, 6, 2);

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

  /* Inject batch of 4 routes with different communities */
  for (int i = 0; i < 4; ++i) {
    auto prefix = "11." + std::to_string(12 + i) + ".0.0/16";
    auto community = std::to_string(1112 + i) + ":1";
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        "11." + std::to_string(12 + i) + ".0.0",
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        "11." + std::to_string(12 + i) + ".0.0",
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Both peers still JOINED_RUNNING after batch */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: JoinedRunning_MultiRoute ===");
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
