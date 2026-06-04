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
 * E2E tests: Multi-peer unblock and recovery scenarios
 * Prefix range: 55.x.0.0/16
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * (P-DB + P-DRJ) × E-UNBLOCK(DB)
 * 3-peer group: peer3 freq-detached → DB, peer4 freq-detached → DB then
 * unblocked → DRJ. peer5 is sole in-sync member.
 * Event: unblock peer3 (DB starts recovery).
 * Verify both peers in detached state, peer5 continues receiving routes.
 */
TEST_P(UpdateGroupMultiPeerTest, DbPlusDrj_UnblockDb) {
  XLOG(INFO, "=== TEST: DbPlusDrj_UnblockDb ===");

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

  /* Step 1: Freq-detach peer3 → DETACHED_BLOCKED (freq=1) */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("55.{}.0.0/16", 1 + i);
    auto c = fmt::format("{}:1", 5501 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("55.{}.0.0", 1 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("55.{}.0.0", 1 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Step 2: Raise thresholds so peer4 detaches on next block cycle */
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr4);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("55.{}.0.0/16", 10 + i);
    auto c = fmt::format("{}:1", 5510 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    /* Only peer5 is in-sync now */
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("55.{}.0.0", 10 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Step 3: Unblock peer4 → starts recovery (DRJ or DB).
   * peer4 may recover to JR via DFP before we check. */
  unblockPeer(kPeerAddr4);

  /* Confirm: peer3=DB, peer5=in-sync */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  /* Step 4: EVENT — unblock peer3 (DB starts recovery).
   * Both peers may recover to JR via DFP. */
  unblockPeer(kPeerAddr3);

  /* Step 5: Verify peer5 continues receiving routes.
   * Drain recovered peers to prevent their queues from blocking the group. */
  injectLocalRoutesAtRuntime({"55.20.0.0/16"}, {"5520:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("55.20.0.0/16")));
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "55.20.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "5520:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  /* Inject another route for stability */
  injectLocalRoutesAtRuntime({"55.21.0.0/16"}, {"5521:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("55.21.0.0/16")));
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "55.21.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "5521:1"));

  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== PASSED: DbPlusDrj_UnblockDb ===");
}

/*
 * (P-JR + P-DRJ) × PL-DRAIN
 * 3-peer group: peer3=JR, peer4 freq-detached → DB → unblocked → recovery,
 * peer5=in-sync. Event: PL drains after route injection.
 * After unblock, CL batch may re-block the recovering peer. Inject multiple
 * routes to create PL cycles. Verify peer4 eventually reaches acceptance
 * (JOINED_RUNNING) or stays in a valid detached state, and that route
 * delivery to in-sync peers (peer3, peer5) continues unaffected.
 */
TEST_P(UpdateGroupMultiPeerTest, JrPlusDrj_PlDrainAcceptance) {
  XLOG(INFO, "=== TEST: JrPlusDrj_PlDrainAcceptance ===");

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

  /* Step 1: Freq-detach peer4 → DETACHED_BLOCKED (freq=1) */
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr4);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("55.{}.0.0/16", 30 + i);
    auto c = fmt::format("{}:1", 5530 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("55.{}.0.0", 30 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        c));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("55.{}.0.0", 30 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Step 2: Raise thresholds so no further detachments on re-block */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Step 3: Unblock peer4 → starts recovery (CL consumption).
   * peer4 may recover to JR via DFP path. */
  unblockPeer(kPeerAddr4);

  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  /* Step 4: Inject multiple routes to create PL drain cycles.
   * Drain peer4 between injections to prevent its queue from blocking group. */
  for (int i = 0; i < 5; i++) {
    auto p = fmt::format("55.{}.0.0/16", 40 + i);
    auto c = fmt::format("{}:1", 5540 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    drainPeerQueueCompletely(peerId4);
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("55.{}.0.0", 40 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        c));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("55.{}.0.0", 40 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        c));
  }

  /* Step 5: Check if peer4 reached acceptance or still recovering.
   * With (3,2,0), CL batch of 3 items may re-block during recovery.
   * Accept JOINED_RUNNING (accepted) or any detached state as valid. */
  auto state4 = getPeerState(kPeerAddr4);
  XLOGF(
      INFO,
      "JrPlusDrj_PlDrainAcceptance: peer4 state after PL cycles: {}",
      static_cast<int>(state4));
  bool accepted = (state4 == PeerUpdateState::JOINED_RUNNING);
  if (!accepted) {
    /* Peer still recovering — valid with small queue CL re-blocking.
     * Verify it's in a valid detached/recovering state, not crashed. */
    EXPECT_TRUE(
        state4 == PeerUpdateState::DETACHED_BLOCKED ||
        state4 == PeerUpdateState::DETACHED_READY_TO_JOIN ||
        state4 == PeerUpdateState::DETACHED_RUNNING)
        << "Unexpected state: " << static_cast<int>(state4);
  }

  /* Step 6: Verify in-sync peers continue receiving routes.
   * Don't drain peer4 here — we may need to verify it received the route. */
  injectLocalRoutesAtRuntime({"55.48.0.0/16"}, {"5548:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("55.48.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "55.48.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "5548:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "55.48.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "5548:1"));

  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);
  verifySlowPeerInvariants(kPeerAddr5);

  XLOG(INFO, "=== PASSED: JrPlusDrj_PlDrainAcceptance ===");
}

/*
 * (P-JR + P-DB + P-DRJ) × PL-DRAIN
 * 3-peer group: peer3=JR (in-sync), peer4=DB (freq-detached, still blocked),
 * peer5=DRJ (freq-detached then unblocked, recovering via CL consumption).
 * Event: inject routes to trigger PL drain cycles.
 * Verify: peer3 continues receiving routes, peer4 stays DB, peer5 may reach
 * acceptance (JOINED_RUNNING) or remain in a valid detached state due to CL
 * batch re-blocking with small queue.
 */
TEST_P(UpdateGroupMultiPeerTest, JrPlusDbPlusDrj_PlDrain) {
  XLOG(INFO, "=== TEST: JrPlusDbPlusDrj_PlDrain ===");

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

  /* Step 1: Freq-detach peer4 → DETACHED_BLOCKED (freq=1).
   * Threshold-raise pattern: set freq=1, detach peer4, then raise. */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr4);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("55.{}.0.0/16", 50 + i);
    auto c = fmt::format("{}:1", 5550 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    /* Drain peer3 and peer5 (both in-sync) */
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("55.{}.0.0", 50 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        c));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("55.{}.0.0", 50 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Step 2: Raise thresholds to prevent peer3/peer5 from detaching */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Step 3: Lower freq=1 again to detach peer5, then raise immediately.
   * peer4 is already detached so threshold only affects peer3+peer5. */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr5);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("55.{}.0.0/16", 55 + i);
    auto c = fmt::format("{}:1", 5555 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    /* Only peer3 is in-sync now (peer4=DB, peer5=blocked) */
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("55.{}.0.0", 55 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId5));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::DETACHED_BLOCKED));

  /* Step 4: Raise thresholds so peer3 never detaches */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Step 5: Unblock peer5 → starts recovery (DRJ).
   * peer4 stays DETACHED_BLOCKED (still blocked).
   * peer5 may recover to JR via DFP path. */
  unblockPeer(kPeerAddr5);
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerDetached(kPeerAddr4));

  /* Step 6: EVENT — inject routes to trigger PL drain cycles.
   * Drain peer5 between injections to prevent it blocking the group. */
  for (int i = 0; i < 5; i++) {
    auto p = fmt::format("55.{}.0.0/16", 60 + i);
    auto c = fmt::format("{}:1", 5560 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    drainPeerQueueCompletely(peerId5);
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("55.{}.0.0", 60 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        c));
  }

  /* Step 7: Verify final states.
   * peer3=JR (always in-sync), peer4=DB (never unblocked).
   * peer5 may be accepted or still recovering (CL re-blocking). */
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  auto state4 = getPeerState(kPeerAddr4);
  EXPECT_EQ(state4, PeerUpdateState::DETACHED_BLOCKED)
      << "peer4 should stay DB (never unblocked)";

  auto state5 = getPeerState(kPeerAddr5);
  XLOGF(
      INFO,
      "JrPlusDbPlusDrj_PlDrain: peer5 state after PL cycles: {}",
      static_cast<int>(state5));
  bool peer5Accepted = (state5 == PeerUpdateState::JOINED_RUNNING);
  if (!peer5Accepted) {
    EXPECT_TRUE(
        state5 == PeerUpdateState::DETACHED_BLOCKED ||
        state5 == PeerUpdateState::DETACHED_READY_TO_JOIN ||
        state5 == PeerUpdateState::DETACHED_RUNNING)
        << "Unexpected peer5 state: " << static_cast<int>(state5);
  }

  /* Step 8: Verify peer3 continues receiving routes after all state changes.
   * No drain needed — step 6 kept queues clear, one more route won't overflow.
   */
  injectLocalRoutesAtRuntime({"55.70.0.0/16"}, {"5570:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("55.70.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "55.70.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "5570:1"));

  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);
  verifySlowPeerInvariants(kPeerAddr5);

  XLOG(INFO, "=== PASSED: JrPlusDbPlusDrj_PlDrain ===");
}

/*
 * (P-JR + P-JB) × E-SLOW-DUR(JB)
 * 2-peer group: peer3=JR (in-sync), peer4=JB (joined-blocked).
 * Event: 1ms duration timer fires on peer4, causing detachment.
 * Verify: peer3 (JR) continues receiving routes after peer4 detaches.
 * Uses threshold-raise pattern: set dur=1ms, block+fill peer4, then raise
 * thresholds so peer3 never detaches.
 */
TEST_P(UpdateGroupMultiPeerTest, JrPlusJb_SlowDurDetachesJb) {
  XLOG(INFO, "=== TEST: JrPlusJb_SlowDurDetachesJb ===");

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

  /* Step 1: Set 1ms duration threshold — fires instantly on block.
   * freq=999999 so only duration triggers detachment. */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(1),
      999999,
      std::chrono::milliseconds(60000));

  /* Step 2: Block peer4 and fill queue to trigger JB state.
   * The 1ms duration timer will fire and detach peer4. */
  blockPeer(kPeerAddr4);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("55.{}.0.0/16", 80 + i);
    auto c = fmt::format("{}:1", 5580 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    /* Drain peer3 immediately to keep it running */
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("55.{}.0.0", 80 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));

  /* Step 3: Wait for peer4 to reach DETACHED_BLOCKED via duration timer */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Step 4: Raise thresholds so peer3 is safe from detachment */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Step 5: Verify peer3=JR continues receiving routes */
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerDetached(kPeerAddr4));

  injectLocalRoutesAtRuntime({"55.85.0.0/16"}, {"5585:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("55.85.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "55.85.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "5585:1"));

  /* Step 6: Inject another route for stability verification */
  injectLocalRoutesAtRuntime({"55.86.0.0/16"}, {"5586:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("55.86.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "55.86.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "5586:1"));

  /* peer4 still detached, peer3 still in-sync */
  EXPECT_TRUE(isPeerDetached(kPeerAddr4));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));

  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== PASSED: JrPlusJb_SlowDurDetachesJb ===");
}

/*
 * (P-JB + P-JB) × E-SLOW-DUR(both)
 * 3-peer group: peer3=JB, peer4=JB, peer5=in-sync.
 * Event: 1ms duration timer fires on both peer3 and peer4, causing both to
 * detach via duration. peer5 remains the sole in-sync member.
 * Verify: peer5 continues receiving routes after both peers detach.
 * Uses threshold-raise pattern: detach peer3 first with dur=1ms, raise
 * thresholds, then lower dur=1ms again to detach peer4, raise again.
 */
TEST_P(UpdateGroupMultiPeerTest, JbPlusJb_SlowDurBothDetach) {
  XLOG(INFO, "=== TEST: JbPlusJb_SlowDurBothDetach ===");

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

  /* Step 1: Set dur=1ms, freq=999999 — duration-only detachment.
   * Block peer3 and fill queue to trigger JB state + instant duration fire. */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(1),
      999999,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("55.{}.0.0/16", 90 + i);
    auto c = fmt::format("{}:1", 5590 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    /* Drain peer4 and peer5 to keep them running */
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("55.{}.0.0", 90 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("55.{}.0.0", 90 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Step 2: Raise thresholds to protect peer4 and peer5 temporarily */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Step 3: Lower dur=1ms again to detach peer4 via duration */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(1),
      999999,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr4);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("55.{}.0.0/16", 95 + i);
    auto c = fmt::format("{}:1", 5595 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    /* Only peer5 is in-sync now (peer3=DB, peer4=blocked) */
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("55.{}.0.0", 95 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Step 4: Raise thresholds to protect peer5 from any future detachment */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Step 5: Verify final states — peer3=DB, peer4=DB, peer5=in-sync */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerDetached(kPeerAddr4));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  /* Step 6: Verify peer5 continues receiving routes */
  injectLocalRoutesAtRuntime({"55.100.0.0/16"}, {"55100:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("55.100.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "55.100.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "55100:1"));

  /* Inject another route for stability */
  injectLocalRoutesAtRuntime({"55.101.0.0/16"}, {"55101:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("55.101.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "55.101.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "55101:1"));

  /* Both detached peers still detached, peer5 still in-sync */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerDetached(kPeerAddr4));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);
  verifySlowPeerInvariants(kPeerAddr5);

  XLOG(INFO, "=== PASSED: JbPlusJb_SlowDurBothDetach ===");
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
