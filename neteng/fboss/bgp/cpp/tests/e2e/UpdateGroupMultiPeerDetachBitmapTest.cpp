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

/* E2E tests: Multi-peer detachment and simultaneous bitmap operations.
 * Prefix range: 55.10-55.44/16.
 *
 * Detach 2 peers from 3-peer group (1 synced remaining)
 * Detach two peers simultaneously — bitmap operations
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Detach 2 peers from 3-peer group — 1 synced peer remaining.
 * E2E supports max 3 peers (peer3, peer4, peer5). Detach peer3 and peer4
 * via freq threshold, leaving only peer5 in sync.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachTwoPeersFromThreePeerGroup) {
  XLOG(INFO, "=== TEST: DetachTwoPeersFromThreePeerGroup ===");

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

  /* Detach peer3 first via freq threshold=1 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);

  for (int i = 10; i <= 12; i++) {
    auto prefix = fmt::format("55.{}.0.0/16", i);
    auto community = fmt::format("55{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("55.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("55.{}.0.0", i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Now detach peer4 via freq threshold=1 */
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr4);

  for (int i = 13; i <= 15; i++) {
    auto prefix = fmt::format("55.{}.0.0/16", i);
    auto community = fmt::format("55{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("55.{}.0.0", i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Both peer3 and peer4 detached, only peer5 remains in sync */
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr5), 2);
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));
  EXPECT_FALSE(isPeerInSync(kPeerAddr4));

  /* Verify peer5 still receives routes normally */
  injectLocalRoutesAtRuntime({"55.16.0.0/16"}, {"5516:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("55.16.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "55.16.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "5516:1"));

  verifySlowPeerInvariants(kPeerAddr5);

  XLOG(INFO, "=== TEST PASSED: DetachTwoPeersFromThreePeerGroup ===");
}

/* Detach two peers at same time — verify bitmap operations.
 * Use 3 peers. Block peer3 and peer4 simultaneously, fill queue to trigger
 * freq detach on both. Bitmap should correctly track 2 detached peers.
 */
TEST_P(UpdateGroupMultiPeerTest, SimultaneousDetachBitmapOps) {
  XLOG(INFO, "=== TEST: SimultaneousDetachBitmapOps ===");

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

  /* Set freq threshold=1 on both peer3 and peer4 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Block both peer3 and peer4 */
  blockPeer(kPeerAddr3);
  blockPeer(kPeerAddr4);

  /* Fill queue with 3 routes — only peer5 drains */
  for (int i = 40; i <= 42; i++) {
    auto prefix = fmt::format("55.{}.0.0/16", i);
    auto community = fmt::format("55{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("55.{}.0.0", i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));

  /* Both should be detached. The group preserves
   * at least 1 in-sync member. With peer5 as the only non-blocked peer,
   * both peer3 and peer4 should be detached.
   */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Verify bitmap: 2 detached, peer5 in sync */
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr5), 2);
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));
  EXPECT_FALSE(isPeerInSync(kPeerAddr4));

  /* Verify peer5 continues to receive routes */
  injectLocalRoutesAtRuntime({"55.43.0.0/16"}, {"5543:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("55.43.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "55.43.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "5543:1"));

  /* Bring peer3 DOWN to test selective cleanup */
  bringDownPeer(kPeerAddr3);

  /* peer5 should still work, peer4 still detached */
  injectLocalRoutesAtRuntime({"55.44.0.0/16"}, {"5544:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("55.44.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "55.44.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "5544:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr5));
  verifySlowPeerInvariants(kPeerAddr5);

  XLOG(INFO, "=== TEST PASSED: SimultaneousDetachBitmapOps ===");
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
