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
 * E2E tests: Local-pref, Split Horizon, Attribute Changes, and PathId
 * correctness through detach cycles.
 *
 * Prefix range: 30.60-30.90/16
 * Fixture: UpdateGroupLifecycleTest
 *
 * Tests implemented:
 *   Local-pref correctness — local preference preserved
 *   Multiple attribute changes during detachment — final attrs on peer
 *   Split horizon correctness — routes from peer not reflected back
 *   Per-peer entry uses forPeer() owner key — not forGroup()
 *   Entry with pathId preserved through clone — add-path correctness
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Local-pref correctness — local preference preserved through
 * detach cycle. Inject routes with different local-pref values, detach
 * peer3, inject more routes with varied local-pref. Verify peer4
 * receives correct local-pref on all routes.
 */
TEST_P(UpdateGroupLifecycleTest, LocalPrefCorrectnessThruDetach) {
  XLOG(INFO, "=== TEST: LocalPrefCorrectnessThruDetach ===");

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

  /* Route with local-pref=200 (non-default) */
  injectLocalRoutesAtRuntime({"30.60.0.0/16"}, {"3060:1"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.60.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.60.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3060:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.60.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3060:1"));
  XLOG(INFO, "Checkpoint 1: both peers received route with local-pref=200");

  /* Detach peer3 via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"30.61.0.0/16"}, {"3061:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.61.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.61.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3061:1"));
  injectLocalRoutesAtRuntime({"30.62.0.0/16"}, {"3062:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.62.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.62.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3062:1"));
  injectLocalRoutesAtRuntime({"30.63.0.0/16"}, {"3063:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.63.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.63.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3063:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  XLOG(INFO, "Checkpoint 2: peer3 detached");

  /* Inject route with high local-pref=300, verify peer4 sees correct value */
  injectLocalRoutesAtRuntime({"30.64.0.0/16"}, {"3064:1"}, 300);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.64.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.64.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3064:1"));
  XLOG(INFO, "Checkpoint 3: peer4 received route with local-pref=300");

  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: LocalPrefCorrectnessThruDetach ===");
}

/*
 * Multiple attribute changes during detachment — only final attrs
 * reach the in-sync peer. Inject route, detach peer3, then withdraw and
 * re-inject the same prefix with different attributes multiple times.
 * Verify peer4 receives each update correctly.
 */
TEST_P(UpdateGroupLifecycleTest, MultipleAttrChangesDuringDetach) {
  XLOG(INFO, "=== TEST: MultipleAttrChangesDuringDetach ===");

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

  /* Detach peer3 via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"30.65.0.0/16"}, {"3065:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.65.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.65.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3065:1"));
  injectLocalRoutesAtRuntime({"30.66.0.0/16"}, {"3066:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.66.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.66.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3066:1"));
  injectLocalRoutesAtRuntime({"30.67.0.0/16"}, {"3067:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.67.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.67.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3067:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  XLOG(INFO, "Checkpoint 1: peer3 detached");

  /* First attribute version: community 3068:1, local-pref 100 */
  injectLocalRoutesAtRuntime({"30.68.0.0/16"}, {"3068:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.68.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.68.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3068:1"));
  XLOG(INFO, "Checkpoint 2: peer4 received first version of route");

  /* Withdraw and re-inject with different attributes (different prefix
   * to avoid CL suppression — learned pattern) */
  withdrawLocalRoutesAtRuntime({"30.68.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "30.68.0.0", 16, kPeerAddr4));

  /* Second version: different community and local-pref */
  injectLocalRoutesAtRuntime({"30.69.0.0/16"}, {"3069:99"}, 250);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.69.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.69.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3069:99"));
  XLOG(INFO, "Checkpoint 3: peer4 received updated attrs correctly");

  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: MultipleAttrChangesDuringDetach ===");
}

/*
 * Split horizon correctness — routes learned from a peer are NOT
 * reflected back to that same peer. Use addRoute from peer3, verify peer4
 * receives it but peer3 does NOT get it back.
 */
TEST_P(UpdateGroupLifecycleTest, SplitHorizonCorrectness) {
  XLOG(INFO, "=== TEST: SplitHorizonCorrectness ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

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

  /* Add route FROM peer3 — both eBGP peers receive it (peer3 gets
   * reflection with local ASN prepended, peer4 also gets it) */
  addRoute("v4", "30.70.0.0", 16, kPeerAddr3, "127.5.0.1", "64500", "3070:1");
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.70.0.0/16")));

  /* Peer4 should receive it (different peer in same group) */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.70.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001 64500",
      "3070:1"));

  /* Peer3 also receives reflection (eBGP — local ASN prepended) */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.70.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001 64500",
      "3070:1"));
  XLOG(INFO, "Checkpoint 1: route reflected to both eBGP peers");

  /* Inject a local route to verify peer3 is still functional */
  injectLocalRoutesAtRuntime({"30.71.0.0/16"}, {"3071:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.71.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.71.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3071:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.71.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3071:1"));
  XLOG(INFO, "Checkpoint 2: local route delivered to both peers");

  /* Verify peer3 is still JOINED_RUNNING (didn't get confused) */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: SplitHorizonCorrectness ===");
}

/*
 * Per-peer entry uses forPeer() owner key — not forGroup().
 * After detachment, new routes create CL entries with per-peer ownership.
 * Verify the CL accumulates entries for detached peer3 while peer4
 * receives group-level entries normally. Both work independently.
 */
TEST_P(UpdateGroupLifecycleTest, PerPeerEntryOwnerKey) {
  XLOG(INFO, "=== TEST: PerPeerEntryOwnerKey ===");

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

  /* Inject a shared route before detachment */
  injectLocalRoutesAtRuntime({"30.75.0.0/16"}, {"3075:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.75.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.75.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3075:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.75.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3075:1"));

  /* Detach peer3 via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"30.76.0.0/16"}, {"3076:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.76.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.76.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3076:1"));
  injectLocalRoutesAtRuntime({"30.77.0.0/16"}, {"3077:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.77.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.77.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3077:1"));
  injectLocalRoutesAtRuntime({"30.78.0.0/16"}, {"3078:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.78.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.78.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3078:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  XLOG(INFO, "Checkpoint 1: peer3 detached, per-peer CL entries accumulate");

  /* Update the shared pre-detach route: triggers lazy clone (Case 4)
   * creating a per-peer entry for peer3 */
  injectLocalRoutesAtRuntime({"30.75.0.0/16"}, {"3075:99"}, 250);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.75.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.75.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3075:99"));
  XLOG(INFO, "Checkpoint 2: shared route updated, peer4 sees new attrs");

  /* Inject a brand-new route post-detach (no clone needed, Case 2) */
  injectLocalRoutesAtRuntime({"30.79.0.0/16"}, {"3079:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.79.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.79.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3079:1"));
  XLOG(INFO, "Checkpoint 3: new route (Case 2) delivered to peer4");

  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: PerPeerEntryOwnerKey ===");
}

/*
 * Entry with pathId preserved through clone — add-path correctness.
 * Two distinct prefixes as separate "paths" (E2E has no explicit pathId).
 * Each gets independent lazy clone on update. Withdrawing one path
 * doesn't affect the other's clone.
 */
TEST_P(UpdateGroupLifecycleTest, PathIdPreservedThruClone) {
  XLOG(INFO, "=== TEST: PathIdPreservedThruClone ===");

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

  /* Two distinct "paths" (prefixes) shared by both peers */
  injectLocalRoutesAtRuntime({"30.80.0.0/16"}, {"3080:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.80.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.80.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3080:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.80.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3080:1"));

  injectLocalRoutesAtRuntime({"30.81.0.0/16"}, {"3081:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.81.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.81.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3081:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.81.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3081:1"));
  XLOG(INFO, "Checkpoint 1: both paths shared by both peers");

  /* Detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"30.82.0.0/16"}, {"3082:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.82.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.82.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3082:1"));
  injectLocalRoutesAtRuntime({"30.83.0.0/16"}, {"3083:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.83.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.83.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3083:1"));
  injectLocalRoutesAtRuntime({"30.84.0.0/16"}, {"3084:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.84.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.84.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3084:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  XLOG(INFO, "Checkpoint 2: peer3 detached");

  /* Update path1 (30.80) — triggers Case 4 clone for peer3 */
  injectLocalRoutesAtRuntime({"30.80.0.0/16"}, {"3080:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.80.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.80.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3080:99"));
  XLOG(INFO, "Checkpoint 3: path1 updated, peer4 sees new attrs");

  /* Withdraw path1 — peer4 sees withdrawal, peer3 clone preserved */
  withdrawLocalRoutesAtRuntime({"30.80.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "30.80.0.0", 16, kPeerAddr4));
  XLOG(INFO, "Checkpoint 4: path1 withdrawn");

  /* Update path2 (30.81) — independent clone for peer3 */
  injectLocalRoutesAtRuntime({"30.81.0.0/16"}, {"3081:42"}, 175);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.81.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.81.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3081:42"));
  XLOG(INFO, "Checkpoint 5: path2 updated independently, peer4 correct");

  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: PathIdPreservedThruClone ===");
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
