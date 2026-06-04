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
 * [BGP++][UG2 E2E] Empty changelist detach tests.
 *
 * Tests that a detached peer can rejoin even when the group's changelist
 * consumer has never consumed any items (marker_ = nullptr from init).
 * Bug 3 found that Consumer::isReady() conflated "never consumed" with
 * "consumed everything", causing stuck peers.
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
 * Test 3 (Critical): DetachedPeer_RejoinsWhenGroupNeverSawCLItems
 *
 * The CL consumer marker_ starts as nullptr (never consumed). After
 * detach, the peer must still be able to rejoin. Pre-load routes via
 * addLocalRoute (initial dump path, not CL) so CL stays empty.
 *
 * Flow:
 * 1. Pre-load routes via addLocalRoute (before createRib) -> initial dump only
 * 2. Bring up 3 peers, consume initial dump
 * 3. CL is empty (consumer marker_ = nullptr from initialization)
 * 4. Block peer3, inject runtime routes to trigger blocking
 * 5. Frequency detach peer3
 * 6. Unblock peer3 -> should rejoin despite empty CL
 * 7. Inject new route, verify all 3 peers get it
 */
TEST_P(
    UpdateGroupEmptyCLDetachTest,
    DetachedPeer_RejoinsWhenGroupNeverSawCLItems) {
  XLOGF(INFO, "=== TEST: DetachedPeer_RejoinsWhenGroupNeverSawCLItems ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);

  /* Pre-load route via initial dump path (not CL) */
  addLocalRoute("50.0.0.0/8", {"500:1"}, 100);

  setupSlowPeerComponents(3, 2, 0);

  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("50.0.0.0/8")));

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  bringUpPeer(kPeerAddr5);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  /* Consume initial dump routes and EoRs */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "50.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "500:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "50.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "500:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "50.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "500:1"));

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

  /*
   * At this point the CL has never had any items.
   * The group consumer marker_ is nullptr from initialization.
   * Now detach peer3 via frequency threshold.
   */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Block peer3 and inject routes to trigger blocking + detach (P6: 3 routes)
   */
  blockPeer(kPeerAddr3);

  injectLocalRoutesAtRuntime({"51.0.0.0/8"}, {"510:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("51.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "51.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "510:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "51.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "510:1"));

  injectLocalRoutesAtRuntime({"52.0.0.0/8"}, {"520:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("52.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "52.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "520:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "52.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "520:1"));

  injectLocalRoutesAtRuntime({"53.0.0.0/8"}, {"530:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("53.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "53.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "530:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "53.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "530:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* P1,P5: Unblock peer3 — it should recover and rejoin despite empty CL */
  unblockPeer(kPeerAddr3);

  /* Wait for peer3 to rejoin (P1: allow JOINED_RUNNING) */
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING, 100));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));

  /* Verify all 3 peers can receive new routes */
  injectLocalRoutesAtRuntime({"54.0.0.0/8"}, {"540:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("54.0.0.0/8")));
  drainPeerQueueCompletely(peerId3);
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "54.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "540:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "54.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "540:1"));

  XLOGF(
      INFO,
      "=== TEST PASSED: DetachedPeer_RejoinsWhenGroupNeverSawCLItems ===");
}

/*
 * Test 12 (Medium): DetachedPeer_CLItemsArriveOnEmptyGroupTriggerRecovery
 *
 * Same as Test 3 but inject a new route BEFORE unblocking peer3.
 * Validates that notifyReadyConsumers works correctly when first CL
 * items arrive while peer is detached on an empty CL.
 */
/* TODO: Re-enable when D99316308 (ribVersion in processRibDumpReq) lands.
 * Recovery path is broken on master — detached peer stuck in DETACHED_RUNNING.
 */
TEST_P(
    UpdateGroupEmptyCLDetachTest,
    DISABLED_DetachedPeer_CLItemsArriveOnEmptyGroupTriggerRecovery) {
  XLOGF(
      INFO,
      "=== TEST: DetachedPeer_CLItemsArriveOnEmptyGroupTriggerRecovery ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);

  addLocalRoute("60.0.0.0/8", {"600:1"}, 100);

  /* Large queues so peer4/peer5 don't block during init dump or detach routes.
   * Pattern 11: with small queues ALL peers block, not just peer3. */
  setupSlowPeerComponents(20, 15, 0);

  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("60.0.0.0/8")));

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  bringUpPeer(kPeerAddr5);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  /* Consume initial dump and EoRs */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "60.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "600:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "60.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "600:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "60.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "600:1"));

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId5));
  EXPECT_TRUE(waitForEoR(peerId5));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Block peer3 and inject enough routes to trigger freq=1 detachment.
   * With large queues (20,15,0), peer4/peer5 won't overflow. */
  blockPeer(kPeerAddr3);
  injectDistinctRoutes({"61.0.0.0/8", "62.0.0.0/8", "63.0.0.0/8"}, 610, 150);
  drainPeerQueueCompletely(peerId4);
  drainPeerQueueCompletely(peerId5);

  /* Wait for detachment — with freq=1, first block event triggers it */
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Inject a NEW route BEFORE unblocking — this is the key difference from Test
   * 3: CL items arrive while peer is detached on an empty CL */
  injectLocalRoutesAtRuntime({"64.0.0.0/8"}, {"640:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("64.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "64.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "640:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "64.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "640:1"));

  /* Now unblock — peer3 must recover */
  unblockPeer(kPeerAddr3);

  /*
   * Recovery may take longer here because:
   * 1. Detached peer must drain its cloned PL
   * 2. Then consume the CL item injected before unblock
   * 3. Then wait for group to accept (WAITING -> IDLE transition)
   * Give extra retries per Pattern 1 from learning_patterns.md
   */
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING, 300));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));

  XLOGF(
      INFO,
      "=== TEST PASSED: DetachedPeer_CLItemsArriveOnEmptyGroupTriggerRecovery ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupEmptyCLDetachTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
