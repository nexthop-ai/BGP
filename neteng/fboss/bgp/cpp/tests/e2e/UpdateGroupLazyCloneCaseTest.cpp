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
 * E2E tests: Lazy Clone Decision Algorithm - Basic Cases
 * Tests for lazy clone decision algorithm cases 1-3.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Case 1: Announcement to detached peer, no clone on repeat update.
 *
 * Setup: Peer3 detached, peer4 running. Announce route (triggers clone,
 *        creates per-peer entry for peer3).
 *         (2) Update the SAME route AGAIN (per-peer entry now exists
 *             - Case 1, NO second clone needed).
 * Verify: Peer4 receives both updates. Peer3 invariants hold.
 * DetachedRibVersion stays fixed - no clone on second update.
 */
TEST_P(UpdateGroupLazyCloneTest, Case1AnnouncementNoClone) {
  XLOGF(INFO, "=== TEST: Case1AnnouncementNoClone ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr4);
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

  /* Both peers receive a shared route before detachment */
  injectLocalRoutesAtRuntime({"13.50.0.0/16"}, {"1350:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.50.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.50.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1350:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.50.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1350:1"));

  /* Detach peer3 via frequency threshold=1 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  injectLocalRoutesAtRuntime({"13.51.0.0/16"}, {"1351:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.51.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.51.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1351:1"));
  injectLocalRoutesAtRuntime({"13.52.0.0/16"}, {"1352:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.52.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.52.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1352:1"));
  injectLocalRoutesAtRuntime({"13.53.0.0/16"}, {"1353:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.53.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.53.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1353:1"));

  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  auto adjRib3 = getAdjRib(kPeerAddr3);
  ASSERT_NE(adjRib3, nullptr);
  auto detachedVersion = adjRib3->getDetachedRibVersion();
  EXPECT_GT(detachedVersion, 0);

  /*
   * Step 1: Update shared route 13.50 — triggers Case 4 clone.
   * This creates a per-peer entry for peer3.
   */
  injectLocalRoutesAtRuntime({"13.50.0.0/16"}, {"1350:50"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.50.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.50.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1350:50"));

  verifySlowPeerInvariants(kPeerAddr3);

  /*
   * Step 2: Update the SAME route AGAIN — per-peer entry now exists
   * from step 1, so Case 1 applies. No second clone fires.
   */
  injectLocalRoutesAtRuntime({"13.50.0.0/16"}, {"1350:99"}, 250);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.50.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.50.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1350:99"));

  /* Verify peer3 invariants hold — no second clone, state unchanged */
  verifySlowPeerInvariants(kPeerAddr3);
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));
  EXPECT_EQ(adjRib3->getDetachedRibVersion(), detachedVersion)
      << "DetachedRibVersion must not change during Case 1 (no clone)";
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  XLOGF(INFO, "=== TEST PASSED: Case1AnnouncementNoClone ===");
}

/*
 * Case 1 + Withdrawal — per-peer entry exists → no clone.
 *
 * Setup: Both peers receive a shared route. Peer3 is detached.
 * Action: (1) Update the shared route (Case 4 clone fires, creates
 *             per-peer entry for peer3).
 *         (2) Withdraw the SAME route (per-peer entry exists from
 *             step 1 → Case 1, no clone needed).
 * Verify: Peer4 receives update then withdrawal. Peer3 invariants hold.
 */
TEST_P(UpdateGroupLazyCloneTest, Case1WithdrawalNoClone) {
  XLOGF(INFO, "=== TEST: Case1WithdrawalNoClone ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr4);
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

  /* Both peers receive shared route */
  injectLocalRoutesAtRuntime({"13.55.0.0/16"}, {"1355:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.55.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.55.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1355:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.55.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1355:1"));

  /* Detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  injectLocalRoutesAtRuntime({"13.56.0.0/16"}, {"1356:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.56.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.56.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1356:1"));
  injectLocalRoutesAtRuntime({"13.57.0.0/16"}, {"1357:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.57.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.57.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1357:1"));
  injectLocalRoutesAtRuntime({"13.58.0.0/16"}, {"1358:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.58.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.58.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1358:1"));

  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /*
   * Step 1: Update shared route — triggers Case 4 clone.
   * Creates per-peer entry for peer3.
   */
  injectLocalRoutesAtRuntime({"13.55.0.0/16"}, {"1355:50"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.55.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.55.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1355:50"));

  /*
   * Step 2: Withdraw the same route — per-peer entry exists (Case 1),
   * no clone fires. Group removes entry; peer3's per-peer entry preserved.
   */
  withdrawLocalRoutesAtRuntime({"13.55.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "13.55.0.0", 16, kPeerAddr4));

  verifySlowPeerInvariants(kPeerAddr3);
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  XLOGF(INFO, "=== TEST PASSED: Case1WithdrawalNoClone ===");
}

/*
 * Case 2 + Announcement — new prefix post-detach, ribVersion==0.
 *
 * Setup: Both peers in group. Peer3 is detached.
 * Action: Inject a brand-new prefix that never existed before detachment.
 *         Since ribVersion==0 for a new prefix, no clone is needed (Case 2).
 * Verify: Peer4 receives the route. Peer3 invariants hold.
 * The new prefix was never shared with peer3, so no clone fires.
 */
TEST_P(UpdateGroupLazyCloneTest, Case2AnnouncementNoClone) {
  XLOGF(INFO, "=== TEST: Case2AnnouncementNoClone ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr4);
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

  /* Detach peer3 — no pre-shared routes needed for this test */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  injectLocalRoutesAtRuntime({"13.60.0.0/16"}, {"1360:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.60.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.60.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1360:1"));
  injectLocalRoutesAtRuntime({"13.61.0.0/16"}, {"1361:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.61.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.61.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1361:1"));
  injectLocalRoutesAtRuntime({"13.62.0.0/16"}, {"1362:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.62.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.62.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1362:1"));

  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  auto adjRib3 = getAdjRib(kPeerAddr3);
  ASSERT_NE(adjRib3, nullptr);
  auto detachedVersion = adjRib3->getDetachedRibVersion();
  EXPECT_GT(detachedVersion, 0);

  /*
   * Inject a brand-new prefix that never existed before detachment.
   * ribVersion==0 for this new prefix → Case 2, no clone needed.
   */
  injectLocalRoutesAtRuntime({"13.63.0.0/16"}, {"1363:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.63.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.63.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1363:1"));

  /* Verify peer3 invariants — no clone fired */
  verifySlowPeerInvariants(kPeerAddr3);
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));
  EXPECT_EQ(adjRib3->getDetachedRibVersion(), detachedVersion)
      << "DetachedRibVersion must not change for Case 2 (new prefix, no clone)";
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  XLOGF(INFO, "=== TEST PASSED: Case2AnnouncementNoClone ===");
}

/*
 * Case 2 + Withdrawal — ribVersion==0 → no clone (never existed).
 *
 * Setup: Both peers in group. Peer3 is detached.
 * Action: (1) Inject a new prefix post-detach (Case 2, no clone).
 *         (2) Withdraw that same prefix (it was never shared with
 *             peer3 → no clone needed, ribVersion==0 at detach time).
 * Verify: Peer4 receives add then withdrawal. Peer3 invariants hold.
 */
TEST_P(UpdateGroupLazyCloneTest, Case2WithdrawalNoClone) {
  XLOGF(INFO, "=== TEST: Case2WithdrawalNoClone ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr4);
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

  /* Detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  injectLocalRoutesAtRuntime({"13.65.0.0/16"}, {"1365:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.65.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.65.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1365:1"));
  injectLocalRoutesAtRuntime({"13.66.0.0/16"}, {"1366:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.66.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.66.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1366:1"));
  injectLocalRoutesAtRuntime({"13.67.0.0/16"}, {"1367:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.67.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.67.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1367:1"));

  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /*
   * Step 1: Inject new prefix post-detach (Case 2, no clone).
   * This prefix never existed before detachment, so ribVersion==0.
   */
  injectLocalRoutesAtRuntime({"13.68.0.0/16"}, {"1368:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.68.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.68.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1368:1"));

  /*
   * Step 2: Withdraw the post-detach prefix — it was never shared with
   * peer3 (ribVersion==0 at detach time), so no clone fires (Case 2).
   */
  withdrawLocalRoutesAtRuntime({"13.68.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "13.68.0.0", 16, kPeerAddr4));

  verifySlowPeerInvariants(kPeerAddr3);
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  XLOGF(INFO, "=== TEST PASSED: Case2WithdrawalNoClone ===");
}

/*
 * Case 3 + Announcement — ribVersion > divergenceRibVersion → no clone.
 *
 * Setup: Both peers in group. Peer3 is detached.
 * Action: (1) Inject a new prefix post-detach (Case 2, ribVersion==0).
 *         (2) Update that post-detach prefix. Its ribVersion is now >
 *             divergenceRibVersion → Case 3, no clone needed since
 *             the entry was created after the peer diverged.
 * Verify: Peer4 receives both the add and the update. Peer3 invariants hold.
 */
TEST_P(UpdateGroupLazyCloneTest, Case3AnnouncementNoClone) {
  XLOGF(INFO, "=== TEST: Case3AnnouncementNoClone ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr4);
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

  /* Detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  injectLocalRoutesAtRuntime({"13.70.0.0/16"}, {"1370:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.70.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.70.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1370:1"));
  injectLocalRoutesAtRuntime({"13.71.0.0/16"}, {"1371:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.71.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.71.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1371:1"));
  injectLocalRoutesAtRuntime({"13.72.0.0/16"}, {"1372:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.72.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.72.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1372:1"));

  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  auto adjRib3 = getAdjRib(kPeerAddr3);
  ASSERT_NE(adjRib3, nullptr);
  auto detachedVersion = adjRib3->getDetachedRibVersion();
  EXPECT_GT(detachedVersion, 0);

  /*
   * Step 1: Inject a new prefix post-detach (Case 2, no clone).
   * This prefix gets a ribVersion > divergenceRibVersion.
   */
  injectLocalRoutesAtRuntime({"13.73.0.0/16"}, {"1373:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.73.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.73.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1373:1"));

  /*
   * Step 2: Update the post-detach prefix with new attributes.
   * Its ribVersion is already > divergenceRibVersion → Case 3.
   * No clone fires because the entry was created after divergence.
   */
  injectLocalRoutesAtRuntime({"13.73.0.0/16"}, {"1373:99"}, 250);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.73.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.73.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1373:99"));

  /* Verify peer3 invariants — no clone fired (Case 3) */
  verifySlowPeerInvariants(kPeerAddr3);
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));
  EXPECT_EQ(adjRib3->getDetachedRibVersion(), detachedVersion)
      << "DetachedRibVersion must not change for Case 3 (no clone)";
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  XLOGF(INFO, "=== TEST PASSED: Case3AnnouncementNoClone ===");
}

/*
 * Case 3 + Withdrawal — ribVersion > divergenceRibVersion → no clone.
 *
 * Setup: Both peers in group. Peer3 is detached.
 * Action: (1) Inject a new prefix post-detach (Case 2, ribVersion==0).
 *         (2) Withdraw that post-detach prefix. Its ribVersion is >
 *             divergenceRibVersion → Case 3, no clone needed.
 * Verify: Peer4 receives add then withdrawal. Peer3 invariants hold.
 * DetachedRibVersion stays fixed — no clone on withdrawal.
 */
TEST_P(UpdateGroupLazyCloneTest, Case3WithdrawalNoClone) {
  XLOGF(INFO, "=== TEST: Case3WithdrawalNoClone ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr4);
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

  /* Detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  injectLocalRoutesAtRuntime({"13.75.0.0/16"}, {"1375:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.75.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.75.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1375:1"));
  injectLocalRoutesAtRuntime({"13.76.0.0/16"}, {"1376:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.76.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.76.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1376:1"));
  injectLocalRoutesAtRuntime({"13.77.0.0/16"}, {"1377:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.77.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.77.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1377:1"));

  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  auto adjRib3 = getAdjRib(kPeerAddr3);
  ASSERT_NE(adjRib3, nullptr);
  auto detachedVersion = adjRib3->getDetachedRibVersion();
  EXPECT_GT(detachedVersion, 0);

  /*
   * Step 1: Inject a new prefix post-detach (Case 2, no clone).
   * This prefix gets a ribVersion > divergenceRibVersion.
   */
  injectLocalRoutesAtRuntime({"13.78.0.0/16"}, {"1378:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.78.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.78.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1378:1"));

  /*
   * Step 2: Withdraw the post-detach prefix. Its ribVersion is >
   * divergenceRibVersion → Case 3, no clone fires.
   */
  withdrawLocalRoutesAtRuntime({"13.78.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "13.78.0.0", 16, kPeerAddr4));

  /* Verify peer3 invariants — no clone fired (Case 3) */
  verifySlowPeerInvariants(kPeerAddr3);
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));
  EXPECT_EQ(adjRib3->getDetachedRibVersion(), detachedVersion)
      << "DetachedRibVersion must not change for Case 3 withdrawal (no clone)";
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  XLOGF(INFO, "=== TEST PASSED: Case3WithdrawalNoClone ===");
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
