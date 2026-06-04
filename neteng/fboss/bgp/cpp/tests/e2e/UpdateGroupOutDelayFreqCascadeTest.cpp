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

/* E2E tests: Out-delay + peer down cleanup, out-delay with multiple peers,
 * frequency detection edge cases, and cascading detachment.
 *
 * Prefix range: 35.x.0.0/16
 * Fixture: UpdateGroupMultiPeerTest
 *
 * Tests:
 *   Out-delay + peer down — deferred entries cleaned up
 *   Out-delay with multiple peers having different delay values
 *   N-1 blocks in W seconds — no detach (below freq threshold)
 *   Rapid block/unblock cycling near frequency threshold
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Out-delay + peer down — deferred entries cleaned up.
 * With update groups enabled, out-delay is disabled (D98009838), so this
 * verifies that bringing a peer DOWN while routes are being processed
 * cleans up all pending entries for the downed peer without leaking. */
TEST_P(UpdateGroupMultiPeerTest, OutDelayPeerDownCleanup) {
  XLOG(INFO, "=== TEST: OutDelayPeerDownCleanup ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

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

  /* Inject several routes — drain one at a time to both peers */
  for (int i = 0; i < 3; ++i) {
    auto prefix = fmt::format("35.{}.0.0/16", 1 + i);
    auto community = fmt::format("{}:1", 3501 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("35.{}.0.0", 1 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("35.{}.0.0", 1 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Bring peer3 DOWN while routes exist */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Inject more routes — only peer4 receives them */
  injectLocalRoutesAtRuntime({"35.4.0.0/16"}, {"3504:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3504:1"));

  /* Withdraw a route — only peer4 gets the withdrawal */
  withdrawLocalRoutesAtRuntime({"35.1.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "35.1.0.0", 16, kPeerAddr4));

  /* Verify peer4 still operational after peer3 DOWN */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED ===");
}

/* Out-delay with multiple peers having different delay values.
 * Since out-delay is disabled with update groups (D98009838), this verifies
 * that multiple peers in the same group all receive routes at the same time
 * without any peer-specific delay differentiation. Uses 3 peers. */
TEST_P(UpdateGroupMultiPeerTest, MultiPeerNoDelayDifferentiation) {
  XLOG(INFO, "=== TEST: MultiPeerNoDelayDifferentiation ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  bringUpPeer(kPeerAddr5);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId5));
  EXPECT_TRUE(waitForEoR(peerId5));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  /* Inject routes — all 3 peers receive simultaneously (no delay) */
  for (int i = 0; i < 3; ++i) {
    auto prefix = fmt::format("35.{}.0.0/16", 10 + i);
    auto community = fmt::format("{}:1", 3510 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("35.{}.0.0", 10 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("35.{}.0.0", 10 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("35.{}.0.0", 10 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));
  }

  /* Bring one peer DOWN — remaining 2 still get routes together */
  bringDownPeer(kPeerAddr5);
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::DOWN));

  injectLocalRoutesAtRuntime({"35.13.0.0/16"}, {"3513:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.13.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3513:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.13.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3513:1"));

  verifySlowPeerInvariants(kPeerAddr3);
  XLOG(INFO, "=== TEST PASSED ===");
}

/* N-1 blocks in W seconds — no detach. Set frequency threshold to 3 block
 * events. Perform only 2 block/unblock cycles. Peer should remain
 * JOINED_RUNNING — frequency threshold NOT exceeded. */
TEST_P(UpdateGroupMultiPeerTest, BelowFreqThresholdNoDetach) {
  XLOG(INFO, "=== TEST: BelowFreqThresholdNoDetach ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  /* Per-peer queue sizes: large for peer4 (fast), small for peer3 (slow) */
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr4);
  setDefaultQueueSizes(3, 2, 0);
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Set threshold=3 — need 3 block events to trigger detachment */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      3,
      std::chrono::milliseconds(60000));

  /* Block cycle #1: 3 routes fill peer3's small (3,2,0) queue naturally */
  for (int i = 20; i <= 22; ++i) {
    auto prefix = fmt::format("35.{}.0.0/16", i);
    auto community = fmt::format("35{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("35.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));
  drainPeerQueueCompletely(peerId3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Block cycle #2: 3 more routes, same pattern */
  for (int i = 23; i <= 25; ++i) {
    auto prefix = fmt::format("35.{}.0.0/16", i);
    auto community = fmt::format("35{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("35.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));
  drainPeerQueueCompletely(peerId3);

  /* Peer3 should NOT be detached — only 2 blocks, threshold is 3 */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));

  /* Verify both peers still receive routes normally */
  injectLocalRoutesAtRuntime({"35.26.0.0/16"}, {"3526:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.26.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.26.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3526:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.26.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3526:1"));
  XLOG(INFO, "=== TEST PASSED ===");
}

/* Rapid block/unblock cycling near frequency threshold. Set threshold=3.
 * Do 3 rapid block/unblock cycles. On the 3rd block, peer3 should detach.
 * Tests that rapid cycling correctly increments the counter. */
TEST_P(UpdateGroupMultiPeerTest, RapidCyclingNearFreqThreshold) {
  XLOG(INFO, "=== TEST: RapidCyclingNearFreqThreshold ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  /* Per-peer queue sizes: large for peer4 (fast), small for peer3 (slow) */
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr4);
  setDefaultQueueSizes(3, 2, 0);
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Set threshold=3 — exactly 3 block events needed */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      3,
      std::chrono::milliseconds(60000));

  /* Rapid cycle #1: 3 routes fill peer3's (3,2,0) queue naturally */
  for (int i = 30; i <= 32; ++i) {
    auto prefix = fmt::format("35.{}.0.0/16", i);
    auto community = fmt::format("35{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("35.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));
  drainPeerQueueCompletely(peerId3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Rapid cycle #2: same pattern, 3 more routes */
  for (int i = 33; i <= 35; ++i) {
    auto prefix = fmt::format("35.{}.0.0/16", i);
    auto community = fmt::format("35{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("35.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));
  drainPeerQueueCompletely(peerId3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Rapid cycle #3 — this triggers detachment (counter=3=threshold) */
  for (int i = 36; i <= 38; ++i) {
    auto prefix = fmt::format("35.{}.0.0/16", i);
    auto community = fmt::format("35{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("35.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Peer3 should now be DETACHED_BLOCKED after 3rd block event */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);

  /* Peer4 still operational post-detachment */
  injectLocalRoutesAtRuntime({"35.39.0.0/16"}, {"3539:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.39.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.39.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3539:1"));

  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED ===");
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
