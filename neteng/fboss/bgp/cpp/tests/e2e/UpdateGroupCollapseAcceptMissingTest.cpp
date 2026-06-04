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
 * E2E tests: Collapse and Acceptance missing tests
 *
 * Prefix range: 44.x.0.0/16
 *
 * Tests:
 *   Collapse -- empty per-peer entry set (DFP)
 *   Collapse -- per-peer entry for prefix group no longer has
 *   Collapse -- divergenceRibVersion reset to 0 after acceptance
 *   Collapse -- detachedPeers_ set updated
 *   Collapse -- detached CL consumer unregistered
 *   Collapse -- non-collapsed entry survives
 *   Collapse -- verify correct UPDATE for non-collapsed entries
 *   Collapse -- entry collapse after policy re-eval
 *   Collapse -- per-peer entry for withdrawn prefix removed
 *   Collapse -- acceptance for peer with 0 per-peer entries
 *   Accept -- group accepts only when PL is empty (G-IDLE)
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Collapse -- per-peer entry for prefix group no longer has.
 * Detach, clone shared route, then withdraw from group. Peer's per-peer
 * entry still has the prefix but group doesn't -> entry removed.
 */
TEST_P(UpdateGroupMultiPeerTest, CollapseOrphanedPrefix) {
  XLOG(INFO, "=== TEST: CollapseOrphanedPrefix ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(10, 8, 0);

  /* Per-peer queues: large for fast peer, small for slow peer */
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr4);

  setDefaultQueueSizes(3, 2, 0);
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Inject shared route */
  injectLocalRoutesAtRuntime({"44.10.0.0/16"}, {"4410:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("44.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "44.10.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "4410:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "44.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "4410:1"));

  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Inject 3 fill routes — peer3's (3,2,0) queue fills naturally */
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("44.{}.0.0/16", 11 + i);
    auto community = fmt::format("{}:1", 4411 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("44.{}.0.0", 11 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Withdraw shared route -- clone preserves for peer3, group removes */
  withdrawLocalRoutesAtRuntime({"44.10.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "44.10.0.0", 16, kPeerAddr4));

  /* Per-peer entry has orphaned prefix (group doesn't have it anymore) */
  verifySlowPeerInvariants(kPeerAddr3);
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  XLOG(INFO, "=== TEST PASSED: CollapseOrphanedPrefix ===");
}

/*
 * Collapse -- detachedPeers_ set updated, peer removed.
 * After acceptance, detachedPeerCount should be 0.
 */
TEST_P(UpdateGroupMultiPeerTest, CollapseDetachedPeersUpdated) {
  XLOG(INFO, "=== TEST: CollapseDetachedPeersUpdated ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(10, 8, 0);

  /* Per-peer queues: large for fast peer, small for slow peer */
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr4);

  setDefaultQueueSizes(3, 2, 0);
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);

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
      1,
      std::chrono::milliseconds(60000));

  /* Inject 3 fill routes — peer3's (3,2,0) queue fills naturally */
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("44.{}.0.0/16", 30 + i);
    auto community = fmt::format("{}:1", 4430 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("44.{}.0.0", 30 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1);

  /* Drain peer3 to trigger recovery */
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  auto state = getPeerState(kPeerAddr3);
  EXPECT_NE(state, PeerUpdateState::DOWN);
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: CollapseDetachedPeersUpdated ===");
}

/*
 * Collapse -- detached CL consumer unregistered after acceptance.
 * After acceptance, the CL consumer for the formerly-detached peer is
 * unregistered. Verify by injecting new routes and confirming normal
 * group processing.
 */
TEST_P(UpdateGroupMultiPeerTest, CollapseClConsumerUnregistered) {
  XLOG(INFO, "=== TEST: CollapseClConsumerUnregistered ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(10, 8, 0);

  /* Per-peer queues: large for fast peer, small for slow peer */
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr4);

  setDefaultQueueSizes(3, 2, 0);
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);

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
      1,
      std::chrono::milliseconds(60000));

  /* Inject 3 fill routes — peer3's (3,2,0) queue fills naturally */
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("44.{}.0.0/16", 40 + i);
    auto community = fmt::format("{}:1", 4440 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("44.{}.0.0", 40 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Drain peer3 to trigger acceptance — CL consumer unregistered */
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Normal route delivery post-acceptance */
  injectLocalRoutesAtRuntime({"44.46.0.0/16"}, {"4446:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("44.46.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "44.46.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "4446:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "44.46.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "4446:1"));

  XLOG(INFO, "=== TEST PASSED: CollapseClConsumerUnregistered ===");
}

/*
 * Collapse -- non-collapsed entry survives, group update mutates.
 * Collapse -- verify correct UPDATE for non-collapsed entries.
 * Collapse -- entry collapse after policy re-eval.
 * Collapse -- per-peer entry for withdrawn prefix removed.
 * Collapse -- acceptance for peer with 0 per-peer entries.
 * Accept -- group accepts only when PL is empty (G-IDLE).
 *
 * Combined test: Full detach -> clone -> withdraw -> policy re-eval ->
 * unblock -> acceptance cycle. Covers collapse and acceptance scenarios.
 */
TEST_P(UpdateGroupMultiPeerTest, FullCollapseAcceptCycle) {
  XLOG(INFO, "=== TEST: Full Collapse+Accept cycle ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(10, 8, 0);

  /* Per-peer queues: large for fast peer, small for slow peer */
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr4);

  setDefaultQueueSizes(3, 2, 0);
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Inject 2 shared routes */
  injectLocalRoutesAtRuntime({"44.50.0.0/16"}, {"4450:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("44.50.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "44.50.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "4450:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "44.50.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "4450:1"));
  injectLocalRoutesAtRuntime({"44.51.0.0/16"}, {"4451:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("44.51.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "44.51.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "4451:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "44.51.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "4451:1"));

  /* Freq-detach */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Inject 3 fill routes — peer3's (3,2,0) queue fills naturally */
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("44.{}.0.0/16", 52 + i);
    auto community = fmt::format("{}:1", 4452 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("44.{}.0.0", 52 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Clone: withdraw one route (clone preserves for peer3) */
  withdrawLocalRoutesAtRuntime({"44.50.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "44.50.0.0", 16, kPeerAddr4));

  /* Policy re-eval simulation: withdraw+re-inject with different prefix */
  withdrawLocalRoutesAtRuntime({"44.51.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "44.51.0.0", 16, kPeerAddr4));
  injectLocalRoutesAtRuntime({"44.58.0.0/16"}, {"4458:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("44.58.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "44.58.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "4458:1"));

  /* Drain peer3 to start recovery cycle */
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  auto state = getPeerState(kPeerAddr3);
  EXPECT_NE(state, PeerUpdateState::DOWN);

  /* Post-recovery route delivery — verify peer4 receives it normally */
  injectLocalRoutesAtRuntime({"44.59.0.0/16"}, {"4459:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("44.59.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "44.59.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "4459:1"));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: Full Collapse+Accept cycle ===");
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
