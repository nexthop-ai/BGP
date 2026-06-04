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
 * E2E tests: Lifecycle checkpoint verification and clone recovery scenarios.
 * Prefix range: 64.x.0.0/16
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Lifecycle checkpoint -- verify invariants at every state transition
 * during a full detach-recover cycle. Check getPeerState, isPeerInSync,
 * isPeerDetached, and verifySlowPeerInvariants at each stage:
 * JOINED_RUNNING -> block -> JOINED_BLOCKED -> fill -> DETACHED_BLOCKED ->
 * unblock -> recovery -> verify peer4 throughout.
 */
TEST_P(UpdateGroupMultiPeerTest, CheckpointAtEveryTransition) {
  XLOGF(INFO, "=== TEST: CheckpointAtEveryTransition ===");

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
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  /* Checkpoint 1: Both peers JOINED_RUNNING */
  XLOGF(INFO, "Checkpoint 1 -- both JOINED_RUNNING");
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));
  EXPECT_FALSE(isPeerDetached(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);

  /* Inject shared baseline route */
  injectLocalRoutesAtRuntime({"64.1.0.0/16"}, {"6401:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("64.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "64.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "6401:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "64.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "6401:1"));

  /* Set freq=1 for detachment */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Block peer3 */
  blockPeer(kPeerAddr3);

  /* Checkpoint 2: After block, before queue fill -- peer3 blocked at test level
   */
  XLOGF(INFO, "Checkpoint 2 -- peer3 blocked, not yet detached");
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  /* Fill queue with 3 routes to trigger detachment */
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("64.{}.0.0/16", 10 + i);
    auto c = fmt::format("{}:1", 6410 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("64.{}.0.0", 10 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Checkpoint 3: peer3 DETACHED_BLOCKED */
  XLOGF(INFO, "Checkpoint 3 -- peer3 DETACHED_BLOCKED");
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_FALSE(isPeerDetached(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);

  /* Raise thresholds to protect peer4 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Inject routes while detached -- CL accumulates for peer3 */
  for (int i = 0; i < 2; i++) {
    auto p = fmt::format("64.{}.0.0/16", 20 + i);
    auto c = fmt::format("{}:1", 6420 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("64.{}.0.0", 20 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
  }

  /* Checkpoint 4: CL items accumulated, peer3 still detached */
  XLOGF(INFO, "Checkpoint 4 -- CL items pending, peer3 still detached");
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  auto state3 = getPeerState(kPeerAddr3);
  EXPECT_NE(state3, PeerUpdateState::DOWN);
  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);

  /* Unblock to start recovery */
  unblockPeer(kPeerAddr3);

  /* Checkpoint 5: After unblock -- peer3 in recovery (DB or DRJ) */
  XLOGF(INFO, "Checkpoint 5 -- post-unblock recovery");
  auto state3Post = getPeerState(kPeerAddr3);
  XLOGF(INFO, "peer3 post-unblock state={}", static_cast<int>(state3Post));
  EXPECT_NE(state3Post, PeerUpdateState::DOWN);
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  /* Verify peer4 still receives routes after recovery starts */
  injectLocalRoutesAtRuntime({"64.30.0.0/16"}, {"6430:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("64.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "64.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "6430:1"));

  /* Final checkpoint: group stable, peer4 in-sync, peer3 non-DOWN */
  XLOGF(INFO, "Final checkpoint -- group stable");
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_NE(getPeerState(kPeerAddr3), PeerUpdateState::DOWN);
  verifySlowPeerInvariants(kPeerAddr4);

  XLOGF(INFO, "=== PASSED: CheckpointAtEveryTransition ===");
}

/*
 * Lifecycle -- 3 peers, all different recovery paths.
 * Detach peer4 first (less CL divergence), then peer5 (more CL divergence).
 * Unblock peer4 first (closer to DFP), then peer5 (DSP with more CL items).
 * peer3 stays in-sync throughout. Verify all reach stable state.
 */
TEST_P(UpdateGroupMultiPeerTest, ThreePeersDifferentRecoveryPaths) {
  XLOGF(INFO, "=== TEST: ThreePeersDifferentRecoveryPaths ===");

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

  /* Inject shared baseline route */
  injectLocalRoutesAtRuntime({"64.40.0.0/16"}, {"6440:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("64.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "64.40.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "6440:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "64.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "6440:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "64.40.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "6440:1"));

  /* Detach peer4 first -- threshold-raise pattern */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr4);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("64.{}.0.0/16", 41 + i);
    auto c = fmt::format("{}:1", 6441 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("64.{}.0.0", 41 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        c));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("64.{}.0.0", 41 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Raise thresholds before detaching peer5 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Inject more routes -- increases CL divergence for peer5 */
  for (int i = 0; i < 2; i++) {
    auto p = fmt::format("64.{}.0.0/16", 50 + i);
    auto c = fmt::format("{}:1", 6450 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("64.{}.0.0", 50 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        c));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("64.{}.0.0", 50 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        c));
  }

  /* Now detach peer5 -- freq=1 again */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr5);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("64.{}.0.0/16", 55 + i);
    auto c = fmt::format("{}:1", 6455 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("64.{}.0.0", 55 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId5));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::DETACHED_BLOCKED));

  /* Raise thresholds to protect peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Unblock peer4 first (less CL divergence -> closer to fast path) */
  unblockPeer(kPeerAddr4);
  auto state4 = getPeerState(kPeerAddr4);
  XLOGF(INFO, "peer4 post-unblock state={}", static_cast<int>(state4));
  EXPECT_NE(state4, PeerUpdateState::DOWN);

  /* Unblock peer5 (more CL divergence -> slow path) */
  unblockPeer(kPeerAddr5);
  auto state5 = getPeerState(kPeerAddr5);
  XLOGF(INFO, "peer5 post-unblock state={}", static_cast<int>(state5));
  EXPECT_NE(state5, PeerUpdateState::DOWN);

  /* Verify peer3 still works */
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  injectLocalRoutesAtRuntime({"64.60.0.0/16"}, {"6460:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("64.60.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "64.60.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "6460:1"));

  /* Final: all non-DOWN */
  EXPECT_NE(getPeerState(kPeerAddr3), PeerUpdateState::DOWN);
  EXPECT_NE(getPeerState(kPeerAddr4), PeerUpdateState::DOWN);
  EXPECT_NE(getPeerState(kPeerAddr5), PeerUpdateState::DOWN);
  verifySlowPeerInvariants(kPeerAddr3);

  XLOGF(INFO, "=== PASSED: ThreePeersDifferentRecoveryPaths ===");
}

/*
 * Lifecycle -- detach + lazy clones + recovery + collapse.
 * Inject shared route, detach peer3, withdraw shared route (triggers Case 4
 * clone preserving peer3's view), inject replacement. Unblock -> recovery
 * collapses per-peer entries. Verify peer4 sees withdrawal+replacement,
 * peer3 non-DOWN after recovery.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachCloneRecoveryCollapse) {
  XLOGF(INFO, "=== TEST: DetachCloneRecoveryCollapse ===");

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
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  /* Inject 3 shared routes before detachment -- these will be cloned */
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("64.{}.0.0/16", 70 + i);
    auto c = fmt::format("{}:1", 6470 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("64.{}.0.0", 70 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        c));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("64.{}.0.0", 70 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
  }

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("64.{}.0.0/16", 80 + i);
    auto c = fmt::format("{}:1", 6480 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("64.{}.0.0", 80 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Raise thresholds */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Withdraw 2 shared routes -- triggers Case 4 clone for peer3's view */
  withdrawLocalRoutesAtRuntime({"64.70.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "64.70.0.0", 16, kPeerAddr4));
  withdrawLocalRoutesAtRuntime({"64.71.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "64.71.0.0", 16, kPeerAddr4));

  /* Inject replacements with different prefixes */
  injectLocalRoutesAtRuntime({"64.85.0.0/16"}, {"6485:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("64.85.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "64.85.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "6485:1"));

  /* peer3 still detached, CL accumulated clone+withdrawal+add entries */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  verifySlowPeerInvariants(kPeerAddr3);

  /* Unblock -> recovery -> clones collapse during acceptance */
  unblockPeer(kPeerAddr3);
  auto state3 = getPeerState(kPeerAddr3);
  XLOGF(INFO, "peer3 post-unblock state={}", static_cast<int>(state3));
  EXPECT_NE(state3, PeerUpdateState::DOWN);

  /* Verify peer4 continues working after recovery */
  injectLocalRoutesAtRuntime({"64.90.0.0/16"}, {"6490:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("64.90.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "64.90.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "6490:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_NE(getPeerState(kPeerAddr3), PeerUpdateState::DOWN);
  verifySlowPeerInvariants(kPeerAddr4);

  XLOGF(INFO, "=== PASSED: DetachCloneRecoveryCollapse ===");
}

/*
 * Lifecycle -- detach + re-announcement with different attrs + recovery.
 * Inject shared route, detach peer3, withdraw shared route, re-inject same
 * prefix range with different community (attribute change). Unblock ->
 * recovery. Verify peer4 sees withdrawal then new route, peer3 non-DOWN.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachReannounceNewAttrsRecovery) {
  XLOGF(INFO, "=== TEST: DetachReannounceNewAttrsRecovery ===");

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
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  /* Inject shared route with original community */
  injectLocalRoutesAtRuntime({"64.130.0.0/16"}, {"64130:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("64.130.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "64.130.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "64130:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "64.130.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "64130:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("64.{}.0.0/16", 140 + i);
    auto c = fmt::format("{}:1", 64140 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("64.{}.0.0", 140 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Raise thresholds */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Withdraw shared route -- clone preserves peer3's old view */
  withdrawLocalRoutesAtRuntime({"64.130.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "64.130.0.0", 16, kPeerAddr4));

  /* Re-inject with DIFFERENT prefix (avoid CL suppression) + different attrs */
  injectLocalRoutesAtRuntime({"64.150.0.0/16"}, {"64150:99"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("64.150.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "64.150.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "64150:99"));

  /* peer3 detached, CL has withdrawal + new route entries */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Unblock -> recovery processes CL divergence */
  unblockPeer(kPeerAddr3);
  auto state3 = getPeerState(kPeerAddr3);
  XLOGF(INFO, "peer3 post-unblock state={}", static_cast<int>(state3));
  EXPECT_NE(state3, PeerUpdateState::DOWN);

  /* Verify group still functional */
  injectLocalRoutesAtRuntime({"64.160.0.0/16"}, {"64160:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("64.160.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "64.160.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "64160:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_NE(getPeerState(kPeerAddr3), PeerUpdateState::DOWN);
  verifySlowPeerInvariants(kPeerAddr4);

  XLOGF(INFO, "=== PASSED: DetachReannounceNewAttrsRecovery ===");
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
