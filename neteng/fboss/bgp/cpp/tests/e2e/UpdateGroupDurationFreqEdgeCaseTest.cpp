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

/* E2E tests: Duration timer and frequency detection edge cases.
 * Prefix range: 26.10-26.46/16.
 *
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Duration timer cancelled by peer going down.
 * Block peer3, set 600s duration threshold, then bring peer3 down.
 * No stale timer callback crash. */
TEST_P(UpdateGroupMultiPeerTest, DurationTimerCancelledByPeerDown) {
  XLOG(INFO, "=== TEST: DurationTimerCancelledByPeerDown ===");

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

  /* Set long duration threshold — should NOT fire before peer goes down */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      100,
      std::chrono::milliseconds(60000));

  /* Block peer3 and fill queue to trigger JOINED_BLOCKED */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"26.10.0.0/16"}, {"2610:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2610:1"));
  injectLocalRoutesAtRuntime({"26.11.0.0/16"}, {"2611:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.11.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.11.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2611:1"));
  injectLocalRoutesAtRuntime({"26.12.0.0/16"}, {"2612:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.12.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.12.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2612:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* Bring peer3 down — timer should be cancelled, no stale callback crash */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* peer4 continues normally after peer3 goes down */
  injectLocalRoutesAtRuntime({"26.13.0.0/16"}, {"2613:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.13.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2613:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: DurationTimerCancelledByPeerDown ===");
}

/* Duration threshold 0ms — immediate detachment on first block.
 * Set 0ms threshold, block peer3, fill queue. Peer3 should detach
 * immediately when blocked. */
TEST_P(UpdateGroupMultiPeerTest, ZeroDurationThreshold_ImmediateDetach) {
  XLOG(INFO, "=== TEST: ZeroDurationThreshold_ImmediateDetach ===");

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

  /* Set 0ms duration threshold — should detach immediately on block */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(0),
      100,
      std::chrono::milliseconds(60000));

  /* Block and fill queue */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"26.20.0.0/16"}, {"2620:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2620:1"));
  injectLocalRoutesAtRuntime({"26.21.0.0/16"}, {"2621:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2621:1"));
  injectLocalRoutesAtRuntime({"26.22.0.0/16"}, {"2622:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.22.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.22.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2622:1"));

  /* 0ms threshold should fire instantly — peer3 detaches */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* peer4 still working */
  injectLocalRoutesAtRuntime({"26.23.0.0/16"}, {"2623:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.23.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.23.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2623:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: ZeroDurationThreshold_ImmediateDetach ===");
}

/* Duration timer fires after peer was already detached by frequency.
 * Detach peer3 via freq threshold first. Then set aggressive duration
 * threshold. No double-detach, no crash. */
TEST_P(UpdateGroupMultiPeerTest, DurationAfterFreqDetach_NoDoubleDetach) {
  XLOG(INFO, "=== TEST: DurationAfterFreqDetach_NoDoubleDetach ===");

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
  injectLocalRoutesAtRuntime({"26.30.0.0/16"}, {"2630:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2630:1"));
  injectLocalRoutesAtRuntime({"26.31.0.0/16"}, {"2631:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2631:1"));
  injectLocalRoutesAtRuntime({"26.32.0.0/16"}, {"2632:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.32.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.32.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2632:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Now set aggressive duration threshold (1ms) — should NOT double-detach */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(1),
      100,
      std::chrono::milliseconds(60000));

  /* Inject more routes to give time for any stale timer to fire */
  injectLocalRoutesAtRuntime({"26.33.0.0/16"}, {"2633:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.33.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.33.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2633:1"));

  /* Peer3 still DETACHED_BLOCKED — no state change, no crash */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: DurationAfterFreqDetach_NoDoubleDetach ===");
}

/* N-1 blocks in W seconds — below freq threshold, no detach.
 * Set threshold to 3 blocks in 60s window. Block/unblock only 2 times.
 * Peer should NOT detach. */
TEST_P(UpdateGroupMultiPeerTest, BelowFreqThreshold_NoDetach) {
  XLOG(INFO, "=== TEST: BelowFreqThreshold_NoDetach ===");

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

  /* Set freq threshold: 3 blocks in 60s window, no duration detach */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      3,
      std::chrono::milliseconds(60000));

  /* Block cycle 1: block → fill → wait blocked → unblock */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"26.40.0.0/16"}, {"2640:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2640:1"));
  injectLocalRoutesAtRuntime({"26.41.0.0/16"}, {"2641:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2641:1"));
  injectLocalRoutesAtRuntime({"26.42.0.0/16"}, {"2642:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.42.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.42.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2642:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* Unblock — peer3 recovers */
  unblockPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Block cycle 2: block again → fill → wait blocked */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"26.43.0.0/16"}, {"2643:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.43.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.43.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2643:1"));
  injectLocalRoutesAtRuntime({"26.44.0.0/16"}, {"2644:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.44.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.44.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2644:1"));
  injectLocalRoutesAtRuntime({"26.45.0.0/16"}, {"2645:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.45.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.45.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2645:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* Only 2 blocks — below threshold of 3. Peer NOT detached */
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* Unblock — peer3 recovers to JOINED_RUNNING */
  unblockPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));

  /* Verify both peers working normally */
  injectLocalRoutesAtRuntime({"26.46.0.0/16"}, {"2646:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.46.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.46.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2646:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: BelowFreqThreshold_NoDetach ===");
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
