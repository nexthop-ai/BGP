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

/* E2E tests: G-READY and G-WAITING state events — PL-DRAIN, POLICY-CHG,
 * EOR, ROUTE-WD, SLOW-FREQ.
 * Prefix range: 30.x.0.0/16.
 *
 * G-READY × E-PL-DRAIN (N/A — no PL in READY)
 * G-READY × E-POLICY-CHG (policy change during READY)
 * G-READY × E-EOR (N/A)
 * G-WAITING × E-ROUTE-WD (withdrawal during PL drain — CL item)
 * G-WAITING × E-SLOW-FREQ (frequency detach during PL drain)
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* G-READY × E-PL-DRAIN — N/A (no PL in READY state).
 * In READY, the group has CL items but no active PL. PL drain event
 * is irrelevant. Verify group stays stable and delivers routes normally. */
TEST_P(UpdateGroupMultiPeerTest, GReady_PlDrainNoop) {
  XLOG(INFO, "=== TEST: GReady_PlDrainNoop ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

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

  /* Inject a route to move group through READY cycle */
  injectLocalRoutesAtRuntime({"30.1.0.0/16"}, {"3001:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3001:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3001:1"));

  /* Group should be back to IDLE after PL drain completes. Both peers stable */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Inject another route to confirm continued operation */
  injectLocalRoutesAtRuntime({"30.2.0.0/16"}, {"3002:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.2.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3002:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3002:1"));

  verifySlowPeerInvariants(kPeerAddr3);

  XLOG(INFO, "=== TEST PASSED: GReady_PlDrainNoop ===");
}

/* G-READY × E-POLICY-CHG — Policy change during READY.
 * Simulated via withdraw+re-inject with DIFFERENT prefix (avoid CL
 * suppression). Withdraw the old prefix, drain the withdrawal, then inject a
 * new prefix with different attributes to exercise the re-evaluation path. */
TEST_P(UpdateGroupMultiPeerTest, GReady_PolicyChange) {
  XLOG(INFO, "=== TEST: GReady_PolicyChange ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

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

  /* Inject initial route */
  injectLocalRoutesAtRuntime({"30.10.0.0/16"}, {"3010:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.10.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3010:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3010:1"));

  /* Simulate policy change: withdraw old prefix, drain, then inject new one */
  withdrawLocalRoutesAtRuntime({"30.10.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "30.10.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "30.10.0.0", 16, kPeerAddr4));

  /* Inject new prefix with different attributes (simulating re-evaluation) */
  injectLocalRoutesAtRuntime({"30.11.0.0/16"}, {"3011:1"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.11.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.11.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3011:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.11.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3011:1"));

  /* Both peers stable after policy change simulation */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOG(INFO, "=== TEST PASSED: GReady_PolicyChange ===");
}

/* G-READY × E-EOR — N/A. Extra EoR on a running peer is harmless.
 * Send an extra EoR to both peers after they're JOINED_RUNNING and have
 * processed routes. Verify no state change and continued route delivery. */
TEST_P(UpdateGroupMultiPeerTest, GReady_EorNoop) {
  XLOG(INFO, "=== TEST: GReady_EorNoop ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

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

  /* Inject a route to move group through READY */
  injectLocalRoutesAtRuntime({"30.20.0.0/16"}, {"3020:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.20.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3020:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3020:1"));

  /* Send extra EoR — should be harmless no-op */
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Verify peers still JOINED_RUNNING — EoR didn't disrupt anything */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Confirm continued operation */
  injectLocalRoutesAtRuntime({"30.21.0.0/16"}, {"3021:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.21.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3021:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3021:1"));

  verifySlowPeerInvariants(kPeerAddr3);

  XLOG(INFO, "=== TEST PASSED: GReady_EorNoop ===");
}

/* G-WAITING × E-ROUTE-WD — Withdrawal during PL drain.
 * Block peer3, fill queue to reach WAITING. While WAITING (PL draining),
 * withdraw a route — it becomes a CL item. After unblock, the CL-origin
 * withdrawal is delivered to both peers. */
TEST_P(UpdateGroupMultiPeerTest, GWaiting_RouteWithdrawal) {
  XLOG(INFO, "=== TEST: GWaiting_RouteWithdrawal ===");

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

  /* Inject a shared route that we'll withdraw later */
  injectLocalRoutesAtRuntime({"30.30.0.0/16"}, {"3030:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.30.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3030:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3030:1"));

  /* Block peer3 and fill queue with 3 routes → JOINED_BLOCKED */
  blockPeer(kPeerAddr3);

  injectLocalRoutesAtRuntime({"30.31.0.0/16"}, {"3031:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3031:1"));

  injectLocalRoutesAtRuntime({"30.32.0.0/16"}, {"3032:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.32.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.32.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3032:1"));

  injectLocalRoutesAtRuntime({"30.33.0.0/16"}, {"3033:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.33.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.33.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3033:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* While WAITING, withdraw the shared route — becomes a CL item */
  withdrawLocalRoutesAtRuntime({"30.30.0.0/16"});

  /* Unblock peer3 — PL drains, then CL-origin withdrawal is delivered.
   * Use verifyRouteWithdraw to consume the CL-origin withdrawal after unblock
   * (learned pattern: CL items delivered after PL drain via verifyRoute*). */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Consume the CL-origin withdrawal on both peers */
  EXPECT_TRUE(verifyRouteWithdraw("v4", "30.30.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "30.30.0.0", 16, kPeerAddr4));

  /* Verify both peers stable and deliver new route */
  injectLocalRoutesAtRuntime({"30.34.0.0/16"}, {"3034:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.34.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.34.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3034:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.34.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3034:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOG(INFO, "=== TEST PASSED: GWaiting_RouteWithdrawal ===");
}

/* G-WAITING × E-SLOW-FREQ — Frequency detach during PL drain.
 * Per-peer queue sizes: peer4 gets large queue (10,8,0), peer3 gets small
 * queue (3,2,0) that fills naturally. Two block cycles with freq
 * threshold=2 → peer3 detaches while group is in WAITING state. */
TEST_P(UpdateGroupMultiPeerTest, GWaiting_SlowFreqDetach) {
  XLOG(INFO, "=== TEST: GWaiting_SlowFreqDetach ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* Large queue for fast peer */
  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr4);

  /* Small queue for slow peer — fills naturally without blockPeer */
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

  /* Set freq threshold: 2 blocks triggers detachment */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      2,
      std::chrono::milliseconds(60000));

  /* Block cycle #1: 3 routes fill peer3's small queue naturally */
  injectLocalRoutesAtRuntime({"30.40.0.0/16"}, {"3040:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.40.0.0/16")));
  injectLocalRoutesAtRuntime({"30.41.0.0/16"}, {"3041:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.41.0.0/16")));
  injectLocalRoutesAtRuntime({"30.42.0.0/16"}, {"3042:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.42.0.0/16")));

  /* Drain fast peer so it stays running */
  drainPeerQueueCompletely(peerId4);

  /* Peer3's small queue should be blocked (freq counter → 1) */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* Drain peer3 to recover from cycle #1 (freq counter stays at 1) */
  drainPeerQueueCompletely(peerId3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Block cycle #2: 3 more routes → freq counter hits 2 → detach.
   * Per Pattern 3: freq threshold check fires on NEXT packing cycle,
   * so all 3 routes must be injected before checking DETACHED_BLOCKED. */
  injectLocalRoutesAtRuntime({"30.43.0.0/16"}, {"3043:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.43.0.0/16")));
  injectLocalRoutesAtRuntime({"30.44.0.0/16"}, {"3044:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.44.0.0/16")));
  injectLocalRoutesAtRuntime({"30.45.0.0/16"}, {"3045:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.45.0.0/16")));

  /* Drain peer4 in order: 30.43 then 30.44 then verify */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.43.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3043:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.44.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3044:1"));

  /* Drain remaining routes from peer4 */
  drainPeerQueueCompletely(peerId4);

  /* Peer3 should now be DETACHED_BLOCKED after 2nd block cycle */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* After detachment, verify peer4 continues delivering routes normally */
  injectLocalRoutesAtRuntime({"30.46.0.0/16"}, {"3046:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.46.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.46.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3046:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: GWaiting_SlowFreqDetach ===");
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
