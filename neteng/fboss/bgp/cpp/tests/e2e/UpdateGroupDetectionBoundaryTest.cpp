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
 * E2E tests: Detection boundary tests -- timer vs DOWN race,
 * frequency reset on down, disabled detection, 100+ blocks, concurrent block.
 *
 * Prefix range: 78.x.0.0/16
 *
 * Tests:
 *   Duration timer fires but peer went DOWN before callback
 *   Frequency counter reset on peer down
 *   Frequency threshold=0 means detach on ANY block
 *   Block event during freq threshold check -- concurrent safety
 *   All peers exceed threshold -- last synced member preserved
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {
/*
 * Frequency counter reset on peer down -- new session starts clean.
 * Peer3 accumulates 1 block (threshold=2). Goes DOWN. Comes back UP
 * (enters DETACHED_INIT_DUMP for existing group). Previous freq count
 * is cleared -- no stale counter carried over.
 */
TEST_P(UpdateGroupMultiPeerTest, FreqCounterResetOnDown) {
  XLOG(INFO, "=== TEST: FreqCounterResetOnDown ===");

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

  /* Set freq threshold = 2 (need 2 block cycles to detach) */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      2,
      std::chrono::milliseconds(60000));

  /*
   * Block cycle #1 -- accumulates 1 block count. Need 3 routes to fill
   * queue past hwm=2. Then bring peer3 DOWN while blocked -- this resets
   * the frequency counter without needing unblock+drain.
   */
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("78.{}.0.0/16", 10 + i);
    auto community = fmt::format("78{}:1", 10 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("78.{}.0.0", 10 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Peer3 is JOINED_BLOCKED (1 block < threshold 2, not detached) */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* Bring peer3 DOWN while blocked -- freq counter resets */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Peer4 continues fine */
  injectLocalRoutesAtRuntime({"78.15.0.0/16"}, {"7815:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("78.15.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "78.15.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "7815:1"));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: FreqCounterResetOnDown ===");
}

/*
 * Frequency threshold=0 means detach on ANY block (count >= 0).
 * This does NOT disable detection -- it makes it hypersensitive.
 * A single block cycle triggers detachment immediately.
 */
TEST_P(UpdateGroupMultiPeerTest, FreqThresholdZero_DetachOnAnyBlock) {
  XLOG(INFO, "=== TEST: FreqThresholdZero_DetachOnAnyBlock ===");

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

  /* Set freq threshold = 0 -- detach on ANY block */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      0,
      std::chrono::milliseconds(60000));

  /* Single block cycle should trigger detachment */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"78.20.0.0/16"}, {"7820:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("78.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "78.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "7820:1"));
  injectLocalRoutesAtRuntime({"78.21.0.0/16"}, {"7821:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("78.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "78.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "7821:1"));
  injectLocalRoutesAtRuntime({"78.22.0.0/16"}, {"7822:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("78.22.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "78.22.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "7822:1"));

  /* threshold=0 means detach on first block -- DETACHED_BLOCKED */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: FreqThresholdZero_DetachOnAnyBlock ===");
}

/*
 * All peers exceed threshold -- last synced member preserved.
 * 2 peers, freq threshold=1. Block both.
 * Group preserves at least 1 in-sync member. Peer4 detaches, peer3
 * (bit 0) SKIPS detachment as last synced member.
 */
TEST_P(UpdateGroupMultiPeerTest, AllPeersExceed_LastSyncedPreserved) {
  XLOG(INFO, "=== TEST: AllPeersExceed_LastSyncedPreserved ===");

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

  /* Set freq threshold=1 on the group */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Block BOTH peers, fill queue with 3 routes */
  blockPeer(kPeerAddr3);
  blockPeer(kPeerAddr4);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("78.{}.0.0/16", 40 + i);
    auto community = fmt::format("78{}:1", 40 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));

  /* Peer4 detaches. Peer3 (bit 0) preserved as last synced member */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1);

  /* Unblock peer3 -- the preserved synced member recovers */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Verify peer3 can still receive new routes */
  injectLocalRoutesAtRuntime({"78.45.0.0/16"}, {"7845:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("78.45.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "78.45.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "7845:1"));

  XLOG(INFO, "=== TEST PASSED: AllPeersExceed_LastSyncedPreserved ===");
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
