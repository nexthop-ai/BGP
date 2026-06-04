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
 * E2E tests: Missing detection edge case tests
 *
 * Prefix range: 36.x.0.0/16
 *
 * Tests:
 *   Duration timer fires for 2 peers simultaneously
 *   Duration timer cancelled by peer going down
 *   Duration threshold 0ms — immediate detachment
 *   Duration fires after already detached by freq
 *   New peer joins while another being detected as slow
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Duration timer fires for 2 peers simultaneously.
 * 3 peers. Block peer3 and peer4 with 1ms duration. Both detach.
 * Peer5 continues.
 */
TEST_P(UpdateGroupMultiPeerTest, DurationTwoPeers_BothDetach) {
  XLOG(INFO, "=== TEST: DurationTwoPeers_BothDetach ===");

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

  /* Set 1ms duration on both peer3 and peer4 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(1),
      999999,
      std::chrono::milliseconds(60000));
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(1),
      999999,
      std::chrono::milliseconds(60000));

  /* Block both and fill queue */
  blockPeer(kPeerAddr3);
  blockPeer(kPeerAddr4);
  injectLocalRoutesAtRuntime({"36.1.0.0/16"}, {"3601:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.1.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3601:1"));
  injectLocalRoutesAtRuntime({"36.2.0.0/16"}, {"3602:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.2.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3602:1"));
  injectLocalRoutesAtRuntime({"36.3.0.0/16"}, {"3603:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.3.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3603:1"));

  /* Both should be DETACHED_BLOCKED (1ms duration fires instantly) */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Peer5 continues */
  injectLocalRoutesAtRuntime({"36.4.0.0/16"}, {"3604:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.4.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3604:1"));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  XLOG(INFO, "=== TEST PASSED: DurationTwoPeers_BothDetach ===");
}

/*
 * Duration timer cancelled by peer going down.
 * Block peer3 with long (600s) duration. Then bringDownPeer immediately.
 * No crash from stale timer callback.
 */
TEST_P(UpdateGroupMultiPeerTest, DurationCancelledByDown) {
  XLOG(INFO, "=== TEST: DurationCancelledByDown ===");

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

  /* Set long duration threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Block peer3 */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"36.10.0.0/16"}, {"3610:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3610:1"));

  /* Immediately bring peer3 DOWN — cancels duration timer */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* No crash from stale timer callback */
  injectLocalRoutesAtRuntime({"36.11.0.0/16"}, {"3611:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.11.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.11.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3611:1"));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: DurationCancelledByDown ===");
}

/*
 * Duration threshold 0ms — immediate detachment.
 * std::chrono::milliseconds(0) causes immediate detach on first block.
 */
TEST_P(UpdateGroupMultiPeerTest, DurationZeroMs_ImmediateDetach) {
  XLOG(INFO, "=== TEST: DurationZeroMs_ImmediateDetach ===");

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

  /* Set 0ms duration threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(0),
      999999,
      std::chrono::milliseconds(60000));

  /* Block and fill queue */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"36.20.0.0/16"}, {"3620:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3620:1"));
  injectLocalRoutesAtRuntime({"36.21.0.0/16"}, {"3621:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3621:1"));
  injectLocalRoutesAtRuntime({"36.22.0.0/16"}, {"3622:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.22.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.22.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3622:1"));

  /* 0ms threshold fires immediately → DETACHED_BLOCKED */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  XLOG(INFO, "=== TEST PASSED: DurationZeroMs_ImmediateDetach ===");
}

/*
 * Duration fires after already detached by freq.
 * Freq-detach peer3 first. Then set 1ms duration on already-detached
 * peer. No double-detachment, stays DETACHED_BLOCKED.
 */
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

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"36.30.0.0/16"}, {"3630:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3630:1"));
  injectLocalRoutesAtRuntime({"36.31.0.0/16"}, {"3631:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3631:1"));
  injectLocalRoutesAtRuntime({"36.32.0.0/16"}, {"3632:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.32.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.32.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3632:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Now set 1ms duration on already-detached peer — no double-detach */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(1),
      999999,
      std::chrono::milliseconds(60000));

  /* Still DETACHED_BLOCKED — no second detachment */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1);

  XLOG(INFO, "=== TEST PASSED: DurationAfterFreqDetach_NoDouble ===");
}

/*
 * New peer joins while another being detected as slow.
 * Inject routes, detach peer3 via freq, then bring peer5 UP. The new
 * peer joining should not interfere with detachment.
 */
TEST_P(UpdateGroupMultiPeerTest, NewPeerDuringDetection) {
  XLOG(INFO, "=== TEST: NewPeerDuringDetection ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  /* Peer5 not up yet */

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Set freq threshold on peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Block and fill to detach peer3 */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"36.40.0.0/16"}, {"3640:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3640:1"));
  injectLocalRoutesAtRuntime({"36.41.0.0/16"}, {"3641:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3641:1"));
  injectLocalRoutesAtRuntime({"36.42.0.0/16"}, {"3642:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.42.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.42.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3642:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Now bring up peer5 — new peer joins while peer3 is detached */
  bringUpPeer(kPeerAddr5);
  sendEoRToPeer(peerId5);
  /* Peer5 enters DETACHED_INIT_DUMP for existing group — no EoR wait needed */

  /* Verify no interference — peer3 still detached, peer4 still synced */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Peer4 continues receiving routes */
  injectLocalRoutesAtRuntime({"36.45.0.0/16"}, {"3645:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.45.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.45.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3645:1"));

  XLOG(INFO, "=== TEST PASSED: NewPeerDuringDetection ===");
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
