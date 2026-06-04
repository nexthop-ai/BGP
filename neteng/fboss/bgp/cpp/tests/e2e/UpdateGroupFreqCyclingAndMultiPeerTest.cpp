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

/* E2E tests: Frequency detection cycling and multi-peer detachment.
 * Prefix range: 27.10-27.43/16.
 *
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Rapid block/unblock cycling near threshold.
 * Set freq threshold=3 in 60s window. Do 2 block/unblock cycles (below
 * threshold), verify no detach. Then 3rd cycle — peer detaches. */
TEST_P(UpdateGroupMultiPeerTest, RapidBlockUnblockNearThreshold) {
  XLOG(INFO, "=== TEST: RapidBlockUnblockNearThreshold ===");

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

  /* Set freq threshold=3, large window */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      3,
      std::chrono::milliseconds(60000));

  /* Block cycle 1 */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"27.10.0.0/16"}, {"2710:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("27.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "27.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2710:1"));
  injectLocalRoutesAtRuntime({"27.11.0.0/16"}, {"2711:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("27.11.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "27.11.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2711:1"));
  injectLocalRoutesAtRuntime({"27.12.0.0/16"}, {"2712:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("27.12.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "27.12.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2712:1"));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));
  unblockPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));

  /* Block cycle 2 */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"27.13.0.0/16"}, {"2713:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("27.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "27.13.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2713:1"));
  injectLocalRoutesAtRuntime({"27.14.0.0/16"}, {"2714:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("27.14.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "27.14.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2714:1"));
  injectLocalRoutesAtRuntime({"27.15.0.0/16"}, {"2715:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("27.15.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "27.15.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2715:1"));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));
  unblockPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));

  /* Block cycle 3 — triggers freq detach (3rd block in window) */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"27.16.0.0/16"}, {"2716:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("27.16.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "27.16.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2716:1"));
  injectLocalRoutesAtRuntime({"27.17.0.0/16"}, {"2717:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("27.17.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "27.17.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2717:1"));
  injectLocalRoutesAtRuntime({"27.18.0.0/16"}, {"2718:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("27.18.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "27.18.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2718:1"));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: RapidBlockUnblockNearThreshold ===");
}

/* All peers block and exceed threshold — last synced preserved.
 * 3 peers, set freq threshold=1 on all. Block all, fill queue.
 * 2 of 3 detach; the last synced member is preserved (skips detachment).
 * Group still functions with 1 in-sync + 2 detached. No crash. */
TEST_P(UpdateGroupMultiPeerTest, AllPeersBlock_LastSyncedPreserved) {
  XLOG(INFO, "=== TEST: AllPeersBlock_LastSyncedPreserved ===");

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

  /* Set freq threshold=1 on ALL peers */
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
  setSlowPeerThresholds(
      kPeerAddr5,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Block ALL peers */
  blockPeer(kPeerAddr3);
  blockPeer(kPeerAddr4);
  blockPeer(kPeerAddr5);

  /* Inject routes to trigger queue fill and freq threshold checks */
  injectLocalRoutesAtRuntime({"27.40.0.0/16"}, {"2740:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("27.40.0.0/16")));
  injectLocalRoutesAtRuntime({"27.41.0.0/16"}, {"2741:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("27.41.0.0/16")));
  injectLocalRoutesAtRuntime({"27.42.0.0/16"}, {"2742:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("27.42.0.0/16")));

  /* All blocked */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId5));

  /* 2 detach, 1 preserved as last in-sync member (peer5 and peer4 detach,
   * peer3 at bit 0 is preserved). */
  EXPECT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr5));
  EXPECT_TRUE(isPeerDetached(kPeerAddr4));

  /* peer3 (bit 0) is the last synced member — NOT detached */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));

  /* Group still functions — inject more routes, no crash */
  injectLocalRoutesAtRuntime({"27.43.0.0/16"}, {"2743:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("27.43.0.0/16")));

  /* Verify group has 2 detached peers */
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 2);

  /* Cleanup: unblock remaining in-sync peer and drain queue to prevent
   * teardown race (coroutine accesses destroyed group data) */
  unblockPeer(kPeerAddr3);
  drainPeerQueueCompletely(peerId3);

  XLOG(INFO, "=== TEST PASSED: AllPeersBlock_LastSyncedPreserved ===");
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
