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
 * E2E tests: Lazy Clone Multi-Peer and Sequence Tests
 * Tests for lazy clone advanced multi-peer clone, re-clone prevention, and
 * sequence.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {
/*
 * Diverged peer goes down during clone iteration - skip gracefully.
 *
 * Setup: 2 peers JOINED_RUNNING. Both share a route. Peer3 is detached.
 * Action: Bring peer3 DOWN. Then update the shared route (clone would
 *         normally fire for peer3, but peer3 is DOWN -- skip gracefully).
 * Verify: Peer4 receives the update. No crash or assertion failure.
 * Group continues normally with just peer4.
 */
TEST_P(UpdateGroupLazyCloneTest, DivergedPeerDownDuringClone) {
  XLOGF(INFO, "=== TEST: DivergedPeerDownDuringClone ===");

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

  /* Both peers receive a shared route */
  injectLocalRoutesAtRuntime({"13.120.0.0/16"}, {"13120:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("13.120.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.120.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "13120:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.120.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "13120:1"));

  /* Detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"13.121.0.0/16"}, {"13121:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("13.121.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.121.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "13121:1"));
  injectLocalRoutesAtRuntime({"13.122.0.0/16"}, {"13122:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("13.122.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.122.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "13122:1"));
  injectLocalRoutesAtRuntime({"13.123.0.0/16"}, {"13123:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("13.123.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.123.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "13123:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Bring peer3 DOWN while it's detached */
  unblockPeer(kPeerAddr3);
  bringDownPeer(kPeerAddr3);

  /*
   * Update the shared route -- clone would fire for peer3 but it's DOWN.
   * Should skip gracefully without crash. Peer4 receives the update.
   */
  injectLocalRoutesAtRuntime({"13.120.0.0/16"}, {"13120:99"}, 250);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("13.120.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.120.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "13120:99"));

  /* Group continues normally with peer4 */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Inject another route to confirm group is fully operational */
  injectLocalRoutesAtRuntime({"13.124.0.0/16"}, {"13124:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("13.124.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.124.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "13124:1"));

  XLOGF(INFO, "=== TEST PASSED: DivergedPeerDownDuringClone ===");
}
/*
 * Clone fires, per-peer entry already exists - Case 1 prevents
 * re-clone.
 *
 * Setup: 2 peers JOINED_RUNNING. Shared route. Peer3 detached.
 * Action: (1) Update shared route -- Case 4 clone fires, creates per-peer
 * entry. (2) Update same route AGAIN -- per-peer entry exists (Case 1), no
 * clone. (3) Update same route a THIRD time -- still Case 1, no clone. Verify:
 * Peer4 receives all 3 updates. Peer3 invariants hold each time. The per-peer
 * entry from the first clone prevents subsequent re-clones.
 */
TEST_P(UpdateGroupLazyCloneTest, Case1PreventsReClone) {
  XLOGF(INFO, "=== TEST: Case1PreventsReClone ===");

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

  /* Both peers receive a shared route */
  injectLocalRoutesAtRuntime({"13.140.0.0/16"}, {"13140:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("13.140.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.140.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "13140:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.140.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "13140:1"));

  /* Detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"13.141.0.0/16"}, {"13141:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("13.141.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.141.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "13141:1"));
  injectLocalRoutesAtRuntime({"13.142.0.0/16"}, {"13142:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("13.142.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.142.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "13142:1"));
  injectLocalRoutesAtRuntime({"13.143.0.0/16"}, {"13143:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("13.143.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.143.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "13143:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Update 1: Case 4 clone fires, creates per-peer entry for peer3 */
  injectLocalRoutesAtRuntime({"13.140.0.0/16"}, {"13140:50"}, 200);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("13.140.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.140.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "13140:50"));
  verifySlowPeerInvariants(kPeerAddr3);

  /* Update 2: Case 1 -- per-peer entry already exists, no re-clone */
  injectLocalRoutesAtRuntime({"13.140.0.0/16"}, {"13140:75"}, 220);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("13.140.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.140.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "13140:75"));
  verifySlowPeerInvariants(kPeerAddr3);

  /* Update 3: Still Case 1 -- per-peer entry persists, no re-clone */
  injectLocalRoutesAtRuntime({"13.140.0.0/16"}, {"13140:99"}, 250);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("13.140.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.140.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "13140:99"));
  verifySlowPeerInvariants(kPeerAddr3);

  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  XLOGF(INFO, "=== TEST PASSED: Case1PreventsReClone ===");
}

/*
 * Withdraw, Clone, Re-announce same prefix.
 *
 * Setup: 2 peers JOINED_RUNNING. Shared route. Peer3 detached.
 * Action: (1) Withdraw the shared route -- clone fires to preserve
 *             peer3's view (Case 4), group removes entry.
 *         (2) Re-announce same prefix with new attributes -- this is
 *             a brand new entry (ribVersion > divergenceRibVersion),
 *             so Case 3 applies (no clone on re-announce).
 * Verify: Peer4 sees withdraw then re-announce. Peer3 invariants hold.
 */
TEST_P(UpdateGroupLazyCloneTest, WithdrawCloneReAnnounceSamePrefix) {
  XLOGF(INFO, "=== TEST: WithdrawCloneReAnnounceSamePrefix ===");

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

  /* Both peers receive a shared route */
  injectLocalRoutesAtRuntime({"13.150.0.0/16"}, {"13150:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("13.150.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.150.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "13150:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.150.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "13150:1"));

  /* Detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"13.151.0.0/16"}, {"13151:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("13.151.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.151.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "13151:1"));
  injectLocalRoutesAtRuntime({"13.152.0.0/16"}, {"13152:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("13.152.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.152.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "13152:1"));
  injectLocalRoutesAtRuntime({"13.153.0.0/16"}, {"13153:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("13.153.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.153.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "13153:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /*
   * Step 1: Withdraw shared route -- Case 4 clone fires to preserve
   * peer3's view. Group removes the entry.
   */
  withdrawLocalRoutesAtRuntime({"13.150.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "13.150.0.0", 16, kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);

  /*
   * Step 2: Re-announce same prefix with new community -- this creates
   * a new group entry with ribVersion > peer3's divergenceRibVersion.
   * Case 3 applies: no clone needed on re-announce.
   */
  injectLocalRoutesAtRuntime({"13.150.0.0/16"}, {"13150:99"}, 250);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("13.150.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.150.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "13150:99"));
  verifySlowPeerInvariants(kPeerAddr3);

  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  XLOGF(INFO, "=== TEST PASSED: WithdrawCloneReAnnounceSamePrefix ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupLazyCloneTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
