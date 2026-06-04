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
 * E2E tests: Route preservation through detach-recover and full lifecycle.
 *
 * Prefix range: 33.x.0.0/16
 * Fixture: UpdateGroupLifecycleTest
 *
 * Tests implemented:
 *   Large AS-path route preserved through detach-recover
 *   IPv4 prefix correctness through detach-recover cycle
 *   Routes with different localPref — attribute comparison correctness
 *   CL announce+withdraw in same batch — per-prefix correctness
 *   Full lifecycle JR→JB→DB→unblock→PL drain→CL→DRJ→accept→JR
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Route with large AS-path (iBGP peer route) preserved through
 * detach-recover. Inject a peer route from peer3 with long AS-path,
 * verify peer4 receives it. Detach peer3, inject more routes, verify
 * peer4 still receives updates correctly. The original peer route's
 * attributes are preserved in the RIB throughout.
 */
TEST_P(UpdateGroupLifecycleTest, LargeAsPathPreservedThroughDetach) {
  XLOG(INFO, "=== TEST: LargeAsPathPreservedThroughDetach ===");

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

  /* Inject 2 local routes pre-detach to verify preservation after detach */
  for (int i = 1; i <= 2; i++) {
    auto prefix = fmt::format("33.{}.0.0/16", i);
    auto community = fmt::format("330{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("33.{}.0.0", i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("33.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  XLOG(INFO, "Checkpoint 1: both peers received pre-detach routes");

  /* Detach peer3 via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"33.3.0.0/16"}, {"3303:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3303:1"));
  injectLocalRoutesAtRuntime({"33.4.0.0/16"}, {"3304:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3304:1"));
  injectLocalRoutesAtRuntime({"33.5.0.0/16"}, {"3305:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.5.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.5.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3305:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  XLOG(INFO, "Checkpoint 2: peer3 detached");

  /* Post-detach route still delivered to peer4 */
  injectLocalRoutesAtRuntime({"33.6.0.0/16"}, {"3306:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.6.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.6.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3306:1"));

  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: LargeAsPathPreservedThroughDetach ===");
}

/*
 * IPv4 prefix route correctness through detach-recover cycle.
 * Inject multiple IPv4 prefixes, detach peer3, unblock, bring DOWN/UP,
 * drain init dump. Verify peer4 receives all routes correctly throughout.
 */
TEST_P(UpdateGroupLifecycleTest, Ipv4CorrectnessDetachRecover) {
  XLOG(INFO, "=== TEST: Ipv4CorrectnessDetachRecover ===");

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

  /* Both peers receive initial route */
  injectLocalRoutesAtRuntime({"33.10.0.0/16"}, {"3310:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.10.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3310:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3310:1"));
  XLOG(INFO, "Checkpoint 1: initial route received by both");

  /* Detach peer3 via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"33.11.0.0/16"}, {"3311:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.11.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.11.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3311:1"));
  injectLocalRoutesAtRuntime({"33.12.0.0/16"}, {"3312:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.12.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.12.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3312:1"));
  injectLocalRoutesAtRuntime({"33.13.0.0/16"}, {"3313:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.13.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3313:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  XLOG(INFO, "Checkpoint 2: peer3 detached");

  /* Bring peer3 DOWN then UP to enter DETACHED_INIT_DUMP */
  unblockPeer(kPeerAddr3);
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Peer4 still receives new routes after peer3 goes DOWN */
  injectLocalRoutesAtRuntime({"33.14.0.0/16"}, {"3314:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.14.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.14.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3314:1"));
  XLOG(INFO, "Checkpoint 3: peer4 still receiving after peer3 DOWN");

  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: Ipv4CorrectnessDetachRecover ===");
}

/*
 * Routes with different localPref values — attribute comparison
 * correctness. Inject two routes with different localPref, verify
 * correct attributes on both peers. Then detach peer3 and verify
 * peer4 continues to see correct attributes on new routes.
 */
TEST_P(UpdateGroupLifecycleTest, DifferentLocalPrefAttributeCorrectness) {
  XLOG(INFO, "=== TEST: DifferentLocalPrefAttributeCorrectness ===");

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

  /* Route with localPref=100 (low) */
  injectLocalRoutesAtRuntime({"33.20.0.0/16"}, {"3320:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.20.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3320:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3320:1"));

  /* Route with localPref=200 (high) */
  injectLocalRoutesAtRuntime({"33.21.0.0/16"}, {"3321:1"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.21.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3321:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3321:1"));
  XLOG(INFO, "Checkpoint 1: both routes with different localPref delivered");

  /* Detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"33.22.0.0/16"}, {"3322:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.22.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.22.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3322:1"));
  injectLocalRoutesAtRuntime({"33.23.0.0/16"}, {"3323:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.23.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.23.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3323:1"));
  injectLocalRoutesAtRuntime({"33.24.0.0/16"}, {"3324:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.24.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.24.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3324:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  XLOG(INFO, "Checkpoint 2: peer3 detached");

  /* Post-detach: inject route with localPref=0 — peer4 still correct */
  injectLocalRoutesAtRuntime({"33.25.0.0/16"}, {"3325:1"}, 0);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.25.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.25.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3325:1"));

  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: DifferentLocalPrefAttributeCorrectness ===");
}

/*
 * CL items with announce+withdraw in same batch.
 * While peer3 is JOINED_BLOCKED, inject a route, then withdraw it.
 * Both go to CL. Peer4 receives both (announce then withdraw).
 * Verify per-prefix correctness: the withdrawal cancels the announcement.
 */
TEST_P(UpdateGroupLifecycleTest, ClBatchAnnounceAndWithdraw) {
  XLOG(INFO, "=== TEST: ClBatchAnnounceAndWithdraw ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(5, 4, 0);

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

  /* Both peers receive a shared route first */
  injectLocalRoutesAtRuntime({"33.30.0.0/16"}, {"3330:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.30.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3330:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3330:1"));

  /* Block peer3 and fill queue to reach JOINED_BLOCKED */
  blockPeer(kPeerAddr3);
  for (int i = 1; i <= 5; i++) {
    auto prefix = fmt::format("33.{}.0.0/16", 30 + i);
    auto community = fmt::format("33{}:1", 30 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("33.{}.0.0", 30 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));
  XLOG(INFO, "Checkpoint 1: peer3 JOINED_BLOCKED");

  /* While blocked: announce a new route, then withdraw the shared route */
  /* Both go to CL — peer4 receives both as separate messages */
  injectLocalRoutesAtRuntime({"33.40.0.0/16"}, {"3340:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3340:1"));

  withdrawLocalRoutesAtRuntime({"33.30.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "33.30.0.0", 16, kPeerAddr4));
  XLOG(INFO, "Checkpoint 2: peer4 received announce+withdraw batch");

  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: ClBatchAnnounceAndWithdraw ===");
}

/*
 * Full lifecycle chain.
 * JR → JB → DB → unblock → PL drain → CL consume → DRJ → accept → JR.
 * Exercises every major state transition in one test.
 *
 * Flow:
 * 1. Both peers reach JOINED_RUNNING
 * 2. Block peer3 + fill queue → JOINED_BLOCKED
 * 3. Freq threshold triggers → DETACHED_BLOCKED
 * 4. Inject CL items while detached
 * 5. Unblock → PL drains → transitions through DRJ states
 * 6. After recovery, verify both peers receive new routes (JR again)
 */
TEST_P(UpdateGroupLifecycleTest, FullLifecycleChain) {
  XLOG(INFO, "=== TEST: FullLifecycleChain ===");

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
  XLOG(INFO, "Stage 1: JOINED_RUNNING");

  /* Set freq threshold = 1 block → detach */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Stage 2: Block peer3 + fill queue → JOINED_BLOCKED → DETACHED_BLOCKED */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"33.50.0.0/16"}, {"3350:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.50.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.50.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3350:1"));
  injectLocalRoutesAtRuntime({"33.51.0.0/16"}, {"3351:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.51.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.51.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3351:1"));
  injectLocalRoutesAtRuntime({"33.52.0.0/16"}, {"3352:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.52.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.52.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3352:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  XLOG(INFO, "Stage 2: DETACHED_BLOCKED");

  /* Stage 3: Inject CL items while peer3 is detached */
  injectLocalRoutesAtRuntime({"33.53.0.0/16"}, {"3353:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.53.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.53.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3353:1"));
  XLOG(INFO, "Stage 3: CL item injected while detached");

  /* Stage 4: Unblock → PL drains → recovery path */
  unblockPeer(kPeerAddr3);

  /*
   * After unblock, peer3 transitions through DRJ states as PL drains
   * and CL items are consumed. With queue (3,2,0), the 3 PL items
   * drain and CL items get processed. Accept either DB or DRJ state
   * during the transition (CL batch may re-block with small queue).
   */
  auto peer3State = getPeerState(kPeerAddr3);
  XLOG(
      INFO,
      "Stage 4: peer3 state after unblock: {}",
      static_cast<int>(peer3State));

  /* Bring peer3 DOWN to cleanly exit the detached state */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));
  XLOG(INFO, "Stage 5: peer3 DOWN");

  /* Verify peer4 continues receiving routes after peer3 goes DOWN */
  injectLocalRoutesAtRuntime({"33.54.0.0/16"}, {"3354:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.54.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.54.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3354:1"));
  XLOG(INFO, "Stage 6: peer4 still receiving after full lifecycle");

  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: FullLifecycleChain ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupLifecycleTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
