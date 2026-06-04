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
 * E2E tests for Update Group slow peer detection.
 * Tests frequency-based and duration-based detection triggers.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Test: Frequency-based slow peer detection.
 * Two peers in same group, one blocks repeatedly.
 * After exceeding block count threshold, the slow peer is detached.
 * The fast peer continues receiving routes normally.
 *
 * Flow:
 * 1. Bring up 2 peers, consume initial dump
 * 2. Set frequency threshold = 2 blocks
 * 3. Block peer3 (test-level), inject routes -> queue fills -> block event #1
 * 4. Unblock peer3 (drains queue)
 * 5. Block peer3 again, inject more routes -> block event #2 -> DETACH
 * 6. Verify peer3 is DETACHED_BLOCKED, peer4 still receives routes
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, FrequencyBasedDetection_PeerDetaches) {
  XLOGF(INFO, "=== TEST: FrequencyBasedDetection_PeerDetaches ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  /*
   * Setup components WITHOUT pre-loaded routes.
   * Both peers will be brought up and both will be in INIT state
   * when the group does its initial dump, ensuring both transition
   * to JOINED_RUNNING together.
   */
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Consume v4+v6 EoRs from both peers */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  /* Both peers should be JOINED_RUNNING */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /*
   * Now inject a route at runtime (after peers are up and synced).
   * Both peers should receive it.
   */
  injectLocalRoutesAtRuntime({"40.0.0.0/8"}, {"400:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("40.0.0.0/8")));

  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "40.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "400:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "40.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "400:1"));

  /*
   * Set low frequency threshold: 2 blocks triggers detachment.
   * Duration very high so only frequency triggers.
   */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      2,
      std::chrono::milliseconds(60000));

  /*
   * Use larger queue for this test so peer4 doesn't also fill up.
   * Queue (8, 6, 2) is large enough for peer4; peer3 is test-level
   * blocked so its queue fills regardless of size.
   * We set up with larger queues, then block peer3 to cause backpressure.
   */

  /* === Block cycle #1 === */
  blockPeer(kPeerAddr3);
  XLOGF(INFO, "Block cycle #1: injecting routes");
  injectLocalRoutesAtRuntime({"41.0.0.0/8"}, {"410:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("41.0.0.0/8")));

  /* Consume peer4's route immediately to prevent its queue from filling */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "41.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "410:1"));

  injectLocalRoutesAtRuntime({"42.0.0.0/8"}, {"420:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("42.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "42.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "420:1"));

  injectLocalRoutesAtRuntime({"43.0.0.0/8"}, {"430:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("43.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "43.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "430:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  XLOGF(INFO, "Block cycle #1: peer3 blocked, peer4 drained");

  /* Unblock peer3: drains its queue (consumes route messages) */
  unblockPeer(kPeerAddr3);
  XLOGF(INFO, "Block cycle #1: peer3 unblocked");

  /* === Block cycle #2 -> triggers detachment === */
  blockPeer(kPeerAddr3);
  XLOGF(INFO, "Block cycle #2: injecting routes to trigger detachment");
  injectLocalRoutesAtRuntime({"44.0.0.0/8"}, {"440:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("44.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "44.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "440:1"));

  injectLocalRoutesAtRuntime({"45.0.0.0/8"}, {"450:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("45.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "45.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "450:1"));

  injectLocalRoutesAtRuntime({"46.0.0.0/8"}, {"460:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("46.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "46.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "460:1"));

  /* Wait for peer3 blocking and then detachment */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Verify peer3 is detached */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));

  /* Verify peer4 is still JOINED_RUNNING and in sync */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Verify invariants */
  verifySlowPeerInvariants(kPeerAddr4);
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);

  XLOGF(INFO, "=== TEST PASSED: FrequencyBasedDetection_PeerDetaches ===");
}

/*
 * Test: Single peer in group cannot be detached.
 * When there's only one synced peer, detachment is skipped to prevent
 * leaving the group with no in-sync members.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, SinglePeerGroup_DetachmentSkipped) {
  XLOGF(INFO, "=== TEST: SinglePeerGroup_DetachmentSkipped ===");

  addPeer(kDefaultPeerSpec3);
  addLocalRoute("50.0.0.0/8", {"500:1"}, 100);

  setupSlowPeerComponents(3, 2, 0);

  auto routePrefix = folly::IPAddress::createNetwork("50.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(routePrefix));

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);

  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "50.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "500:1"));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Set threshold to 1 block (immediate trigger) */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);

  injectLocalRoutesAtRuntime({"51.0.0.0/8"}, {"510:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("51.0.0.0/8")));
  injectLocalRoutesAtRuntime({"52.0.0.0/8"}, {"520:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("52.0.0.0/8")));
  injectLocalRoutesAtRuntime({"53.0.0.0/8"}, {"530:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("53.0.0.0/8")));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /*
   * Peer should stay JOINED_BLOCKED, NOT DETACHED_BLOCKED.
   * As the only synced peer, detachment is skipped.
   */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 0);

  /* Unblock and verify routes flow after recovery */
  unblockPeer(kPeerAddr3);

  /* Inject a new route to verify peer is still functional */
  injectLocalRoutesAtRuntime({"54.0.0.0/8"}, {"540:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("54.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "54.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "540:1"));

  XLOGF(INFO, "=== TEST PASSED: SinglePeerGroup_DetachmentSkipped ===");
}

/*
 * Test: Peer blocks but unblocks before threshold -> no detachment.
 * Verifies that blocking alone doesn't trigger detachment if the peer
 * recovers before the frequency threshold is reached.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, BlockWithoutThreshold_NoDetachment) {
  XLOGF(INFO, "=== TEST: BlockWithoutThreshold_NoDetachment ===");

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

  /* Set HIGH frequency threshold so blocking alone won't trigger */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      100, /* very high - won't trigger */
      std::chrono::milliseconds(60000));

  /* Block and cause queue to fill */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"61.0.0.0/8"}, {"610:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("61.0.0.0/8")));
  injectLocalRoutesAtRuntime({"62.0.0.0/8"}, {"620:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("62.0.0.0/8")));
  injectLocalRoutesAtRuntime({"63.0.0.0/8"}, {"630:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("63.0.0.0/8")));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Unblock - peer recovers, should NOT be detached */
  unblockPeer(kPeerAddr3);

  EXPECT_FALSE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Consume peer4's queued routes 61-63 (received while peer3 was blocked) */
  EXPECT_TRUE(verifyRoutes(
      "v4",
      kPeerAddr4,
      {{.prefix = "61.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = getExpectedNexthop(kPeerAddr4),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "610:1"},
       {.prefix = "62.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = getExpectedNexthop(kPeerAddr4),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "620:1"},
       {.prefix = "63.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = getExpectedNexthop(kPeerAddr4),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "630:1"}}));

  /* Verify both peers receive a new route after recovery */
  injectLocalRoutesAtRuntime({"64.0.0.0/8"}, {"640:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("64.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "64.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "640:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "64.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "640:1"));

  XLOGF(INFO, "=== TEST PASSED: BlockWithoutThreshold_NoDetachment ===");
}
/*
 * Test: Multiple block-unblock cycles below threshold.
 * Peer blocks and unblocks 3 times with threshold=5.
 * Verify peer stays JOINED_RUNNING after each cycle, never detaches.
 */
TEST_P(
    UpdateGroupSlowPeerDetectionTest,
    MultipleBlockUnblockCycles_BelowThreshold) {
  XLOGF(INFO, "=== TEST: MultipleBlockUnblockCycles_BelowThreshold ===");

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

  /* High threshold: 5 blocks needed for detachment */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      5,
      std::chrono::milliseconds(60000));

  /* Cycle 1: block, inject, drain peer4, unblock */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"240.0.0.0/8"}, {"2400:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("240.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "240.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2400:1"));
  injectLocalRoutesAtRuntime({"241.0.0.0/8"}, {"2410:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("241.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "241.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2410:1"));
  injectLocalRoutesAtRuntime({"242.0.0.0/8"}, {"2420:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("242.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "242.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2420:1"));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  unblockPeer(kPeerAddr3);
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));
  XLOGF(INFO, "Cycle 1 complete: peer3 not detached");

  /* Cycle 2 */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"243.0.0.0/8"}, {"2430:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("243.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "243.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2430:1"));
  injectLocalRoutesAtRuntime({"244.0.0.0/8"}, {"2440:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("244.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "244.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2440:1"));
  injectLocalRoutesAtRuntime({"245.0.0.0/8"}, {"2450:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("245.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "245.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2450:1"));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  unblockPeer(kPeerAddr3);
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));
  XLOGF(INFO, "Cycle 2 complete: peer3 not detached");

  /* Cycle 3 */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"246.0.0.0/8"}, {"2460:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("246.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "246.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2460:1"));
  injectLocalRoutesAtRuntime({"247.0.0.0/8"}, {"2470:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("247.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "247.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2470:1"));
  injectLocalRoutesAtRuntime({"248.0.0.0/8"}, {"2480:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("248.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "248.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2480:1"));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  unblockPeer(kPeerAddr3);

  /* After 3 cycles with threshold=5, peer should NOT be detached */
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));

  /* Verify peer3 can still receive routes */
  injectLocalRoutesAtRuntime({"249.0.0.0/8"}, {"2490:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("249.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "249.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2490:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "249.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2490:1"));

  XLOGF(INFO, "=== TEST PASSED: MultipleBlockUnblockCycles_BelowThreshold ===");
}
/*
 * Test: 3 peers, detach 2, third cannot detach (last synced guard).
 */
TEST_P(
    UpdateGroupSlowPeerDetectionTest,
    ThreePeers_DetachTwo_ThirdCannotDetach) {
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

  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Detach peer3 */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"10.26.0.0/16"}, {"1026:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.26.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.26.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1026:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.26.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1026:1"));
  injectLocalRoutesAtRuntime({"10.27.0.0/16"}, {"1027:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.27.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.27.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1027:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.27.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1027:1"));
  injectLocalRoutesAtRuntime({"10.28.0.0/16"}, {"1028:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.28.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.28.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1028:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.28.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1028:1"));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);

  /* Detach peer5 — only peer4 remains synced */
  blockPeer(kPeerAddr5);
  injectLocalRoutesAtRuntime({"10.29.0.0/16"}, {"1029:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.29.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.29.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1029:1"));
  injectLocalRoutesAtRuntime({"10.30.0.0/16"}, {"1030:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1030:1"));
  injectLocalRoutesAtRuntime({"10.31.0.0/16"}, {"1031:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1031:1"));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId5));
  EXPECT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 2);

  /* Now try to block peer4 — should stay JOINED_BLOCKED (last synced) */
  blockPeer(kPeerAddr4);
  injectLocalRoutesAtRuntime({"10.32.0.0/16"}, {"1032:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.32.0.0/16")));
  injectLocalRoutesAtRuntime({"10.33.0.0/16"}, {"1033:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.33.0.0/16")));
  injectLocalRoutesAtRuntime({"10.34.0.0/16"}, {"1034:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.34.0.0/16")));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_BLOCKED));
  EXPECT_FALSE(isPeerDetached(kPeerAddr4));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 2);
  verifySlowPeerInvariants(kPeerAddr4);
}
/*
 * Test: 5 routes injected before detachment, verify all reach fast peer.
 */
TEST_P(
    UpdateGroupSlowPeerDetectionTest,
    MultipleRoutesBeforeDetach_AllReachFastPeer) {
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

  /* Inject 5 routes — both peers receive all of them */
  for (int i = 0; i < 5; ++i) {
    auto prefix = "10." + std::to_string(38 + i) + ".0.0/16";
    auto community = std::to_string(1038 + i) + ":1";
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        "10." + std::to_string(38 + i) + ".0.0",
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        "10." + std::to_string(38 + i) + ".0.0",
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Now detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"10.43.0.0/16"}, {"1043:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.43.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.43.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1043:1"));
  injectLocalRoutesAtRuntime({"10.44.0.0/16"}, {"1044:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.44.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.44.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1044:1"));
  injectLocalRoutesAtRuntime({"10.45.0.0/16"}, {"1045:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.45.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.45.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1045:1"));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Post-detach routes go only to peer4 */
  injectLocalRoutesAtRuntime({"10.46.0.0/16"}, {"1046:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.46.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.46.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1046:1"));
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
