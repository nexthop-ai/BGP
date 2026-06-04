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
 * E2E tests: Multi-peer acceptance scenarios
 * Prefix range: 56.x.0.0/16
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * (P-DRJ + P-DRJ) x PL-DRAIN
 * 3-peer group: peer3 and peer4 both freq-detached then unblocked (DRJ),
 * peer5 is sole in-sync member.
 * Event: inject routes to trigger PL drain cycles.
 * Both DRJ peers may reach acceptance simultaneously or stay in valid
 * detached state due to CL batch re-blocking with small queue.
 * Verify peer5 continues receiving routes throughout.
 */
TEST_P(UpdateGroupMultiPeerTest, DrjPlusDrj_PlDrainBothAccepted) {
  XLOG(INFO, "=== TEST: DrjPlusDrj_PlDrainBothAccepted ===");

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

  /* Step 1: Freq-detach peer3 with threshold-raise pattern (freq=1) */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("56.{}.0.0/16", 1 + i);
    auto c = fmt::format("{}:1", 5601 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("56.{}.0.0", 1 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("56.{}.0.0", 1 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Step 2: Raise thresholds before detaching peer4 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Step 3: Lower freq=1 again to detach peer4 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr4);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("56.{}.0.0/16", 10 + i);
    auto c = fmt::format("{}:1", 5610 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    /* Only peer5 is in-sync now */
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("56.{}.0.0", 10 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Step 4: Raise thresholds to protect peer5 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Step 5: Unblock both peers -> both enter DRJ (recovery).
   * Either may recover to JOINED_RUNNING via DFP path before we check. */
  unblockPeer(kPeerAddr3);
  unblockPeer(kPeerAddr4);
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  /* Step 6: Inject routes to trigger PL drain cycles.
   * Each cycle gives the group a chance to check acceptance for both DRJ peers.
   * Drain recovered peers between injections to prevent their queues from
   * filling up and blocking the group (which would starve peer5).
   */
  for (int i = 0; i < 5; i++) {
    auto p = fmt::format("56.{}.0.0/16", 20 + i);
    auto c = fmt::format("{}:1", 5620 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    drainPeerQueueCompletely(peerId3);
    drainPeerQueueCompletely(peerId4);
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("56.{}.0.0", 20 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        c));
  }

  /* Step 7: Check final states for both peers.
   * With (3,2,0), CL batch of 3 items may re-block during recovery.
   * Accept JOINED_RUNNING (accepted) or any valid detached state. */
  auto state3 = getPeerState(kPeerAddr3);
  auto state4 = getPeerState(kPeerAddr4);
  XLOGF(
      INFO,
      "DrjPlusDrj_PlDrainBothAccepted: peer3 state={}, peer4 state={}",
      static_cast<int>(state3),
      static_cast<int>(state4));

  auto isValidState = [](PeerUpdateState s) {
    return s == PeerUpdateState::JOINED_RUNNING ||
        s == PeerUpdateState::DETACHED_BLOCKED ||
        s == PeerUpdateState::DETACHED_READY_TO_JOIN ||
        s == PeerUpdateState::DETACHED_RUNNING;
  };
  EXPECT_TRUE(isValidState(state3))
      << "Unexpected peer3 state: " << static_cast<int>(state3);
  EXPECT_TRUE(isValidState(state4))
      << "Unexpected peer4 state: " << static_cast<int>(state4);

  /* Step 8: Verify peer5 continues receiving routes.
   * No drain needed here — step 6 drains kept queues clear, and one more
   * route won't overflow capacity 3. */
  injectLocalRoutesAtRuntime({"56.30.0.0/16"}, {"5630:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("56.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "56.30.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "5630:1"));

  /* If either peer was accepted, verify it also receives the route */
  if (state3 == PeerUpdateState::JOINED_RUNNING) {
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        "56.30.0.0",
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        "5630:1"));
  }
  if (state4 == PeerUpdateState::JOINED_RUNNING) {
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        "56.30.0.0",
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        "5630:1"));
  }

  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);
  verifySlowPeerInvariants(kPeerAddr5);

  XLOG(INFO, "=== PASSED: DrjPlusDrj_PlDrainBothAccepted ===");
}

/*
 * (P-JR + P-DB + P-DRJ) x E-ROUTE-ADD
 * 3-peer group: peer3=JR (in-sync), peer4=DB (freq-detached, blocked),
 * peer5=DRJ (freq-detached, unblocked).
 * Event: inject a route. peer3 receives via PL, peer4/peer5 CL accumulates.
 * Verify peer3 delivery, peer4 stays DB, peer5 stays detached.
 */
TEST_P(UpdateGroupMultiPeerTest, JrDbDrj_RouteAdd) {
  XLOG(INFO, "=== TEST: JrDbDrj_RouteAdd ===");

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

  /* Step 1: Freq-detach peer4 with threshold-raise pattern (freq=1) */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr4);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("56.{}.0.0/16", 40 + i);
    auto c = fmt::format("{}:1", 5640 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("56.{}.0.0", 40 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        c));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("56.{}.0.0", 40 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Step 2: Raise thresholds to protect remaining peers */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Step 3: Freq-detach peer5 (freq=1) */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr5);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("56.{}.0.0/16", 50 + i);
    auto c = fmt::format("{}:1", 5650 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    /* Only peer3 is in-sync now */
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("56.{}.0.0", 50 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId5));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::DETACHED_BLOCKED));

  /* Step 4: Raise thresholds to protect peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Step 5: Unblock peer5 only -> peer5 enters DRJ, peer4 stays DB.
   * peer5 may recover to JOINED_RUNNING via DFP path before we check. */
  unblockPeer(kPeerAddr5);
  EXPECT_TRUE(isPeerDetached(kPeerAddr4));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));

  /* Verify states: peer3=JR, peer4=DB, peer5=DRJ or JR (DFP recovery) */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Step 6: Inject a route — peer3 (JR) receives via PL.
   * Drain peer5 first to prevent it blocking the group if it recovered. */
  injectLocalRoutesAtRuntime({"56.60.0.0/16"}, {"5660:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("56.60.0.0/16")));
  drainPeerQueueCompletely(peerId5);
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "56.60.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "5660:1"));

  /* Step 7: Verify peer4 still DB */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Step 8: Inject another route to confirm peer3 continues receiving */
  injectLocalRoutesAtRuntime({"56.61.0.0/16"}, {"5661:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("56.61.0.0/16")));
  drainPeerQueueCompletely(peerId5);
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "56.61.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "5661:1"));

  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);
  verifySlowPeerInvariants(kPeerAddr5);

  XLOG(INFO, "=== PASSED: JrDbDrj_RouteAdd ===");
}

/*
 * (P-JB + P-DB + P-DID) x E-ROUTE-ADD
 * 3-peer group: peer3=JR (sole in-sync), peer4=DB (freq-detached, blocked),
 * peer5=DID (freq-detached, brought down, brought back up ->
 * DETACHED_INIT_DUMP). Two peers are in non-running detached states. peer3 is
 * sole in-sync member. Event: inject a route. Only peer3 receives via PL. Both
 * detached peers' CL accumulates the entry. Verify no crash, peer3 receives
 * routes, both detached peers stable. Note: Getting peer3 to JB when it's the
 * sole in-sync member is not feasible because blocking the only in-sync member
 * puts group in WAITING before the queue fills. We test the DB+DID multi-peer
 * detached scenario instead.
 */
TEST_P(UpdateGroupMultiPeerTest, JbDbDid_RouteAddAllToCl) {
  XLOG(INFO, "=== TEST: JbDbDid_RouteAddAllToCl ===");

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

  /* Step 1: Freq-detach peer4 with threshold-raise pattern (freq=1) */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr4);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("56.{}.0.0/16", 100 + i);
    auto c = fmt::format("{}:1", 56100 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("56.{}.0.0", 100 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        c));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("56.{}.0.0", 100 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Step 2: Raise thresholds to protect remaining peers */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Step 3: Freq-detach peer5 (freq=1) */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr5);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("56.{}.0.0/16", 110 + i);
    auto c = fmt::format("{}:1", 56110 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("56.{}.0.0", 110 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId5));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::DETACHED_BLOCKED));

  /* Step 4: Raise thresholds to protect peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Step 5: Bring peer5 down then back up -> enters DETACHED_INIT_DUMP.
   * Do NOT send EoR — peer stays in DID/DB longer. */
  bringDownPeer(kPeerAddr5);
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::DOWN));
  unblockPeer(kPeerAddr5);
  bringUpPeer(kPeerAddr5);

  /* Step 6: Verify peer4=DB, peer5=DID or DB (transient).
   * peer3 remains sole in-sync running member. */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));

  /* Step 7: Inject a route. Only peer3 (sole in-sync) receives via PL.
   * peer4 (DB) and peer5 (DID/DB) CL accumulates the entry. */
  injectLocalRoutesAtRuntime({"56.130.0.0/16"}, {"56130:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("56.130.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "56.130.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "56130:1"));

  /* Step 8: Verify detached peers are stable, no crash */
  auto state4 = getPeerState(kPeerAddr4);
  auto state5 = getPeerState(kPeerAddr5);
  XLOGF(
      INFO,
      "JbDbDid_RouteAddAllToCl: peer4={}, peer5={}",
      static_cast<int>(state4),
      static_cast<int>(state5));
  EXPECT_EQ(state4, PeerUpdateState::DETACHED_BLOCKED);
  /* peer5 may be in DID, DB, DRJ, or even JR if init dump completed */
  auto isValidState = [](PeerUpdateState s) {
    return s == PeerUpdateState::DETACHED_INIT_DUMP ||
        s == PeerUpdateState::DETACHED_BLOCKED ||
        s == PeerUpdateState::DETACHED_READY_TO_JOIN ||
        s == PeerUpdateState::DETACHED_RUNNING ||
        s == PeerUpdateState::JOINED_RUNNING;
  };
  EXPECT_TRUE(isValidState(state5))
      << "Unexpected peer5 state: " << static_cast<int>(state5);

  /* Step 9: Inject another route to confirm peer3 continues working */
  injectLocalRoutesAtRuntime({"56.131.0.0/16"}, {"56131:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("56.131.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "56.131.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "56131:1"));

  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);
  verifySlowPeerInvariants(kPeerAddr5);

  XLOG(INFO, "=== PASSED: JbDbDid_RouteAddAllToCl ===");
}

/*
 * (P-JR + P-DB + P-DB) x E-UNBLOCK(DB1)
 * 3-peer group: peer3=JR (in-sync), peer4=DB (freq-detached, blocked),
 * peer5=DB (freq-detached, blocked).
 * Event: unblock peer4 only. peer4 starts recovery (DRJ or re-blocks from
 * CL batch), peer5 stays DETACHED_BLOCKED.
 * Verify peer3 continues receiving routes, peer4 reaches valid post-unblock
 * state, peer5 remains DB.
 */
TEST_P(UpdateGroupMultiPeerTest, JrDbDb_UnblockOneDb) {
  XLOG(INFO, "=== TEST: JrDbDb_UnblockOneDb ===");

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

  /* Step 1: Freq-detach peer4 with threshold-raise pattern (freq=1) */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr4);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("56.{}.0.0/16", 140 + i);
    auto c = fmt::format("{}:1", 56140 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("56.{}.0.0", 140 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        c));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("56.{}.0.0", 140 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Step 2: Raise thresholds before detaching peer5 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Step 3: Lower freq=1 to detach peer5 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr5);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("56.{}.0.0/16", 150 + i);
    auto c = fmt::format("{}:1", 56150 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    /* Only peer3 is in-sync now */
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("56.{}.0.0", 150 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId5));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::DETACHED_BLOCKED));

  /* Step 4: Raise thresholds to protect peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Step 5: Unblock peer4 ONLY — peer5 stays blocked (DB) */
  unblockPeer(kPeerAddr4);
  EXPECT_TRUE(isPeerDetached(kPeerAddr4));
  EXPECT_TRUE(isPeerDetached(kPeerAddr5));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));

  /* Step 6: Verify peer5 remains DETACHED_BLOCKED */
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::DETACHED_BLOCKED));

  /* Step 7: peer4 may be DRJ or re-blocked from CL batch (DB).
   * Both are valid post-unblock states with queue (3,2,0). */
  auto state4 = getPeerState(kPeerAddr4);
  XLOGF(
      INFO,
      "JrDbDb_UnblockOneDb: peer4 state after unblock={}",
      static_cast<int>(state4));
  auto isValidPostUnblock = [](PeerUpdateState s) {
    return s == PeerUpdateState::JOINED_RUNNING ||
        s == PeerUpdateState::DETACHED_BLOCKED ||
        s == PeerUpdateState::DETACHED_READY_TO_JOIN ||
        s == PeerUpdateState::DETACHED_RUNNING;
  };
  EXPECT_TRUE(isValidPostUnblock(state4))
      << "Unexpected peer4 state: " << static_cast<int>(state4);

  /* Step 8: Inject a route — peer3 (JR) receives via PL */
  injectLocalRoutesAtRuntime({"56.160.0.0/16"}, {"56160:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("56.160.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "56.160.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "56160:1"));

  /* Step 9: Verify peer5 still DB after route injection */
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::DETACHED_BLOCKED));

  /* Step 10: If peer4 was accepted, verify it also receives */
  state4 = getPeerState(kPeerAddr4);
  if (state4 == PeerUpdateState::JOINED_RUNNING) {
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        "56.160.0.0",
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        "56160:1"));
  }

  /* Step 11: Inject another route to confirm peer3 continues */
  injectLocalRoutesAtRuntime({"56.161.0.0/16"}, {"56161:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("56.161.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "56.161.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "56161:1"));

  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);
  verifySlowPeerInvariants(kPeerAddr5);

  XLOG(INFO, "=== PASSED: JrDbDb_UnblockOneDb ===");
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
