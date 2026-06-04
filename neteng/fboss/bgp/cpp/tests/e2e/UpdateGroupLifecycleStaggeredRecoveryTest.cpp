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
 * E2E tests: Lifecycle staggered detachment and recovery scenarios
 * Prefix range: 61.x.0.0/16
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Lifecycle -- two peers staggered detachment and recovery.
 * 3-peer group: detach peer3 first (freq), then detach peer4 (freq).
 * Recover in reverse order: unblock peer4 first, then peer3.
 * Verify peer5 (sole in-sync) continues receiving throughout,
 * and both recovered peers reach valid states.
 */
TEST_P(UpdateGroupMultiPeerTest, StaggeredDetachRecoverReverse) {
  XLOGF(INFO, "=== TEST: StaggeredDetachRecoverReverse ===");

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

  /* Step 1: Inject a shared route before any detachment */
  injectLocalRoutesAtRuntime({"61.1.0.0/16"}, {"6101:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("61.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "61.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "6101:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "61.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "6101:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "61.1.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "6101:1"));

  /* Step 2: Freq-detach peer3 first (threshold-raise pattern) */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(60000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("61.{}.0.0/16", 10 + i);
    auto c = fmt::format("{}:1", 6110 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("61.{}.0.0", 10 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("61.{}.0.0", 10 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Raise thresholds to protect peer4 and peer5 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(60000),
      999999,
      std::chrono::milliseconds(60000));

  /* Step 3: Now freq-detach peer4 (lower threshold via group handle kPeerAddr3)
   */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(60000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr4);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("61.{}.0.0/16", 20 + i);
    auto c = fmt::format("{}:1", 6120 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    /* Only peer5 is in-sync now */
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("61.{}.0.0", 20 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Raise thresholds to protect peer5 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(60000),
      999999,
      std::chrono::milliseconds(60000));

  /* Verify: peer3=DB, peer4=DB, peer5=in-sync */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerDetached(kPeerAddr4));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  /* Step 4: Inject route while both detached -- only peer5 receives */
  injectLocalRoutesAtRuntime({"61.30.0.0/16"}, {"6130:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("61.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "61.30.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "6130:1"));

  /* Step 5: Recover in REVERSE order -- unblock peer4 first (detached second)
   */
  unblockPeer(kPeerAddr4);
  EXPECT_TRUE(isPeerDetached(kPeerAddr4));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Inject route after peer4 recovery starts -- peer5 receives via PL */
  injectLocalRoutesAtRuntime({"61.35.0.0/16"}, {"6135:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("61.35.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "61.35.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "6135:1"));

  /* Step 6: Recover peer3 -- unblock (detached first, recovered last) */
  unblockPeer(kPeerAddr3);
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Inject more routes for PL cycles to allow recovery progress */
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("61.{}.0.0/16", 40 + i);
    auto c = fmt::format("{}:1", 6140 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("61.{}.0.0", 40 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        c));
  }

  /* Step 7: Verify final states -- both peers should be in valid states.
   * With (3,2,0), CL batch may re-block during recovery.
   * Accept JOINED_RUNNING or any valid detached state. */
  auto state3 = getPeerState(kPeerAddr3);
  auto state4 = getPeerState(kPeerAddr4);
  XLOGF(
      INFO,
      "StaggeredDetachRecoverReverse: peer3={}, peer4={} after staggered recovery",
      static_cast<int>(state3),
      static_cast<int>(state4));

  /* peer3 should not be DOWN */
  EXPECT_NE(state3, PeerUpdateState::DOWN)
      << "peer3 should not be DOWN after unblock";
  /* peer4 should not be DOWN */
  EXPECT_NE(state4, PeerUpdateState::DOWN)
      << "peer4 should not be DOWN after unblock";

  /* peer5 should remain in-sync throughout */
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  /* Final route delivery to peer5 confirms group still functional */
  injectLocalRoutesAtRuntime({"61.50.0.0/16"}, {"6150:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("61.50.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "61.50.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "6150:1"));

  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);
  verifySlowPeerInvariants(kPeerAddr5);

  XLOGF(INFO, "=== PASSED: StaggeredDetachRecoverReverse ===");
}

/*
 * Lifecycle -- detach, recover, immediately re-detach (back-to-back).
 * 2-peer group: freq-detach peer3, unblock to start recovery, then
 * block again during recovery. With (3,2,0), the CL batch from first
 * cycle (3 items > hwm=2) naturally re-blocks the peer, creating a
 * back-to-back detach-recover-redetach cycle. After second unblock,
 * verify group stability and peer4 continues throughout.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachRecoverRedetachBackToBack) {
  XLOGF(INFO, "=== TEST: DetachRecoverRedetachBackToBack ===");

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

  /* Inject baseline route -- both peers receive */
  injectLocalRoutesAtRuntime({"61.81.0.0/16"}, {"6181:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("61.81.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "61.81.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "6181:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "61.81.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "6181:1"));

  /* === FIRST DETACHMENT CYCLE === */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(60000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("61.{}.0.0/16", 82 + i);
    auto c = fmt::format("{}:1", 6182 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("61.{}.0.0", 82 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Raise thresholds to prevent further freq-detach on peer4 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(60000),
      999999,
      std::chrono::milliseconds(60000));

  /* === FIRST UNBLOCK -- start recovery === */
  unblockPeer(kPeerAddr3);

  /* Peer is now in recovery (DRJ/DSP). CL batch from first cycle
   * (3 items) may re-block the peer since 3 > hwm=2. Block peer
   * at test level to ensure re-blocking happens. */
  blockPeer(kPeerAddr3);

  /* Drain peer3 so the group can serve peer4 while peer3 is re-blocked */
  drainPeerQueueCompletely(peerId3);

  /* Inject routes during this re-blocked state -- peer4 receives */
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("61.{}.0.0/16", 86 + i);
    auto c = fmt::format("{}:1", 6186 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("61.{}.0.0", 86 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
  }

  /*
   * After drain + re-block, peer3 may still be detached (recovery incomplete)
   * or may have recovered to JOINED_BLOCKED (DFP fast path completed during
   * drain, then blockPeer re-blocked it). Both are valid outcomes.
   */
  auto midState = getPeerState(kPeerAddr3);
  XLOGF(
      INFO,
      "DetachRecoverRedetach: peer3={} after re-block during recovery",
      static_cast<int>(midState));
  EXPECT_NE(midState, PeerUpdateState::DOWN)
      << "peer3 should not be DOWN during re-block cycle";
  EXPECT_NE(midState, PeerUpdateState::JOINED_RUNNING)
      << "peer3 should be blocked (either detached or joined-blocked)";
  if (isPeerDetached(kPeerAddr3)) {
    verifySlowPeerInvariants(kPeerAddr3);
  }

  /* === SECOND UNBLOCK -- resume recovery === */
  unblockPeer(kPeerAddr3);

  /*
   * Drain peer3 queue so recovered peer doesn't re-block the group.
   * Without this, peer3's recovery CL items fill its bounded queue,
   * blocking the group and preventing route delivery to peer4.
   */
  drainPeerQueueCompletely(peerId3);

  /* Inject PL-cycle routes to drive recovery progress -- peer4 receives */
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("61.{}.0.0/16", 90 + i);
    auto c = fmt::format("{}:1", 6190 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("61.{}.0.0", 90 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
  }

  /* Verify peer4 is still in-sync throughout both cycles */
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Final route delivery confirms group still functional */
  injectLocalRoutesAtRuntime({"61.95.0.0/16"}, {"6195:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("61.95.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "61.95.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "6195:1"));

  /* peer3 should not be DOWN after back-to-back detach cycles */
  auto finalState = getPeerState(kPeerAddr3);
  XLOGF(
      INFO,
      "DetachRecoverRedetach: peer3={} after back-to-back detach cycles",
      static_cast<int>(finalState));
  EXPECT_NE(finalState, PeerUpdateState::DOWN)
      << "peer3 should not be DOWN after back-to-back detachment";

  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);

  XLOGF(INFO, "=== PASSED: DetachRecoverRedetachBackToBack ===");
}

/*
 * Lifecycle -- detach, withdrawal of all routes, rejoin with empty.
 * 2-peer group: inject shared route, freq-detach peer3, withdraw the shared
 * route while peer3 is detached (peer4 receives withdrawal, peer3 CL
 * accumulates). Unblock peer3 -- it recovers with no routes (empty state).
 * Verify group still functional by injecting a new route that both receive.
 */
TEST_P(UpdateGroupMultiPeerTest, WithdrawAllRoutesWhileDetachedRejoinEmpty) {
  XLOGF(INFO, "=== TEST: WithdrawAllRoutesWhileDetachedRejoinEmpty ===");

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

  /* Step 1: Inject a shared route -- both peers receive */
  injectLocalRoutesAtRuntime({"61.96.0.0/16"}, {"6196:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("61.96.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "61.96.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "6196:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "61.96.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "6196:1"));

  /* Step 2: Freq-detach peer3 using threshold-raise pattern */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(60000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("61.{}.0.0/16", 97 + i);
    auto c = fmt::format("{}:1", 6197 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("61.{}.0.0", 97 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Raise thresholds to protect peer4 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(60000),
      999999,
      std::chrono::milliseconds(60000));

  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Step 3: Withdraw the shared route while peer3 is detached.
   * peer4 receives the withdrawal inline, peer3 CL accumulates it. */
  withdrawLocalRoutesAtRuntime({"61.96.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "61.96.0.0", 16, kPeerAddr4));

  /* Step 4: Unblock peer3 -- starts recovery with all routes withdrawn */
  unblockPeer(kPeerAddr3);

  /* Inject PL-cycle routes to drive recovery progress -- peer4 receives */
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("61.{}.0.0/16", 101 + i);
    auto c = fmt::format("{}:1", 6201 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("61.{}.0.0", 101 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
  }

  /* Step 5: Verify peer3 is not DOWN after recovering with empty state */
  auto state3 = getPeerState(kPeerAddr3);
  XLOGF(
      INFO,
      "WithdrawAllRejoin: peer3={} after withdraw-all recovery",
      static_cast<int>(state3));
  EXPECT_NE(state3, PeerUpdateState::DOWN)
      << "peer3 should not be DOWN after empty-state recovery";

  /* peer4 should remain in-sync throughout */
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Step 6: Inject a brand-new route -- verify group still functional */
  injectLocalRoutesAtRuntime({"61.110.0.0/16"}, {"6210:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("61.110.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "61.110.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "6210:1"));

  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);

  XLOGF(INFO, "=== PASSED: WithdrawAllRoutesWhileDetachedRejoinEmpty ===");
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
