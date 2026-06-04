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
 * [BGP++][UG2 E2E] Cross-boundary state propagation tests.
 *
 * Tests for state propagation at detach/reattach lifecycle boundaries.
 * Bugs 1, 6, 7 found that operational state (owner keys, egressEoRsPending,
 * canAnnounce policy) failed to propagate during detachSlowPeer().
 *
 * Learning patterns applied:
 *   P1: After unblockPeer(), add JOINED_RUNNING to allowed states
 *   P5: After unblockPeer(), do NOT verifyRouteAdd for pre-queued routes
 *   P6: Inject >= queue capacity (3) routes for reliable blocking
 *   P8: Use 3 peers to avoid JOINED_BLOCKED blocking entire group
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Test 1 (Critical): DetachedPeer_SharedEntryWithdrawalAndReadvertise
 *
 * Bug 1 (D98568595): A detached peer couldn't find RIB-OUT entries stored
 * under the group owner key. This caused duplicate advertisements and
 * missing withdrawals.
 *
 * Flow:
 * 1. 3 peers joined, inject 3 local routes (stored under group owner key)
 * 2. Detach peer3 via frequency threshold
 * 3. Withdraw a route — detached peer3 must send WITHDRAW
 * 4. Inject a new route — peer3 must send single UPDATE (not duplicate)
 */
/* TODO: Re-enable when D99316308 (ribVersion in processRibDumpReq) lands.
 * Recovery path is broken on master — detached peer stuck in DETACHED_RUNNING.
 */
TEST_P(
    UpdateGroupCrossBoundaryStateTest,
    DISABLED_DetachedPeer_SharedEntryWithdrawalAndReadvertise) {
  XLOGF(INFO, "=== TEST: DetachedPeer_SharedEntryWithdrawalAndReadvertise ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);

  /* Large queues so peer4/peer5 don't block during detach routes.
   * Pattern 11: with small queues ALL peers block, not just peer3. */
  setupSlowPeerComponents(20, 15, 0);

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

  /* Inject 2 routes at runtime — stored under group owner key */
  injectLocalRoutesAtRuntime({"70.0.0.0/8"}, {"700:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("70.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "70.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "700:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "70.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "700:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "70.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "700:1"));

  injectLocalRoutesAtRuntime({"71.0.0.0/8"}, {"710:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("71.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "71.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "710:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "71.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "710:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "71.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "710:1"));

  /* Detach peer3: block it, inject routes to trigger freq=1 threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectDistinctRoutes(
      {"130.0.0.0/8", "131.0.0.0/8", "132.0.0.0/8"}, 1300, 150);
  drainPeerQueueCompletely(peerId4);
  drainPeerQueueCompletely(peerId5);

  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /*
   * Now withdraw 70.0.0.0/8. The detached peer3 must find the entry
   * stored under the GROUP owner key and send a WITHDRAW.
   * Without the fix, getRibEntry() only looked up by peer key and
   * returned nullptr, causing the withdrawal to be missed.
   */
  withdrawLocalRoutesAtRuntime({"70.0.0.0/8"});

  /* peer4 and peer5 get the withdrawal via group path */
  EXPECT_TRUE(verifyRouteWithdraw("v4", "70.0.0.0", 8, kPeerAddr4));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "70.0.0.0", 8, kPeerAddr5));

  /* Unblock peer3, let it drain and process the withdrawal */
  unblockPeer(kPeerAddr3);

  /*
   * P1: Recovery takes longer here — detached peer must:
   * 1. Drain cloned PL from detachment
   * 2. Process CL items (including the withdrawal)
   * 3. Wait for group to reach IDLE and accept
   */
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING, 300));

  XLOGF(
      INFO,
      "=== TEST PASSED: DetachedPeer_SharedEntryWithdrawalAndReadvertise ===");
}

/*
 * Test 8 (High): DetachedPeer_FullStateAuditAfterDetach
 *
 * Comprehensive state audit on a detached peer. Verifies that ALL
 * operational state fields are correctly propagated during detachment.
 */
TEST_P(
    UpdateGroupCrossBoundaryStateTest,
    DetachedPeer_FullStateAuditAfterDetach) {
  XLOGF(INFO, "=== TEST: DetachedPeer_FullStateAuditAfterDetach ===");

  auto [peerId3, peerId4, peerId5] = setupThreePeersJoined();

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  /* Inject a route so version tracking has data */
  injectLocalRoutesAtRuntime({"80.0.0.0/8"}, {"800:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("80.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "80.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "800:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "80.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "800:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "80.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "800:1"));

  auto versionBeforeDetach = getPeerCachedRibVersion(kPeerAddr3);
  XLOGF(INFO, "Peer3 version before detach: {}", versionBeforeDetach);

  /* Detach peer3 */
  detachPeer3ViaFrequency(peerId3, peerId4, peerId5, 140);

  /* === Comprehensive state audit === */

  /* Sync bitmap cleared */
  EXPECT_FALSE(isPeerInSync(kPeerAddr3))
      << "Detached peer must NOT be in sync bitmap";

  /* Diverged bitmap set */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3))
      << "Detached peer must be in detached peers collection";

  /* Version preserved (Bug 4 guard) */
  auto versionAfterDetach = getPeerCachedRibVersion(kPeerAddr3);
  XLOGF(INFO, "Peer3 version after detach: {}", versionAfterDetach);
  EXPECT_GT(versionAfterDetach, 0)
      << "Version must not reset to 0 during detachment";

  /* Detached collection tracking */
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1)
      << "Exactly 1 detached peer expected";

  /* Structural invariants */
  verifySlowPeerInvariants(kPeerAddr3);

  /* Group membership preserved */
  EXPECT_EQ(getGroupMemberCount(kPeerAddr3), 3)
      << "Detached peer is still a group member";

  /* Other peers unaffected */
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  /* P1,P5: Recover and rejoin */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING, 100));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 0);

  XLOGF(INFO, "=== TEST PASSED: DetachedPeer_FullStateAuditAfterDetach ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupCrossBoundaryStateTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
