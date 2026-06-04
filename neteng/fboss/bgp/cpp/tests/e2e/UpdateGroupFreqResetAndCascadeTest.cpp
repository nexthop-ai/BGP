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

/* E2E tests: Frequency counter reset, independent tracking,
 * selective detachment, and cascading detachment scenarios.
 * Prefix range: 32.x.0.0/16.
 *
 * Frequency counter reset on peer acceptance (rejoin)
 * Two peers hitting frequency threshold at different times
 * 3 peers in group, only the one exceeding threshold detaches
 *
 * Fix: per-peer queue sizes (fast peers 10,8,0 / slow peers 3,2,0)
 * and threshold-raise pattern (thresholds are per-group, not per-peer).
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Frequency counter reset on peer acceptance (rejoin).
 * Freq-detach peer3 via 2 block cycles, bring it DOWN then UP (rejoin).
 * After rejoin, verify the detachment state is reset and peer4 continues
 * to function normally. Uses per-peer queue sizes: peer4 (10,8,0) fast,
 * peer3 (3,2,0) slow.
 */
TEST_P(UpdateGroupMultiPeerTest, FreqCounterResetOnRejoin) {
  XLOG(INFO, "=== TEST: FreqCounterResetOnRejoin ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* Large queue for fast peer4, small queue for slow peer3 */
  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr4);
  setDefaultQueueSizes(3, 2, 0);
  bringUpPeer(kPeerAddr3);

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Freq threshold=2 for the group */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      2,
      std::chrono::milliseconds(60000));

  /* Block cycle #1: inject 3 routes, drain peer4 inline.
   * Peer3 queue fills naturally to 3 > hwm=2 → blocked. */
  injectLocalRoutesAtRuntime({"32.1.0.0/16"}, {"3201:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3201:1"));

  injectLocalRoutesAtRuntime({"32.2.0.0/16"}, {"3202:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3202:1"));

  injectLocalRoutesAtRuntime({"32.3.0.0/16"}, {"3203:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3203:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Drain peer3 to complete cycle #1 */
  drainPeerQueueCompletely(peerId3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Block cycle #2: triggers freq detachment (count=2 >= threshold=2) */
  injectLocalRoutesAtRuntime({"32.4.0.0/16"}, {"3204:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3204:1"));

  injectLocalRoutesAtRuntime({"32.5.0.0/16"}, {"3205:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.5.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.5.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3205:1"));

  injectLocalRoutesAtRuntime({"32.6.0.0/16"}, {"3206:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.6.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.6.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3206:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Bring peer3 DOWN then UP (rejoin). Freq counters should reset. */
  bringDownPeer(kPeerAddr3);
  setDefaultQueueSizes(3, 2, 0);
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);

  /* Drain init dump queue for peer3 */
  drainPeerQueueCompletely(peerId3);

  /* Peer4 should still be working */
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* After rejoin, peer3 should not be in old DETACHED_BLOCKED state.
   * The detachment state and freq counter are reset by the down/up cycle. */
  auto state3 = getPeerState(kPeerAddr3);
  EXPECT_NE(state3, PeerUpdateState::DETACHED_BLOCKED);
  EXPECT_NE(state3, PeerUpdateState::DOWN);

  /* Verify peer4 still receives routes after rejoin */
  injectLocalRoutesAtRuntime({"32.7.0.0/16"}, {"3207:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.7.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.7.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3207:1"));

  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: FreqCounterResetOnRejoin ===");
}

/* Two peers hitting frequency threshold at DIFFERENT times.
 * Uses threshold-raise pattern: set threshold=1, detach peer3, then
 * raise threshold to 3 for peer4's 3 block-drain cycles. Peer5 is
 * the in-sync verifier with a large queue (10,8,0).
 * Thresholds are per-group, so threshold-raise is needed for
 * independent detachment behavior.
 */
TEST_P(UpdateGroupMultiPeerTest, TwoPeersIndependentFreqTracking) {
  XLOG(INFO, "=== TEST: TwoPeersIndependentFreqTracking ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  /* Large queue for fast peer5 */
  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr5);

  /* Small queues for slow peers 3 and 4 */
  setDefaultQueueSizes(3, 2, 0);
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

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

  /* Phase 1: threshold=1, detach peer3. Drain peer4+5 inline so
   * only peer3's queue fills. */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  injectLocalRoutesAtRuntime({"32.10.0.0/16"}, {"3210:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3210:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.10.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3210:1"));

  injectLocalRoutesAtRuntime({"32.11.0.0/16"}, {"3211:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.11.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.11.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3211:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.11.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3211:1"));

  injectLocalRoutesAtRuntime({"32.12.0.0/16"}, {"3212:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.12.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.12.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3212:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.12.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3212:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Peer4 should NOT be detached (threshold=1, but it was drained inline
   * so never blocked). */
  EXPECT_FALSE(isPeerDetached(kPeerAddr4));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Phase 2: raise group threshold to 3 for peer4's block cycles.
   * Peer3 is detached, routes only go to peer4 and peer5. */
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      3,
      std::chrono::milliseconds(60000));

  /* Block cycle #1 for peer4: inject 3 routes, drain peer5 inline */
  injectLocalRoutesAtRuntime({"32.13.0.0/16"}, {"3213:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.13.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3213:1"));

  injectLocalRoutesAtRuntime({"32.14.0.0/16"}, {"3214:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.14.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.14.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3214:1"));

  injectLocalRoutesAtRuntime({"32.15.0.0/16"}, {"3215:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.15.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.15.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3215:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  drainPeerQueueCompletely(peerId4);
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Block cycle #2 for peer4 */
  injectLocalRoutesAtRuntime({"32.16.0.0/16"}, {"3216:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.16.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.16.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3216:1"));

  injectLocalRoutesAtRuntime({"32.17.0.0/16"}, {"3217:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.17.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.17.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3217:1"));

  injectLocalRoutesAtRuntime({"32.18.0.0/16"}, {"3218:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.18.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.18.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3218:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  drainPeerQueueCompletely(peerId4);
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Block cycle #3 for peer4: triggers detachment (count=3 >= threshold=3) */
  injectLocalRoutesAtRuntime({"32.19.0.0/16"}, {"3219:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.19.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.19.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3219:1"));

  injectLocalRoutesAtRuntime({"32.20.0.0/16"}, {"3220:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.20.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3220:1"));

  injectLocalRoutesAtRuntime({"32.21.0.0/16"}, {"3221:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.21.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3221:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr4));

  /* Both peer3 and peer4 are detached. Peer5 remains in-sync. */
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr5), 2);
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  /* Verify peer5 still receives routes */
  injectLocalRoutesAtRuntime({"32.22.0.0/16"}, {"3222:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.22.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.22.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3222:1"));

  verifySlowPeerInvariants(kPeerAddr5);

  XLOG(INFO, "=== TEST PASSED: TwoPeersIndependentFreqTracking ===");
}

/* 3 peers in group, all block, but only peer3 exceeds freq threshold.
 * Uses threshold-raise pattern: set threshold=1, detach peer3 by filling
 * only its queue (drain peer4/5 inline). Then raise threshold to 999999,
 * inject routes without draining peer4/5 so they reach JOINED_BLOCKED
 * without detaching.
 */
TEST_P(UpdateGroupMultiPeerTest, OnlyThresholdExceedingPeerDetaches) {
  XLOG(INFO, "=== TEST: OnlyThresholdExceedingPeerDetaches ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  /* All peers get small queue (3,2,0) — we drain peer4/5 inline in phase 1
   * to prevent them from blocking, then let them fill in phase 2. */
  setDefaultQueueSizes(3, 2, 0);
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

  /* Phase 1: threshold=1, detach only peer3.
   * Drain peer4 and peer5 inline after each injection so their queues
   * never reach hwm. Only peer3's queue fills. */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  injectLocalRoutesAtRuntime({"32.50.0.0/16"}, {"3250:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.50.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.50.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3250:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.50.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3250:1"));

  injectLocalRoutesAtRuntime({"32.51.0.0/16"}, {"3251:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.51.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.51.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3251:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.51.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3251:1"));

  injectLocalRoutesAtRuntime({"32.52.0.0/16"}, {"3252:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.52.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.52.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3252:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.52.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3252:1"));

  /* Peer3 should be DETACHED_BLOCKED (threshold=1 exceeded) */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Peer4 and Peer5 should NOT be blocked (drained inline) */
  EXPECT_FALSE(isPeerDetached(kPeerAddr4));
  EXPECT_FALSE(isPeerDetached(kPeerAddr5));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  /* Phase 2: raise threshold to 999999 so peer4/5 block without detaching.
   * Inject 3 routes without draining — both queues fill. */
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  injectLocalRoutesAtRuntime({"32.53.0.0/16"}, {"3253:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.53.0.0/16")));
  injectLocalRoutesAtRuntime({"32.54.0.0/16"}, {"3254:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.54.0.0/16")));
  injectLocalRoutesAtRuntime({"32.55.0.0/16"}, {"3255:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.55.0.0/16")));

  /* Peer4 and Peer5 should be JOINED_BLOCKED (threshold=999999, not exceeded)
   */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId5));
  EXPECT_FALSE(isPeerDetached(kPeerAddr4));
  EXPECT_FALSE(isPeerDetached(kPeerAddr5));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  /* Drain peer4 and peer5 — they should recover */
  drainPeerQueueCompletely(peerId4);
  drainPeerQueueCompletely(peerId5);
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  /* Verify remaining peers receive routes */
  injectLocalRoutesAtRuntime({"32.56.0.0/16"}, {"3256:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.56.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.56.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3256:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.56.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3256:1"));

  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: OnlyThresholdExceedingPeerDetaches ===");
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
