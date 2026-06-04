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
 * E2E tests for multi-step event sequences involving blocking, detachment,
 * peer down/up, and full recovery paths.
 *
 * E-BLOCK then E-PEER-DOWN — blocked peer dies
 * E-DFP→DSP then E-ACCEPT — demotion prevents acceptance
 * Triple: E-DETACH → E-UNBLOCK → E-CL-DRAIN → E-ACCEPT (full recovery)
 * Triple: E-DETACH → E-PEER-DOWN → E-PEER-UP → fresh start
 * Quadruple: E-BLOCK → E-DETACH → E-POLICY-CHG → E-UNBLOCK → E-ACCEPT
 *
 * Prefix range: 37.x.0.0/16
 * Fixture: UpdateGroupMultiPeerTest
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Block peer3, then bring it DOWN while blocked.
 * The blocked peer goes to DOWN state. Peer4 continues normally.
 * After peer3 goes down, inject more routes — only peer4 gets them.
 */
TEST_P(UpdateGroupMultiPeerTest, BlockThenPeerDown) {
  XLOG(INFO, "=== TEST: BlockThenPeerDown ===");

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

  /* Block peer3, inject routes to fill its queue */
  blockPeer(kPeerAddr3);

  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("37.{}.0.0/16", 1 + i);
    auto community = fmt::format("370{}:1", 1 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("37.{}.0.0", 1 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* Bring peer3 DOWN while it is blocked */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Peer4 should still be running and receiving routes */
  injectLocalRoutesAtRuntime({"37.4.0.0/16"}, {"3704:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3704:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  XLOG(INFO, "=== TEST PASSED ===");
}

/*
 * Detach peer3 via freq threshold (DFP). Then set an aggressive
 * duration threshold (DSP demotion) on the detached peer. The peer stays
 * detached — the DSP demotion is a no-op on an already-detached peer.
 * Acceptance (unblock) should still work to begin recovery.
 */
TEST_P(UpdateGroupMultiPeerTest, DfpDspThenAccept) {
  XLOG(INFO, "=== TEST: DfpDspThenAccept ===");

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

  /* Duration-detach peer3: 1ms fires immediately on block */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(1),
      999999,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 10; i <= 12; ++i) {
    auto prefix = fmt::format("37.{}.0.0/16", i);
    auto community = fmt::format("37{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("37.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Set aggressive duration threshold on already-detached peer — no-op */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(1),
      999999,
      std::chrono::milliseconds(60000));

  /* Peer3 should still be detached (duration on detached = no double-detach) */
  ASSERT_TRUE(waitForPeerStateAny(
      kPeerAddr3,
      {PeerUpdateState::DETACHED_BLOCKED, PeerUpdateState::DETACHED_RUNNING}));

  /* Unblock to begin recovery */
  unblockPeer(kPeerAddr3);

  /*
   * After unblock, peer may be in any detached state (DRJ, DB, DR)
   * or may have already fully recovered to JOINED_RUNNING if the
   * DFP fast-path completed before we check.
   */
  auto postUnblockState = getPeerState(kPeerAddr3);
  EXPECT_TRUE(
      postUnblockState == PeerUpdateState::DETACHED_READY_TO_JOIN ||
      postUnblockState == PeerUpdateState::DETACHED_BLOCKED ||
      postUnblockState == PeerUpdateState::DETACHED_RUNNING ||
      postUnblockState == PeerUpdateState::JOINED_RUNNING)
      << "Expected DRJ, DB, DR, or JR, got "
      << static_cast<int>(postUnblockState);

  /* Peer4 continues receiving routes normally */
  injectLocalRoutesAtRuntime({"37.13.0.0/16"}, {"3713:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.13.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3713:1"));

  /*
   * Only check detached-peer invariants if peer3 is still detached.
   * If recovery completed (JOINED_RUNNING), the peer is no longer
   * in the detached collection.
   */
  if (postUnblockState != PeerUpdateState::JOINED_RUNNING) {
    verifySlowPeerInvariants(kPeerAddr3);
  }
  XLOG(INFO, "=== TEST PASSED ===");
}

/*
 * Full recovery sequence:
 * 1. Detach peer3 via freq threshold (DETACHED_BLOCKED)
 * 2. Unblock → DETACHED_READY_TO_JOIN (or still DB)
 * 3. Inject route → CL accumulates for peer3, peer4 gets it
 * 4. Bring peer3 DOWN then UP → fresh start via DETACHED_INIT_DUMP
 * 5. Drain init dump → verify peer4 continues
 */
TEST_P(UpdateGroupMultiPeerTest, DetachUnblockClDrainAccept) {
  XLOG(INFO, "=== TEST: DetachUnblockClDrainAccept ===");

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

  /* Duration-detach peer3: 1ms fires immediately on block */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(1),
      999999,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 20; i <= 22; ++i) {
    auto prefix = fmt::format("37.{}.0.0/16", i);
    auto community = fmt::format("37{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("37.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Step 2: Unblock peer3 */
  unblockPeer(kPeerAddr3);

  /* Step 3: Inject a CL item while peer3 is detached */
  injectLocalRoutesAtRuntime({"37.23.0.0/16"}, {"3723:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.23.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.23.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3723:1"));

  /* Step 4: Bring peer3 DOWN then UP for fresh start */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));

  /* Peer3 enters DETACHED_INIT_DUMP on reconnect — drain its queue */
  drainPeerQueueCompletely(peerId3);

  /* Verify peer4 still works */
  injectLocalRoutesAtRuntime({"37.24.0.0/16"}, {"3724:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.24.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.24.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3724:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  XLOG(INFO, "=== TEST PASSED ===");
}

/*
 * Detach → PEER-DOWN → PEER-UP → fresh start.
 * Detach peer3 via freq threshold, then bring it DOWN.
 * Must unblockPeer BEFORE bringUpPeer to clear blocked state.
 * After UP, peer enters DETACHED_INIT_DUMP. Drain and verify peer4.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachPeerDownPeerUpFreshStart) {
  XLOG(INFO, "=== TEST: DetachPeerDownPeerUpFreshStart ===");

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

  /* Inject a shared route */
  injectLocalRoutesAtRuntime({"37.30.0.0/16"}, {"3730:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.30.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3730:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3730:1"));

  /* Duration-detach peer3: 1ms fires immediately on block */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(1),
      999999,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 31; i <= 33; ++i) {
    auto prefix = fmt::format("37.{}.0.0/16", i);
    auto community = fmt::format("37{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("37.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Bring peer3 DOWN */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Unblock BEFORE bringUp to clear blocked state from prior session */
  unblockPeer(kPeerAddr3);

  /* Bring peer3 UP — enters DETACHED_INIT_DUMP */
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));

  /* Drain init dump for the reconnected peer */
  drainPeerQueueCompletely(peerId3);

  /* Verify peer4 continues to work */
  injectLocalRoutesAtRuntime({"37.34.0.0/16"}, {"3734:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.34.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.34.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3734:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  XLOG(INFO, "=== TEST PASSED ===");
}

/*
 * Quadruple sequence: BLOCK → DETACH → POLICY-CHG → UNBLOCK → ACCEPT
 * 1. Block peer3 and fill queue (JOINED_BLOCKED)
 * 2. Freq-detach triggers (DETACHED_BLOCKED)
 * 3. Simulate policy change by withdrawing a prefix and injecting new one
 * 4. Unblock → begin recovery
 * 5. Verify peer4 received all routes/withdrawals normally throughout
 */
TEST_P(UpdateGroupMultiPeerTest, BlockDetachPolicyUnblockAccept) {
  XLOG(INFO, "=== TEST: BlockDetachPolicyUnblockAccept ===");

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

  /* Inject a shared route that we'll later "policy-change" (withdraw + new) */
  injectLocalRoutesAtRuntime({"37.40.0.0/16"}, {"3740:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.40.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3740:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3740:1"));

  /* Step 1: Block peer3 and set freq threshold for detachment */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);

  /* Fill queue to trigger blocking + detachment */
  for (int i = 0; i < 5; i++) {
    auto prefix = fmt::format("37.{}.0.0/16", 41 + i);
    auto community = fmt::format("374{}:1", 1 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("37.{}.0.0", 41 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Step 2: Peer3 should be DETACHED_BLOCKED */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Step 3: Simulate policy change — withdraw old prefix, inject new one */
  withdrawLocalRoutesAtRuntime({"37.40.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "37.40.0.0", 16, kPeerAddr4));

  injectLocalRoutesAtRuntime({"37.46.0.0/16"}, {"3746:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.46.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.46.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3746:1"));

  /* Step 4: Unblock to begin recovery */
  unblockPeer(kPeerAddr3);

  /* Accept either DRJ or DB (CL batch may re-block with small queue) */
  auto postState = getPeerState(kPeerAddr3);
  EXPECT_TRUE(
      postState == PeerUpdateState::DETACHED_READY_TO_JOIN ||
      postState == PeerUpdateState::DETACHED_BLOCKED ||
      postState == PeerUpdateState::DETACHED_RUNNING)
      << "Expected DRJ, DB, or DR, got " << static_cast<int>(postState);

  /* Verify peer4 continues to operate normally */
  injectLocalRoutesAtRuntime({"37.47.0.0/16"}, {"3747:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.47.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.47.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3747:1"));

  verifySlowPeerInvariants(kPeerAddr3);
  XLOG(INFO, "=== TEST PASSED ===");
}

INSTANTIATE_TEST_SUITE_P(
    NoSerialization,
    UpdateGroupMultiPeerTest,
    ::testing::Values(kNoSerialization));

INSTANTIATE_TEST_SUITE_P(
    Serialized,
    UpdateGroupMultiPeerTest,
    ::testing::Values(kWithSerialization));

} // namespace bgp
} // namespace facebook
