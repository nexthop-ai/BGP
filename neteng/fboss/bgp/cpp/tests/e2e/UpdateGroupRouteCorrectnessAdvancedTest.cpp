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
 * E2E tests: Route Correctness Advanced Verification
 * Tests for route updates during detachment, re-announcements,
 * rapid block/unblock, and route injection during state transitions.
 *
 * Prefix range: 14.x.0.0/16
 * Fixture: UpdateGroupLifecycleTest
 *
 * Tests implemented:
 *   Route update during detachment
 *   Announce -> Withdraw -> Re-announce cycle
 *   Rapid block/unblock near threshold — routes never lost
 *   Route injection during every state transition
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {
/*
 * Route update from announcement->withdrawal->re-announcement.
 * Verify final state is correct on the in-sync peer after the full cycle.
 *
 * Flow:
 * 1. Both peers receive route with community 1440:1
 * 2. Detach peer3
 * 3. Withdraw the route -> peer4 sees withdrawal
 * 4. Re-announce same prefix with community 1440:99 -> peer4 sees it
 * 5. Verify final state on peer4 is the re-announced route
 */
TEST_P(UpdateGroupLifecycleTest, AnnounceWithdrawReannounce) {
  XLOG(INFO, "=== TEST: AnnounceWithdrawReannounce ===");

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

  /* Both peers receive initial route */
  injectLocalRoutesAtRuntime({"14.40.0.0/16"}, {"1440:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("14.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "14.40.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1440:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "14.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1440:1"));
  XLOG(INFO, "Checkpoint 1: initial announcement received by both");

  /* Detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"14.41.0.0/16"}, {"1441:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("14.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "14.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1441:1"));
  injectLocalRoutesAtRuntime({"14.42.0.0/16"}, {"1442:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("14.42.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "14.42.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1442:1"));
  injectLocalRoutesAtRuntime({"14.43.0.0/16"}, {"1443:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("14.43.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "14.43.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1443:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  XLOG(INFO, "Checkpoint 2: peer3 detached");

  /* Withdraw the route */
  withdrawLocalRoutesAtRuntime({"14.40.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "14.40.0.0", 16, kPeerAddr4));
  XLOG(INFO, "Checkpoint 3: withdrawal received by peer4");

  /* Re-announce with different community */
  injectLocalRoutesAtRuntime({"14.40.0.0/16"}, {"1440:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("14.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "14.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1440:99"));
  XLOG(INFO, "Checkpoint 4: re-announcement with new community received");

  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: AnnounceWithdrawReannounce ===");
}
/*
 * Route injection during every state transition.
 * Inject routes at each stage: JOINED_RUNNING, JOINED_BLOCKED,
 * DETACHED_BLOCKED, and after peer goes DOWN. Verify no route loss
 * on the in-sync peer at any stage.
 *
 * Flow:
 * 1. JOINED_RUNNING: both peers receive route
 * 2. JOINED_BLOCKED: peer3 blocked, route goes to peer4
 * 3. DETACHED_BLOCKED: peer3 detached, route goes to peer4
 * 4. After peer3 DOWN: peer4 still receives routes
 */
TEST_P(UpdateGroupLifecycleTest, RouteInjectionDuringEveryTransition) {
  XLOG(INFO, "=== TEST: RouteInjectionDuringEveryTransition ===");

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

  /* Stage 1: JOINED_RUNNING — both peers receive route */
  injectLocalRoutesAtRuntime({"14.60.0.0/16"}, {"1460:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("14.60.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "14.60.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1460:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "14.60.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1460:1"));
  XLOG(INFO, "Stage 1 PASSED: route during JOINED_RUNNING");

  /* Set threshold = 1 for detachment */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Stage 2: Block peer3, inject routes (JOINED_BLOCKED -> DETACHED_BLOCKED) */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"14.61.0.0/16"}, {"1461:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("14.61.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "14.61.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1461:1"));
  injectLocalRoutesAtRuntime({"14.62.0.0/16"}, {"1462:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("14.62.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "14.62.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1462:1"));
  injectLocalRoutesAtRuntime({"14.63.0.0/16"}, {"1463:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("14.63.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "14.63.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1463:1"));
  XLOG(INFO, "Stage 2 PASSED: routes during block/detach transition");

  /* Verify peer3 detached */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Stage 3: Route while DETACHED_BLOCKED — only peer4 */
  injectLocalRoutesAtRuntime({"14.64.0.0/16"}, {"1464:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("14.64.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "14.64.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1464:1"));
  XLOG(INFO, "Stage 3 PASSED: route during DETACHED_BLOCKED");

  /* Stage 4: Peer3 goes DOWN, inject route — peer4 still works */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  injectLocalRoutesAtRuntime({"14.65.0.0/16"}, {"1465:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("14.65.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "14.65.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1465:1"));
  XLOG(INFO, "Stage 4 PASSED: route after peer3 DOWN");

  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: RouteInjectionDuringEveryTransition ===");
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
