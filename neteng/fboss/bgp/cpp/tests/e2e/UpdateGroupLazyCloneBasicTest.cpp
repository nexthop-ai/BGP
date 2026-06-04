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
 * E2E tests: Lazy Clone Basic Operations
 * Tests for basic lazy clone behavior on announcement, withdrawal,
 * and announce-clone-withdraw sequences.
 *
 * Test plan:
 * https://docs.google.com/document/d/11lBp_Q_i6UYocI3meYbI3sUShZzZsu6Qlq8iSCdVXRc
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 *
 * Setup: Both peers receive a shared route. Peer3 is detached.
 * Action: Group updates the same route (new community/LP).
 * Verify: In-sync peer4 receives the update. Detached peer3's
 * invariants hold (lazy clone fires before group mutates the entry).
 * RIB version on detached peer stays at the point of divergence.
 */
TEST_P(UpdateGroupLazyCloneTest, AnnouncementWithCloneNeeded) {
  XLOG(INFO, "=== TEST: AnnouncementWithCloneNeeded ===");

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

  /* Both peers receive a shared route before detachment */
  injectLocalRoutesAtRuntime({"13.1.0.0/16"}, {"1301:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1301:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1301:1"));

  /* Detach peer3 via frequency threshold=1 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"13.2.0.0/16"}, {"1302:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1302:1"));
  injectLocalRoutesAtRuntime({"13.3.0.0/16"}, {"1303:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1303:1"));
  injectLocalRoutesAtRuntime({"13.4.0.0/16"}, {"1304:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1304:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));

  /* Record peer3's detachedRibVersion before group mutation */
  auto adjRib3 = getAdjRib(kPeerAddr3);
  ASSERT_NE(adjRib3, nullptr);
  auto detachedVersion = adjRib3->getDetachedRibVersion();
  EXPECT_GT(detachedVersion, 0)
      << "Detached peer should have non-zero detachedRibVersion";

  /*
   * Now update the shared route 13.1.0.0/16 with new attributes.
   * This triggers lazy clone: the group clones the old entry
   * to peer3 before mutating it. Peer4 sees the update.
   */
  injectLocalRoutesAtRuntime({"13.1.0.0/16"}, {"1301:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1301:99"));

  /* Verify detached peer3 invariants still hold after group mutation */
  verifySlowPeerInvariants(kPeerAddr3);
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));

  /* DetachedRibVersion should not change after group mutation */
  EXPECT_EQ(adjRib3->getDetachedRibVersion(), detachedVersion)
      << "DetachedRibVersion should stay fixed during detached period";

  /* Group version should advance past the detached peer's version */
  auto group = getUpdateGroupForPeer(kPeerAddr3);
  ASSERT_NE(group, nullptr);
  EXPECT_GE(group->getLastSeenRibVersion(), detachedVersion)
      << "Group version should be >= detached peer's version";

  XLOG(INFO, "=== TEST PASSED: AnnouncementWithCloneNeeded ===");
}

/*
 *
 * Setup: Both peers receive a shared route. Peer3 is detached.
 * Action: Group withdraws the shared route.
 * Verify: In-sync peer4 receives the withdrawal. Detached peer3's
 * view is preserved (lazy clone fires before group removes the entry).
 * The entry should be cloned to peer3's per-peer slot before removal
 * from the group.
 */
TEST_P(UpdateGroupLazyCloneTest, WithdrawalWithCloneNeeded) {
  XLOG(INFO, "=== TEST: WithdrawalWithCloneNeeded ===");

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

  /* Both peers receive shared routes before detachment */
  injectLocalRoutesAtRuntime({"13.5.0.0/16"}, {"1305:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.5.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.5.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1305:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.5.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1305:1"));

  /* Detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"13.6.0.0/16"}, {"1306:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.6.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.6.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1306:1"));
  injectLocalRoutesAtRuntime({"13.7.0.0/16"}, {"1307:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.7.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.7.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1307:1"));
  injectLocalRoutesAtRuntime({"13.8.0.0/16"}, {"1308:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.8.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.8.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1308:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /*
   * Withdraw the shared route 13.5.0.0/16.
   * Lazy clone should fire: clone old entry to peer3 before removing
   * it from the group. Peer4 receives the withdrawal.
   */
  withdrawLocalRoutesAtRuntime({"13.5.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "13.5.0.0", 16, kPeerAddr4));

  /* Verify detached peer3 invariants still hold */
  verifySlowPeerInvariants(kPeerAddr3);
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));

  /* Group should still be functional for peer4 */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: WithdrawalWithCloneNeeded ===");
}

/*
 * Announce -> Clone -> Withdraw same prefix.
 *
 * Setup: Both peers receive a shared route. Peer3 is detached.
 * Action: (1) Update the shared route (triggers clone for peer3).
 *         (2) Withdraw the same route (per-peer entry now exists
 *             from step 1, so Case 1 applies — no second clone).
 * Verify: Peer4 sees the update then the withdrawal. Peer3
 * invariants hold throughout. The per-peer entry created by the
 * first clone protects peer3's view.
 */
TEST_P(UpdateGroupLazyCloneTest, AnnounceCloneWithdrawSamePrefix) {
  XLOG(INFO, "=== TEST: AnnounceCloneWithdrawSamePrefix ===");

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
  injectLocalRoutesAtRuntime({"13.10.0.0/16"}, {"1310:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.10.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1310:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1310:1"));

  /* Detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"13.11.0.0/16"}, {"1311:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.11.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.11.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1311:1"));
  injectLocalRoutesAtRuntime({"13.12.0.0/16"}, {"1312:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.12.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.12.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1312:1"));
  injectLocalRoutesAtRuntime({"13.13.0.0/16"}, {"1313:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.13.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1313:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /*
   * Step 1: Update the shared route — triggers lazy clone (Case 4).
   * Clone creates per-peer entry for peer3.
   */
  injectLocalRoutesAtRuntime({"13.10.0.0/16"}, {"1310:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1310:99"));

  /*
   * Step 2: Withdraw the same route — per-peer entry exists (Case 1),
   * so no second clone fires. Group removes its entry; peer3's
   * per-peer entry is preserved.
   */
  withdrawLocalRoutesAtRuntime({"13.10.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "13.10.0.0", 16, kPeerAddr4));

  /* Verify peer3 invariants hold through the announce+withdraw sequence */
  verifySlowPeerInvariants(kPeerAddr3);
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  XLOG(INFO, "=== TEST PASSED: AnnounceCloneWithdrawSamePrefix ===");
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
