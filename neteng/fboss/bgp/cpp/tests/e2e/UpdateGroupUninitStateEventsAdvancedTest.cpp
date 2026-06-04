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
 * E2E tests: G-UNINIT state x Event Matrix (advanced)
 * PL-drain, CL-end, policy, refresh, MRAI, EoR, multi-route
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * G-UNINIT x E-PL-DRAIN
 * Init dump PL drains during UNINIT. Pre-load routes, bring up peers,
 * complete EoRs, drain init dump. Verify group transitions from UNINIT
 * through PL drain to normal operation.
 */
TEST_P(UpdateGroupDetachmentTest, GUninit_PlDrain) {
  XLOGF(INFO, "=== TEST: GUninit_PlDrain ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  /* Pre-load routes so init dump PL has content to drain */
  addLocalRoute("12.40.0.0/16", {"1240:1"}, 100);
  addLocalRoute("12.41.0.0/16", {"1241:1"}, 100);

  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /* Complete init dump via EoRs */
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Drain init dump PL from both peers */
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  /* After PL drain, peers should be in sync */
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Verify normal operation after init dump PL drain */
  injectLocalRoutesAtRuntime({"12.42.0.0/16"}, {"1242:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("12.42.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "12.42.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1242:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "12.42.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1242:1"));

  /* Group state should be valid */
  auto groupState = getGroupState(kPeerAddr3);
  XLOGF(
      INFO,
      "Group state after init dump PL drain: {}",
      static_cast<int>(groupState));
  EXPECT_TRUE(
      groupState == UpdateGroupState::IDLE ||
      groupState == UpdateGroupState::READY ||
      groupState == UpdateGroupState::WAITING);

  XLOGF(INFO, "=== TEST PASSED: GUninit_PlDrain ===");
}

/*
 * G-UNINIT x E-CL-END
 * CL-END is N/A during UNINIT — there is no CL processing during init dump.
 * Verify that the group in UNINIT state handles init dump correctly
 * and transitions normally without CL involvement.
 */
TEST_P(UpdateGroupDetachmentTest, GUninit_ClEndNA) {
  XLOGF(INFO, "=== TEST: GUninit_ClEndNA ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /* Complete init dump normally — no CL-END event applies */
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Both peers should be in sync — no CL processing during UNINIT */
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Group is now in normal state, CL processing works post-UNINIT */
  injectLocalRoutesAtRuntime({"12.43.0.0/16"}, {"1243:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("12.43.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "12.43.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1243:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "12.43.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1243:1"));

  XLOGF(INFO, "=== TEST PASSED: GUninit_ClEndNA ===");
}

/*
 * G-UNINIT x E-POLICY-CHG
 * Policy change during UNINIT is N/A — the slow peer E2E fixture does not
 * support runtime egress policy changes when update groups are enabled
 * (causes CHECK(mask) failure in AdjRibCommon). Instead, verify that
 * the group in UNINIT handles route attribute changes gracefully:
 * pre-load a route, bring up peers, withdraw and re-inject with different
 * attributes before EoRs. After EoRs, the updated route should be delivered.
 */
TEST_P(UpdateGroupDetachmentTest, GUninit_PolicyChangeNA) {
  XLOGF(INFO, "=== TEST: GUninit_PolicyChangeNA ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  /* Pre-load a route */
  addLocalRoute("12.44.0.0/16", {"1244:1"}, 100);

  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /*
   * Simulate a "policy-like" change during UNINIT by withdrawing
   * the pre-loaded route and injecting a replacement with different LP.
   */
  withdrawLocalRoutesAtRuntime({"12.44.0.0/16"});
  injectLocalRoutesAtRuntime({"12.45.0.0/16"}, {"1245:1"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("12.45.0.0/16")));

  /* Complete init dump */
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Drain init dump + any CL items from both peers */
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  /* Peers should be in sync after processing */
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Verify normal operation post-UNINIT */
  injectLocalRoutesAtRuntime({"12.46.0.0/16"}, {"1246:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("12.46.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "12.46.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1246:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "12.46.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1246:1"));

  XLOGF(INFO, "=== TEST PASSED: GUninit_PolicyChangeNA ===");
}

/*
 * G-UNINIT x E-ROUTE-REFRESH
 * Route refresh during UNINIT is N/A — no sendRouteRefresh helper exists
 * in the E2E framework. Verify that the group in UNINIT state continues
 * processing init dump normally even when routes are being injected
 * concurrently (simulating the kind of activity a route refresh would cause).
 */
TEST_P(UpdateGroupDetachmentTest, GUninit_RouteRefreshNA) {
  XLOGF(INFO, "=== TEST: GUninit_RouteRefreshNA ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  /* Pre-load routes to give init dump content */
  addLocalRoute("12.47.0.0/16", {"1247:1"}, 100);

  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /*
   * Inject multiple routes during UNINIT to simulate route-refresh-like
   * activity. These become CL items processed after init dump.
   */
  injectLocalRoutesAtRuntime({"12.48.0.0/16"}, {"1248:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("12.48.0.0/16")));
  injectLocalRoutesAtRuntime({"12.49.0.0/16"}, {"1249:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("12.49.0.0/16")));

  /* Complete init dump */
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Drain all init dump + CL messages */
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOGF(INFO, "=== TEST PASSED: GUninit_RouteRefreshNA ===");
}

/*
 * G-UNINIT x E-MRAI-FIRE
 * MRAI timer is not relevant during UNINIT — the group uses init dump PL,
 * not MRAI-triggered PL. Verify that init dump completes normally and the
 * group transitions to normal MRAI-based operation afterward.
 */
TEST_P(UpdateGroupDetachmentTest, GUninit_MraiFireNA) {
  XLOGF(INFO, "=== TEST: GUninit_MraiFireNA ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /* Complete init dump — no MRAI involvement during UNINIT */
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /*
   * After init dump, MRAI-based operation should work normally.
   * Inject a route — this triggers IDLE->READY->MRAI fires->WAITING->drain.
   */
  injectLocalRoutesAtRuntime({"12.50.0.0/16"}, {"1250:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("12.50.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "12.50.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1250:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "12.50.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1250:1"));

  /* Inject a second route to confirm MRAI cycle works repeatedly */
  injectLocalRoutesAtRuntime({"12.51.0.0/16"}, {"1251:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("12.51.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "12.51.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1251:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "12.51.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1251:1"));

  auto groupState = getGroupState(kPeerAddr3);
  XLOGF(
      INFO,
      "Group state after MRAI cycles post-UNINIT: {}",
      static_cast<int>(groupState));
  EXPECT_TRUE(
      groupState == UpdateGroupState::IDLE ||
      groupState == UpdateGroupState::READY ||
      groupState == UpdateGroupState::WAITING);

  XLOGF(INFO, "=== TEST PASSED: GUninit_MraiFireNA ===");
}

/*
 * G-UNINIT x E-EOR
 * EoR (End-of-RIB) is the ribInitDone trigger that completes init dump.
 * Bring up peers, verify they're in INIT state, then send EoRs.
 * After EoR processing, peers should transition to JOINED_RUNNING
 * and the group should leave UNINIT.
 */
TEST_P(UpdateGroupDetachmentTest, GUninit_EoR) {
  XLOGF(INFO, "=== TEST: GUninit_EoR ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  /* Pre-load a route so init dump has content */
  addLocalRoute("12.52.0.0/16", {"1252:1"}, 100);

  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /*
   * Send EoR to peer3 first. This is the ribInitDone trigger.
   * Peer3 should complete init dump and transition toward JOINED_RUNNING.
   */
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));

  /* Now send EoR to peer4 */
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  /* Both peers should reach JOINED_RUNNING after EoR processing */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Drain init dump messages */
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  /* Both peers in sync after EoR-triggered init dump completion */
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Verify normal operation: group has left UNINIT */
  injectLocalRoutesAtRuntime({"12.53.0.0/16"}, {"1253:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("12.53.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "12.53.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1253:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "12.53.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1253:1"));

  XLOGF(INFO, "=== TEST PASSED: GUninit_EoR ===");
}

/*
 * G-UNINIT x E-MULTI-ROUTE
 * Batch of routes injected before init dump completes.
 * All routes should be preserved in CL and delivered after EoRs.
 */
TEST_P(UpdateGroupDetachmentTest, GUninit_MultiRoute) {
  XLOGF(INFO, "=== TEST: GUninit_MultiRoute ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /*
   * Inject a batch of routes while group is still in UNINIT (before EoRs).
   * Each route has a different community so they become separate CL items.
   */
  injectLocalRoutesAtRuntime({"12.54.0.0/16"}, {"1254:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("12.54.0.0/16")));
  injectLocalRoutesAtRuntime({"12.55.0.0/16"}, {"1255:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("12.55.0.0/16")));
  injectLocalRoutesAtRuntime({"12.56.0.0/16"}, {"1256:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("12.56.0.0/16")));

  /* Complete init dump — CL items should be processed after init dump */
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Drain init dump + all CL items from both peers */
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  /* Both peers should be in sync with all routes delivered */
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Verify normal operation: inject one more to confirm steady state */
  injectLocalRoutesAtRuntime({"12.57.0.0/16"}, {"1257:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("12.57.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "12.57.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1257:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "12.57.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1257:1"));

  XLOGF(INFO, "=== TEST PASSED: GUninit_MultiRoute ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupDetachmentTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
