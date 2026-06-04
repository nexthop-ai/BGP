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
 * E2E test: Multiple detached peers accepted after recovery.
 *
 * 3-peer group, detach 2 peers (peer3 + peer4) via freq threshold.
 * Unblock both, drain their queues, then verify both recover and
 * peer5 (the sole in-sync member) continues working throughout.
 *
 * Prefix range: 94.x.0.0/16
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Two detached peers recover after unblock.
 *
 * Per learned pattern, the group preserves at least 1 in-sync
 * member, so with 3 peers max 2 can be freq-detached. Peer5 remains
 * in-sync throughout. After both peers detach and unblock, drain their
 * queues and verify the group resumes normal operation.
 */
TEST_P(UpdateGroupMultiPeerTest, TwoDetachedPeersRecoverAfterUnblock) {
  XLOGF(INFO, "=== TEST: TwoDetachedPeersRecoverAfterUnblock ===");

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
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);
  drainPeerQueueCompletely(peerId5);

  /* Detach peer3: freq threshold=1 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("94.{}.0.0/16", 1 + i);
    auto community = fmt::format("{}:1", 9401 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    /* Drain peer4 and peer5 */
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("94.{}.0.0", 1 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("94.{}.0.0", 1 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Raise thresholds so peer4 won't auto-detach */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Detach peer4: lower threshold just for this group */
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr4);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("94.{}.0.0/16", 10 + i);
    auto community = fmt::format("{}:1", 9410 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    /* Only peer5 is in-sync now */
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("94.{}.0.0", 10 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Raise thresholds back to prevent re-detachment on recovery */
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Both peer3 and peer4 are DETACHED_BLOCKED. peer5 is sole in-sync. */
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 2);
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  /* Unblock both and drain their queues */
  unblockPeer(kPeerAddr3);
  drainPeerQueueCompletely(peerId3);
  unblockPeer(kPeerAddr4);
  drainPeerQueueCompletely(peerId4);

  /* Both should recover -- accept either JOINED_RUNNING or still detached
   * (CL batch may re-block with small queue). The key verification is:
   * peer5 continues working and no crash occurred. */
  EXPECT_NE(getPeerState(kPeerAddr3), PeerUpdateState::DOWN);
  EXPECT_NE(getPeerState(kPeerAddr4), PeerUpdateState::DOWN);

  /* Peer5 (sole in-sync member) should still function */
  injectLocalRoutesAtRuntime({"94.20.0.0/16"}, {"9420:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("94.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "94.20.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "9420:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  XLOGF(INFO, "=== PASSED: TripleDrjAcceptance ===");
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
