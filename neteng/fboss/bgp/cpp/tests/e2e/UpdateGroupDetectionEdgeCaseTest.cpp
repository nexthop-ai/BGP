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

/* E2E tests: Duration/frequency detection edge cases. Prefix range: 26.x/16.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* 3 peers, peer3+4 block with 1ms duration, peer5 fast. Both detach. */
TEST_P(UpdateGroupMultiPeerTest, DurationTwoPeersSimultaneousDetach) {
  XLOG(INFO, "=== TEST: DurationTwoPeersSimultaneousDetach ===");
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
      std::chrono::milliseconds(1),
      100,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  blockPeer(kPeerAddr4);
  injectLocalRoutesAtRuntime({"26.1.0.0/16"}, {"2601:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.1.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "2601:1"));
  injectLocalRoutesAtRuntime({"26.2.0.0/16"}, {"2602:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.2.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "2602:1"));
  injectLocalRoutesAtRuntime({"26.3.0.0/16"}, {"2603:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.3.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "2603:1"));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));
  /* Peer5 still works normally */
  injectLocalRoutesAtRuntime({"26.4.0.0/16"}, {"2604:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.4.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "2604:1"));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));
  verifySlowPeerInvariants(kPeerAddr5);
  XLOG(INFO, "=== TEST PASSED: DurationTwoPeersSimultaneousDetach ===");
}

/* Block peer3 with long duration, then DOWN before timer fires. */
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
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      100,
      std::chrono::milliseconds(60000));
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
  /* Bring peer3 DOWN — cancels duration timer */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));
  /* Peer4 continues — no crash from stale timer callback */
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
  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: DurationTimerCancelledByPeerDown ===");
}

/* 0ms duration threshold — detach on first block event. */
TEST_P(UpdateGroupMultiPeerTest, DurationZeroMsImmediateDetach) {
  XLOG(INFO, "=== TEST: DurationZeroMsImmediateDetach ===");
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
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(0),
      100,
      std::chrono::milliseconds(60000));
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
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  /* Peer4 still functional */
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
  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: DurationZeroMsImmediateDetach ===");
}

/* Freq detach first, then set 1ms duration — no double detach. */
TEST_P(UpdateGroupMultiPeerTest, DurationAfterFreqDetach_NoDouble) {
  XLOG(INFO, "=== TEST: DurationAfterFreqDetach_NoDouble ===");
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
  /* Freq threshold = 1 block to detach */
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
  /* Set aggressive 1ms duration on already-detached peer */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(1),
      100,
      std::chrono::milliseconds(60000));
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
  /* Peer3 still DETACHED_BLOCKED — no double detach */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: DurationAfterFreqDetach_NoDouble ===");
}

/* Freq threshold=3, only 2 block cycles — no detach. */
TEST_P(UpdateGroupMultiPeerTest, FreqBelowThreshold_NoDetach) {
  XLOG(INFO, "=== TEST: FreqBelowThreshold_NoDetach ===");
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
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      3,
      std::chrono::milliseconds(60000));
  /* Block cycle 1 */
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
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  /* Block cycle 2 */
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
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  /* 2 block events < threshold of 3 — NOT detached */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  /* Both peers receive a final route */
  injectLocalRoutesAtRuntime({"26.46.0.0/16"}, {"2646:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.46.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.46.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2646:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.46.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2646:1"));
  verifySlowPeerInvariants(kPeerAddr3);
  XLOG(INFO, "=== TEST PASSED: FreqBelowThreshold_NoDetach ===");
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
