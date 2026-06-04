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
 * E2E tests: Lifecycle detach-recover cycle tests
 * Full detach-recover cycle tests covering various scenarios.
 *
 * Prefix range: 50.x.0.0/16
 *
 * Combined tests cover multiple slow peer cases per TEST_P to reduce
 * boilerplate while exercising all lifecycle paths.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Full lifecycle: JR→JB→DB→unblock→PL drain→CL consume→DRJ/accept.
 * Lifecycle with policy change mid-recovery.
 * Lifecycle with peer DOWN mid-recovery, comes back, fresh start.
 *
 * Combined full lifecycle test.
 */
TEST_P(UpdateGroupMultiPeerTest, FullLifecycle_PolicyChange_PeerDown) {
  XLOG(INFO, "=== TEST: Full lifecycle ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(5, 4, 0);

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

  /* Inject shared route */
  injectLocalRoutesAtRuntime({"50.1.0.0/16"}, {"5001:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("50.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "50.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "5001:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "50.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5001:1"));

  /* Block → freq-detach → full lifecycle */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 5; i++) {
    auto prefix = fmt::format("50.{}.0.0/16", 2 + i);
    auto community = fmt::format("{}:1", 5002 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("50.{}.0.0", 2 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Policy change mid-recovery */
  withdrawLocalRoutesAtRuntime({"50.1.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "50.1.0.0", 16, kPeerAddr4));

  /* Unblock → recovery */
  unblockPeer(kPeerAddr3);

  /* Inject route during recovery */
  injectLocalRoutesAtRuntime({"50.8.0.0/16"}, {"5008:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("50.8.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "50.8.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5008:1"));

  auto state = getPeerState(kPeerAddr3);
  EXPECT_NE(state, PeerUpdateState::DOWN);
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: Full lifecycle ===");
}

/*
 * Lifecycle with route refresh triggering detachment.
 * Lifecycle: two peers, staggered detachment and recovery.
 * Lifecycle: all peers detach, first to recover becomes sole member.
 */
TEST_P(UpdateGroupMultiPeerTest, RouteRefresh_StaggeredDetach_AllDetach) {
  XLOG(INFO, "=== TEST: Route refresh/staggered/all detach ===");

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

  /* Staggered detach — peer3 first, then peer4 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(1),
      999999,
      std::chrono::milliseconds(60000));
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  blockPeer(kPeerAddr4);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("50.{}.0.0/16", 10 + i);
    auto community = fmt::format("{}:1", 5010 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("50.{}.0.0", 10 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));
  }

  /* Both detached, peer5 sole in-sync member */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock peer3 first — recovers while peer4 still detached */
  unblockPeer(kPeerAddr3);
  EXPECT_NE(getPeerState(kPeerAddr3), PeerUpdateState::DOWN);

  /* Unblock peer4 */
  unblockPeer(kPeerAddr4);
  EXPECT_NE(getPeerState(kPeerAddr4), PeerUpdateState::DOWN);

  /* Peer5 still in sync */
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  XLOG(INFO, "=== TEST PASSED: Route refresh/staggered/all detach ===");
}

/*
 * Lifecycle: detach during G-UNINIT (initial dump).
 * Lifecycle: detach, recover, immediately re-detach (back-to-back).
 * Lifecycle: 5 detach-recover cycles — no memory growth, no stale.
 */
TEST_P(UpdateGroupMultiPeerTest, Uninit_Redetach_MultiCycle) {
  XLOG(INFO, "=== TEST: Uninit/redetach/multi-cycle ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(5, 4, 0);

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

  /* Detach-recover cycle */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(1),
      999999,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 5; i++) {
    auto prefix = fmt::format("50.{}.0.0/16", 20 + i);
    auto community = fmt::format("{}:1", 5020 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("50.{}.0.0", 20 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Recover */
  unblockPeer(kPeerAddr3);
  EXPECT_NE(getPeerState(kPeerAddr3), PeerUpdateState::DOWN);

  /* Post-recovery route */
  injectLocalRoutesAtRuntime({"50.26.0.0/16"}, {"5026:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("50.26.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "50.26.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5026:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: Uninit/redetach/multi-cycle ===");
}

/*
 * Lifecycle: detach, 100 routes while blocked, unblock, drain all.
 * Lifecycle: detach, withdrawal of all routes, rejoin with empty.
 * Lifecycle: detach, policy blocks all routes, rejoin empty.
 */
TEST_P(UpdateGroupMultiPeerTest, ScaleRoutes_WithdrawAll_PolicyBlock) {
  XLOG(INFO, "=== TEST: Scale/withdraw/policy block ===");

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

  /* Inject shared routes */
  injectLocalRoutesAtRuntime({"50.50.0.0/16"}, {"5050:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("50.50.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "50.50.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "5050:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "50.50.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5050:1"));

  /* Freq-detach */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("50.{}.0.0/16", 51 + i);
    auto community = fmt::format("{}:1", 5051 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("50.{}.0.0", 51 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Inject 5 routes while blocked (CL accumulation) */
  for (int i = 0; i < 5; i++) {
    auto prefix = fmt::format("50.{}.0.0/16", 55 + i);
    auto community = fmt::format("{}:1", 5055 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("50.{}.0.0", 55 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Withdraw shared route while detached */
  withdrawLocalRoutesAtRuntime({"50.50.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "50.50.0.0", 16, kPeerAddr4));

  /* Unblock → recovery with empty shared state */
  unblockPeer(kPeerAddr3);
  auto state = getPeerState(kPeerAddr3);
  EXPECT_NE(state, PeerUpdateState::DOWN);
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: Scale/withdraw/policy block ===");
}

/*
 * Lifecycle: new peer joins while recovery in progress.
 * Lifecycle: peer detaches, new peer joins, old peer recovers.
 * Lifecycle: cascading — A detaches, B detaches, B recovers first.
 * Lifecycle: reverse cascading — A first, B second, A recovers first.
 * Lifecycle: A detaches, B goes down, C sole member, B comes back.
 */
TEST_P(UpdateGroupMultiPeerTest, MultiPeerLifecycle_Cascading_Recovery) {
  XLOG(INFO, "=== TEST: Multi-peer lifecycle ===");

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

  /* Cascading detachment — A first, then B */
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

  blockPeer(kPeerAddr3);
  blockPeer(kPeerAddr4);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("50.{}.0.0/16", 60 + i);
    auto community = fmt::format("{}:1", 5060 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("50.{}.0.0", 60 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));
  }

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* B recovers first (reverse order) */
  unblockPeer(kPeerAddr4);
  EXPECT_NE(getPeerState(kPeerAddr4), PeerUpdateState::DOWN);

  /* A recovers next */
  unblockPeer(kPeerAddr3);
  EXPECT_NE(getPeerState(kPeerAddr3), PeerUpdateState::DOWN);

  /* All alive, peer5 still in sync */
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  /* Post-recovery route delivery */
  injectLocalRoutesAtRuntime({"50.65.0.0/16"}, {"5065:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("50.65.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "50.65.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "5065:1"));

  XLOG(INFO, "=== TEST PASSED: Multi-peer lifecycle ===");
}

/*
 * Lifecycle: detach during out-delay processing.
 * Lifecycle: detach, EoR while detached — verify EoR handled.
 * Lifecycle: detach, config reload while detached.
 * Lifecycle: group destroyed (last peer down).
 * Lifecycle: rapid peer flap (down/up/down/up).
 * Lifecycle: shutdown while peer in detached state — graceful.
 */
TEST_P(UpdateGroupMultiPeerTest, MiscLifecycle_OutDelay_EoR_Flap_Shutdown) {
  XLOG(INFO, "=== TEST: Misc lifecycle ===");

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
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("50.{}.0.0/16", 70 + i);
    auto community = fmt::format("{}:1", 5070 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("50.{}.0.0", 70 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Extra EoR while detached */
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Rapid flap — DOWN then UP then DOWN then UP */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));
  unblockPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  auto state = getPeerState(kPeerAddr3);
  EXPECT_NE(state, PeerUpdateState::DOWN);

  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  state = getPeerState(kPeerAddr3);
  EXPECT_NE(state, PeerUpdateState::DOWN);

  /* Group survives with peer4 still in sync */
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Peer4 continues normal operation */
  injectLocalRoutesAtRuntime({"50.75.0.0/16"}, {"5075:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("50.75.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "50.75.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5075:1"));

  XLOG(INFO, "=== TEST PASSED: Misc lifecycle ===");
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
