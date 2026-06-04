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

/* E2E tests: Duration timer and frequency detection boundary conditions.
 * Prefix range: 31.x.0.0/16.
 *
 * Duration timer fires during policy re-evaluation -- verify ordering
 * Duration timer fires during group state transition (READY→WAITING)
 * Duration threshold set to very large value -- never fires
 * Exactly N blocks in exactly W seconds (boundary) -- detach triggers
 * N blocks but window expired between N-1th and Nth -- no detach
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Duration timer fires during policy re-evaluation.
 * Simulate policy re-evaluation via withdraw+re-inject with different prefix
 * while peer3 has a 1ms duration threshold and is blocked. The duration timer
 * fires while CL items (policy re-evaluation artifacts) are pending. Verify
 * detachment occurs cleanly and peer4 receives the CL items after unblock.
 */
TEST_P(UpdateGroupMultiPeerTest, DurationDuringPolicyReEval) {
  XLOGF(INFO, "=== TEST: DurationDuringPolicyReEval ===");

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

  /* Inject a shared route that we will later withdraw to simulate policy chg */
  injectLocalRoutesAtRuntime({"31.1.0.0/16"}, {"3101:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3101:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3101:1"));

  /* Set 1ms duration threshold -- fires almost immediately on block */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(1),
      999999,
      std::chrono::milliseconds(60000));

  /* Block peer3 and fill queue */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"31.2.0.0/16"}, {"3102:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3102:1"));
  injectLocalRoutesAtRuntime({"31.3.0.0/16"}, {"3103:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3103:1"));
  injectLocalRoutesAtRuntime({"31.4.0.0/16"}, {"3104:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3104:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Simulate policy re-evaluation: withdraw old prefix, inject new one.
   * These go to CL while peer3 is blocked and duration timer is firing. */
  withdrawLocalRoutesAtRuntime({"31.1.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "31.1.0.0", 16, kPeerAddr4));
  injectLocalRoutesAtRuntime({"31.5.0.0/16"}, {"3105:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.5.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.5.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3105:1"));

  /* Peer3 should be detached by duration timer (1ms fires immediately) */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Peer4 still in sync and receiving */
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOGF(INFO, "=== TEST PASSED: DurationDuringPolicyReEval ===");
}

/* Duration timer fires during group state transition READY→WAITING.
 * Set a 1ms duration threshold, block peer3, fill queue to trigger WAITING,
 * then inject another route (CL item) during the READY→WAITING transition.
 * The duration timer fires during or after the transition. Verify consistent
 * state: peer3 detaches, peer4 continues receiving.
 */
TEST_P(UpdateGroupMultiPeerTest, DurationDuringReadyToWaiting) {
  XLOGF(INFO, "=== TEST: DurationDuringReadyToWaiting ===");

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

  /* Set 1ms duration threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(1),
      999999,
      std::chrono::milliseconds(60000));

  /* Block peer3 and rapidly fill queue -- triggers READY→WAITING transition */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"31.10.0.0/16"}, {"3110:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3110:1"));
  injectLocalRoutesAtRuntime({"31.11.0.0/16"}, {"3111:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.11.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.11.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3111:1"));
  injectLocalRoutesAtRuntime({"31.12.0.0/16"}, {"3112:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.12.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.12.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3112:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Inject CL item during WAITING -- duration timer may fire concurrently */
  injectLocalRoutesAtRuntime({"31.13.0.0/16"}, {"3113:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.13.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3113:1"));

  /* Peer3 should be DETACHED_BLOCKED from 1ms duration timer */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Peer4 continues normally */
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  injectLocalRoutesAtRuntime({"31.14.0.0/16"}, {"3114:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.14.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.14.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3114:1"));

  verifySlowPeerInvariants(kPeerAddr4);

  XLOGF(INFO, "=== TEST PASSED: DurationDuringReadyToWaiting ===");
}

/* Duration threshold set to very large value (600s) -- never fires.
 * Block peer3 for a short time and unblock. The 600s timer should NOT fire
 * during the test window. Peer3 stays JOINED_BLOCKED then recovers normally.
 */
TEST_P(UpdateGroupMultiPeerTest, LargeDurationNeverFires) {
  XLOGF(INFO, "=== TEST: LargeDurationNeverFires ===");

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

  /* Set very large (600s) duration threshold -- should never fire in test */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Block peer3 and fill queue */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"31.20.0.0/16"}, {"3120:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3120:1"));
  injectLocalRoutesAtRuntime({"31.21.0.0/16"}, {"3121:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3121:1"));
  injectLocalRoutesAtRuntime({"31.22.0.0/16"}, {"3122:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.22.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.22.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3122:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Peer3 is JOINED_BLOCKED -- NOT detached (600s timer hasn't fired) */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));

  /* Unblock peer3 -- should recover normally.
   * unblockPeer() drains all pending messages, so do NOT verifyRouteAdd
   * for routes injected before unblock (they were already consumed). */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Verify both peers receive a new route normally */
  injectLocalRoutesAtRuntime({"31.23.0.0/16"}, {"3123:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.23.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.23.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3123:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.23.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3123:1"));

  verifySlowPeerInvariants(kPeerAddr3);

  XLOGF(INFO, "=== TEST PASSED: LargeDurationNeverFires ===");
}
/* N blocks but window expired between N-1th and Nth -- no detach.
 * Set freq threshold=2 with very short window (1ms). Block cycle #1 completes
 * and the timestamp is recorded. By the time block cycle #2 happens, the 1ms
 * window has expired, so the #1 block is evicted from the counter. The 2nd
 * block counts as the FIRST in a new window, not the Nth. No detach.
 */
TEST_P(UpdateGroupMultiPeerTest, FreqWindowExpired_NoDetach) {
  XLOGF(INFO, "=== TEST: FreqWindowExpired_NoDetach ===");

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

  /* Freq threshold=2 with VERY short window (1ms).
   * By the time cycle #2 happens, cycle #1 is outside the window. */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      2,
      std::chrono::milliseconds(1));

  /* Block cycle #1 */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"31.40.0.0/16"}, {"3140:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3140:1"));
  injectLocalRoutesAtRuntime({"31.41.0.0/16"}, {"3141:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3141:1"));
  injectLocalRoutesAtRuntime({"31.42.0.0/16"}, {"3142:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.42.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.42.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3142:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* Unblock cycle #1 -- peer recovers.
   * unblockPeer() drains all pending messages, so do NOT verifyRouteAdd
   * for routes injected before unblock (they were already consumed). */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* By now, the 1ms window has EXPIRED. The #1 block timestamp is stale.
   * Block cycle #2 -- this should NOT trigger detachment because the window
   * only sees 1 block (the current one). */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"31.43.0.0/16"}, {"3143:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.43.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.43.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3143:1"));
  injectLocalRoutesAtRuntime({"31.44.0.0/16"}, {"3144:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.44.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.44.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3144:1"));
  injectLocalRoutesAtRuntime({"31.45.0.0/16"}, {"3145:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.45.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.45.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3145:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Peer3 should be JOINED_BLOCKED, NOT DETACHED -- window expired */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));

  /* Unblock and verify recovery.
   * unblockPeer() drains all pending messages, so do NOT verifyRouteAdd
   * for routes injected before unblock (they were already consumed). */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Final route -- both peers receive it */
  injectLocalRoutesAtRuntime({"31.46.0.0/16"}, {"3146:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.46.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.46.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3146:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.46.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3146:1"));

  verifySlowPeerInvariants(kPeerAddr3);

  XLOGF(INFO, "=== TEST PASSED: FreqWindowExpired_NoDetach ===");
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
