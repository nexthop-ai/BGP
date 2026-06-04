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
 * E2E tests: Detection interaction tests -- detached counter skip,
 * duration+freq simultaneous, single-peer guard, re-block timer reset,
 * 3-peer near-threshold.
 *
 * Prefix range: 79.x.0.0/16
 *
 * Tests:
 *   Detached peer's freq counter not incremented
 *   Duration fires for peer A, freq fires for peer B simultaneously
 *   Last two peers: one blocks, one is only synced -- skip detach
 *   Peer unblocks then immediately reblocks -- timer reset
 *   Three peers: one DETACHED, one JOINED_BLOCKED (near threshold)
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {
/*
 * Duration fires for peer A, freq fires for peer B simultaneously.
 * Peer3: 1ms duration threshold. Peer4: freq threshold=1.
 * Block both. Peer3 detaches via duration, peer4 via frequency.
 * Peer5 continues as sole in-sync member.
 */
TEST_P(UpdateGroupMultiPeerTest, DurationAndFreq_SimultaneousDetach) {
  XLOG(INFO, "=== TEST: DurationAndFreq_SimultaneousDetach ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  /* Fast peer with large queue */
  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr5);

  /* Slow peers with tiny queue — queues fill naturally */
  setDefaultQueueSizes(3, 2, 0);
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

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

  /*
   * Set 1ms duration on peer3 (duration path) and freq=1 on peer4 (freq path).
   * Note: thresholds are per-group, so we set them on one peer address
   * but they affect the group. We set duration=1ms and freq=1 -- whichever
   * triggers first for each peer depends on timing, but both should detach.
   */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(1),
      1,
      std::chrono::milliseconds(60000));

  /* Inject routes — peer3/peer4 queues fill naturally, peer5 drains */
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("79.{}.0.0/16", 10 + i);
    auto community = fmt::format("79{}:1", 10 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("79.{}.0.0", 10 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));
  }

  /* Both peer3 and peer4 should be DETACHED_BLOCKED */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Peer5 continues as sole in-sync member */
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));
  injectLocalRoutesAtRuntime({"79.15.0.0/16"}, {"7915:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("79.15.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "79.15.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "7915:1"));

  XLOG(INFO, "=== TEST PASSED: DurationAndFreq_SimultaneousDetach ===");
}

/*
 * Three peers: one DETACHED, one JOINED_BLOCKED (near threshold),
 * one in-sync. Freq-detach peer3 first (threshold=1), then raise
 * threshold to 999999 so peer4 blocks but doesn't detach.
 */
TEST_P(UpdateGroupMultiPeerTest, ThreePeers_DetachedAndNearThreshold) {
  XLOG(INFO, "=== TEST: ThreePeers_DetachedAndNearThreshold ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  /* Fast peer with large queue */
  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr5);

  /* Slow peers with tiny queue — queues fill naturally */
  setDefaultQueueSizes(3, 2, 0);
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

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

  /* Step 1: Freq-detach peer3 (threshold=1) */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Inject routes — peer3 queue fills naturally, peer4/peer5 drain */
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("79.{}.0.0/16", 40 + i);
    auto community = fmt::format("79{}:1", 40 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("79.{}.0.0", 40 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("79.{}.0.0", 40 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Step 2: Raise threshold to prevent peer4 from detaching */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Step 3: peer4 queue fills naturally — stays JOINED_BLOCKED (threshold too
   * high) */
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("79.{}.0.0/16", 45 + i);
    auto community = fmt::format("79{}:1", 45 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("79.{}.0.0", 45 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));

  /* Verify 3-way state: detached, blocked, in-sync */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_BLOCKED));
  EXPECT_FALSE(isPeerDetached(kPeerAddr4));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  XLOG(INFO, "=== TEST PASSED: ThreePeers_DetachedAndNearThreshold ===");
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
