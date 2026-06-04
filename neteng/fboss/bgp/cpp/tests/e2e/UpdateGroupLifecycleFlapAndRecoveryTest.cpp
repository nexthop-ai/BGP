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
 * E2E tests: Lifecycle scenarios involving peer flapping, continuous route
 * injection, multi-peer recovery paths, withdrawal of cloned prefixes,
 * and route refresh independence.
 *
 * Prefix range: 35.x.0.0/16
 * Fixture: UpdateGroupLifecycleTest
 *
 * Tests implemented:
 *   Rapid peer flap (down/up/down/up) while another peer detached
 *   Continuous route injection, peer blocks then detaches — final routes
 *   3 peers, all different recovery paths (DFP, DSP, down+fresh)
 *   Detach + withdrawal of all cloned prefixes + recovery — empty set
 *   Detach + route refresh for in-sync peer + recovery — independent
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Rapid peer flap (down/up/down/up) on peer4 while peer3 is detached.
 * Detached peer unaffected, peer4 recovers and works after flaps. */
TEST_P(UpdateGroupLifecycleTest, RapidPeerFlapWhileDetached) {
  XLOG(INFO, "=== TEST: RapidPeerFlapWhileDetached ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* Fast peer with large queue */
  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr4);

  /* Slow peer with tiny queue — fills naturally without blockPeer */
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

  /* Inject a shared route so both peers have something */
  injectLocalRoutesAtRuntime({"35.1.0.0/16"}, {"3501:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3501:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3501:1"));

  /* Detach peer3 via freq threshold (1 block = detach) */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Need 3 fill routes to exceed hwm=2 (blocking requires queue > hwm) */
  for (int i = 2; i <= 4; i++) {
    auto prefix = fmt::format("35.{}.0.0/16", i);
    auto community = fmt::format("350{}:1", i);
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

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  XLOG(INFO, "Checkpoint 1: peer3 detached");

  /* Rapid flap peer4: down → up → down → up */
  bringDownPeer(kPeerAddr4);
  XLOG(INFO, "Checkpoint 2: peer4 flap #1 down");

  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  XLOG(INFO, "Checkpoint 3: peer4 flap #1 up");
  drainPeerQueueCompletely(peerId4);

  bringDownPeer(kPeerAddr4);
  XLOG(INFO, "Checkpoint 4: peer4 flap #2 down");

  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  XLOG(INFO, "Checkpoint 5: peer4 flap #2 up");
  drainPeerQueueCompletely(peerId4);

  /* peer3 should still be detached */
  ASSERT_TRUE(waitForPeerStateAny(
      kPeerAddr3,
      {PeerUpdateState::DETACHED_BLOCKED,
       PeerUpdateState::DETACHED_READY_TO_JOIN}));
  XLOG(INFO, "Checkpoint 6: peer3 still detached after peer4 flaps");

  /* Verify peer4 can still receive routes after flapping */
  injectLocalRoutesAtRuntime({"35.5.0.0/16"}, {"3505:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.5.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.5.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3505:1"));

  XLOG(INFO, "=== TEST PASSED: RapidPeerFlapWhileDetached ===");
}

/* Continuous route injection, peer blocks and detaches mid-stream.
 * Final routes still delivered to in-sync peer. */
TEST_P(UpdateGroupLifecycleTest, ContinuousInjectionBlockDetach) {
  XLOG(INFO, "=== TEST: ContinuousInjectionBlockDetach ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(3, 2, 0);

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

  /* Set freq threshold: 1 block triggers detachment */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Block peer3 and inject routes continuously (different communities) */
  blockPeer(kPeerAddr3);

  for (int i = 10; i <= 14; i++) {
    auto prefix = fmt::format("35.{}.0.0/16", i);
    auto community = fmt::format("35{:02d}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    /* Drain fast peer inline */
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("35.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* peer3 should be detached after the first block cycle */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  XLOG(INFO, "Checkpoint 1: peer3 detached after continuous injection");

  /* Inject more post-detach routes — only peer4 gets them */
  for (int i = 15; i <= 17; i++) {
    auto prefix = fmt::format("35.{}.0.0/16", i);
    auto community = fmt::format("35{:02d}:1", i);
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

  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: ContinuousInjectionBlockDetach ===");
}

/* 3 peers, different recovery paths: peer3 stays DB, peer4 DSP (DOWN),
 * peer5 stays in-sync throughout. */
TEST_P(UpdateGroupLifecycleTest, ThreePeersDifferentRecoveryPaths) {
  XLOG(INFO, "=== TEST: ThreePeersDifferentRecoveryPaths ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  /* Fast peer with large queue — stays in-sync throughout */
  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr5);

  /*
   * Peer4 gets medium queue (5,3,0) — won't accidentally detach during
   * phase 1 (peer3 detachment) since verifyRouteAdd drains it inline.
   * In phase 2, 4 fill routes exceed hwm=3 to detach peer4.
   */
  setDefaultQueueSizes(5, 3, 0);
  bringUpPeer(kPeerAddr4);

  /* Slow peer with tiny queue — fills naturally without blockPeer */
  setDefaultQueueSizes(3, 2, 0);
  bringUpPeer(kPeerAddr3);
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

  /* Inject a shared route */
  injectLocalRoutesAtRuntime({"35.20.0.0/16"}, {"3520:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.20.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3520:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3520:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.20.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3520:1"));
  XLOG(INFO, "Checkpoint 1: shared route delivered to all 3 peers");

  /* Detach peer3 via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Need 3 fill routes to exceed hwm=2 on peer3's (3,2,0) queue */
  for (int i = 21; i <= 23; i++) {
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
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("35.{}.0.0", i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  XLOG(INFO, "Checkpoint 2: peer3 detached (stays DB)");

  /* Detach peer4 via freq threshold then bring DOWN (DSP path) */
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Need 4 fill routes to exceed hwm=3 on peer4's (5,3,0) queue */
  for (int i = 24; i <= 27; i++) {
    auto prefix = fmt::format("35.{}.0.0/16", i);
    auto community = fmt::format("35{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("35.{}.0.0", i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));
  XLOG(INFO, "Checkpoint 3: peer4 detached");

  /* Bring peer4 DOWN (DSP path) */
  drainPeerQueueCompletely(peerId4);
  bringDownPeer(kPeerAddr4);
  XLOG(INFO, "Checkpoint 4: peer4 brought DOWN (DSP path)");

  /* Peer5 continues receiving routes normally */
  injectLocalRoutesAtRuntime({"35.28.0.0/16"}, {"3528:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.28.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.28.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3528:1"));

  verifySlowPeerInvariants(kPeerAddr5);
  XLOG(INFO, "=== TEST PASSED: ThreePeersDifferentRecoveryPaths ===");
}

/* Detach peer3, withdraw ALL shared prefixes. Per-peer set empties.
 * Verify peer4 processes withdrawals and group remains stable. */
TEST_P(UpdateGroupLifecycleTest, DetachWithdrawAllClonedPrefixes) {
  XLOG(INFO, "=== TEST: DetachWithdrawAllClonedPrefixes ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* Fast peer with large queue */
  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr4);

  /* Slow peer with tiny queue — fills naturally without blockPeer */
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

  /* Inject 3 shared routes pre-detach (each with different community) */
  for (int i = 30; i <= 32; i++) {
    auto prefix = fmt::format("35.{}.0.0/16", i);
    auto community = fmt::format("35{:02d}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("35.{}.0.0", i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("35.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  XLOG(INFO, "Checkpoint 1: 3 shared routes injected");

  /* Detach peer3 via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Need 3 fill routes to exceed hwm=2 on peer3's (3,2,0) queue */
  for (int i = 33; i <= 35; i++) {
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

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  XLOG(INFO, "Checkpoint 2: peer3 detached");

  /* Withdraw ALL 3 shared prefixes — CL accumulates for peer3 */
  for (int i = 30; i <= 32; i++) {
    auto prefix = fmt::format("35.{}.0.0/16", i);
    withdrawLocalRoutesAtRuntime({prefix});
    EXPECT_TRUE(
        verifyRouteWithdraw("v4", fmt::format("35.{}.0.0", i), 16, kPeerAddr4));
  }
  XLOG(INFO, "Checkpoint 3: all 3 shared routes withdrawn, peer4 drained");

  /* Inject a new route to prove group still works */
  injectLocalRoutesAtRuntime({"35.36.0.0/16"}, {"3536:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.36.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.36.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3536:1"));

  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: DetachWithdrawAllClonedPrefixes ===");
}

/* Detach peer3, route refresh simulation for in-sync peer4.
 * Independent operations — peer4 receives all routes, peer3 stays detached. */
TEST_P(UpdateGroupLifecycleTest, DetachRouteRefreshForInSyncPeer) {
  XLOG(INFO, "=== TEST: DetachRouteRefreshForInSyncPeer ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(5, 4, 0);

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

  /* Detach peer3 via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  /* Fill queue to trigger detachment — need enough to fill past hwm=4 */
  for (int i = 40; i <= 44; i++) {
    auto prefix = fmt::format("35.{}.0.0/16", i);
    auto community = fmt::format("35{:02d}:1", i);
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

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  XLOG(INFO, "Checkpoint 1: peer3 detached");

  /* Simulate route refresh for peer4 — burst of routes (different communities)
   * inject-drain one at a time to avoid order issues */
  for (int i = 45; i <= 48; i++) {
    auto prefix = fmt::format("35.{}.0.0/16", i);
    auto community = fmt::format("35{:02d}:1", i);
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
  XLOG(INFO, "Checkpoint 2: route refresh burst delivered to peer4");

  /* peer3 still detached — route refresh was independent */
  ASSERT_TRUE(waitForPeerStateAny(
      kPeerAddr3,
      {PeerUpdateState::DETACHED_BLOCKED,
       PeerUpdateState::DETACHED_READY_TO_JOIN}));

  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: DetachRouteRefreshForInSyncPeer ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupLifecycleTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
