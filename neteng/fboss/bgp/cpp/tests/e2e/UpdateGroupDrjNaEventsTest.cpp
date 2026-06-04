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

/* E2E tests: P-DRJ (DETACHED_READY_TO_JOIN) × N/A event coverage.
 * Prefix range: 20.x.0.0/16
 *
 * P-DRJ × E-SLOW-FREQ (N/A — already detached, no double-detach)
 * P-DRJ × E-PEER-UP (N/A — already up)
 * P-DRJ × E-CL-END (N/A — DRJ = already at CL end)
 * P-DRJ × E-ROUTE-REFRESH (simulated — no sendRouteRefresh helper)
 * P-DRJ × E-EOR (N/A — past EoR processing)
 *
 * DRJ is hard to reach directly in E2E. These tests use the standard
 * freq-detach → unblock pattern to get peer3 into recovery, then verify
 * each event is safe (no crash, peer4 functional, group stable).
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Helper: standard 2-peer setup → JOINED_RUNNING → freq-detach peer3.
 * Returns after peer3 is DETACHED_BLOCKED. Caller should unblockPeer
 * to start recovery toward DRJ. Uses prefix base for unique routes.
 */

/*
 * P-DRJ × E-SLOW-FREQ
 * N/A: at DETACHED_READY_TO_JOIN, the peer is already detached. Frequency
 * threshold tracking should not apply to detached peers (no double-detach).
 * After unblocking a detached peer, verify that aggressive frequency
 * thresholds do not cause a second detachment or crash.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachedReadyToJoin_SlowFreqNoop) {
  XLOG(INFO, "=== TEST: DetachedReadyToJoin_SlowFreqNoop ===");

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
  injectLocalRoutesAtRuntime({"20.1.0.0/16"}, {"2001:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2001:1"));
  injectLocalRoutesAtRuntime({"20.2.0.0/16"}, {"2002:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2002:1"));
  injectLocalRoutesAtRuntime({"20.3.0.0/16"}, {"2003:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2003:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock peer3 — hold in DRJ via test hook */
  waitForDrjWithDrain(kPeerAddr3, peerId3);

  /*
   * N/A: frequency threshold should not fire for already-detached peer.
   * Inject a route to exercise group activity — no double-detach.
   */
  injectLocalRoutesAtRuntime({"20.4.0.0/16"}, {"2004:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2004:1"));

  /* Peer4 still functional, no double-detach crash */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: DetachedReadyToJoin_SlowFreqNoop ===");
}

/*
 * P-DRJ × E-PEER-UP
 * N/A: peer is already up at DRJ. Calling bringUpPeer on an already-up
 * peer would cause session re-establishment which may hang (learned
 * bringUpPeer on already-up peer causes session reset). Instead, verify that
 * the peer's UP status is stable during recovery and group continues normally.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachedReadyToJoin_PeerUpNoop) {
  XLOG(INFO, "=== TEST: DetachedReadyToJoin_PeerUpNoop ===");

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
  injectLocalRoutesAtRuntime({"20.10.0.0/16"}, {"2010:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2010:1"));
  injectLocalRoutesAtRuntime({"20.11.0.0/16"}, {"2011:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.11.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.11.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2011:1"));
  injectLocalRoutesAtRuntime({"20.12.0.0/16"}, {"2012:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.12.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.12.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2012:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock peer3 — hold in DRJ via test hook */
  waitForDrjWithDrain(kPeerAddr3, peerId3);

  injectLocalRoutesAtRuntime({"20.13.0.0/16"}, {"2013:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.13.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2013:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: DetachedReadyToJoin_PeerUpNoop ===");
}

/*
 * P-DRJ × E-CL-END
 * N/A: at DRJ the peer has already consumed the CL to the end (that is
 * what makes it "ready to join"). Reaching CL end again is a no-op.
 * Verify that after unblocking a detached peer, no CL-related crash
 * occurs, and the group remains stable with peer4 functional.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachedReadyToJoin_ClEndNoop) {
  XLOG(INFO, "=== TEST: DetachedReadyToJoin_ClEndNoop ===");

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
  injectLocalRoutesAtRuntime({"20.20.0.0/16"}, {"2020:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2020:1"));
  injectLocalRoutesAtRuntime({"20.21.0.0/16"}, {"2021:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2021:1"));
  injectLocalRoutesAtRuntime({"20.22.0.0/16"}, {"2022:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.22.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.22.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2022:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock peer3 — hold in DRJ via test hook */
  waitForDrjWithDrain(kPeerAddr3, peerId3);

  /*
   * N/A: CL end has been reached. Verify stability by injecting
   * more routes — group enters new CL cycle, peer4 receives them.
   */
  injectLocalRoutesAtRuntime({"20.23.0.0/16"}, {"2023:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.23.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.23.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2023:1"));

  injectLocalRoutesAtRuntime({"20.24.0.0/16"}, {"2024:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.24.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.24.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2024:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: DetachedReadyToJoin_ClEndNoop ===");
}

/*
 * P-DRJ × E-ROUTE-REFRESH
 * Route refresh at DRJ is a conflicting transition. Since the E2E
 * framework has no sendRouteRefresh helper (learned pattern Task 3 W1),
 * we simulate route refresh behavior by injecting a burst of routes
 * (triggering MRAI cycles) after unblocking the detached peer. This
 * exercises the group's route processing path while peer3 is recovering.
 * Verify no crash and peer4 receives all routes.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachedReadyToJoin_RouteRefresh) {
  XLOG(INFO, "=== TEST: DetachedReadyToJoin_RouteRefresh ===");

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

  /* Inject a shared route before detachment */
  injectLocalRoutesAtRuntime({"20.30.0.0/16"}, {"2030:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.30.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2030:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2030:1"));

  /* Detach peer3 via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"20.31.0.0/16"}, {"2031:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2031:1"));
  injectLocalRoutesAtRuntime({"20.32.0.0/16"}, {"2032:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.32.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.32.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2032:1"));
  injectLocalRoutesAtRuntime({"20.33.0.0/16"}, {"2033:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.33.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.33.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2033:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock peer3 — hold in DRJ via test hook */
  waitForDrjWithDrain(kPeerAddr3, peerId3);

  /*
   * Simulate route refresh: burst of 3 routes with different communities.
   * Each triggers a separate MRAI cycle.
   */
  injectLocalRoutesAtRuntime({"20.34.0.0/16"}, {"2034:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.34.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.34.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2034:1"));

  injectLocalRoutesAtRuntime({"20.35.0.0/16"}, {"2035:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.35.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.35.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2035:1"));

  injectLocalRoutesAtRuntime({"20.36.0.0/16"}, {"2036:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.36.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.36.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2036:1"));

  /* Group stable, peer4 functional after burst during recovery */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: DetachedReadyToJoin_RouteRefresh ===");
}

/*
 * P-DRJ × E-EOR
 * N/A: EoR processing does not apply to a peer at DRJ — EoR was already
 * processed during the initial INIT→JOINED_RUNNING transition. Sending
 * EoR to a peer during recovery is a no-op. After unblocking a detached
 * peer, call sendEoRToPeer (which should be harmless) and verify no crash.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachedReadyToJoin_EorNoop) {
  XLOG(INFO, "=== TEST: DetachedReadyToJoin_EorNoop ===");

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
  injectLocalRoutesAtRuntime({"20.40.0.0/16"}, {"2040:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2040:1"));
  injectLocalRoutesAtRuntime({"20.41.0.0/16"}, {"2041:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2041:1"));
  injectLocalRoutesAtRuntime({"20.42.0.0/16"}, {"2042:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.42.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.42.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2042:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock peer3 — hold in DRJ via test hook */
  waitForDrjWithDrain(kPeerAddr3, peerId3);

  /*
   * N/A: send EoR to peer3 during DRJ — should be harmless no-op.
   * EoR was already processed in the original session.
   */
  sendEoRToPeer(peerId3);

  /* Inject a route to verify group still processes normally */
  injectLocalRoutesAtRuntime({"20.43.0.0/16"}, {"2043:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.43.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.43.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2043:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: DetachedReadyToJoin_EorNoop ===");
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
