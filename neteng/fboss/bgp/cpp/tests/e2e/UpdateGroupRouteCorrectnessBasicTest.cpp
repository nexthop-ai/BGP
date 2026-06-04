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
 * E2E tests: Route Correctness Basic Verification
 * Tests for community correctness, ghost routes, and withdrawal
 * reflection through detach cycles.
 *
 * Test plan:
 * https://docs.google.com/document/d/11lBp_Q_i6UYocI3meYbI3sUShZzZsu6Qlq8iSCdVXRc
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Community correctness through detach cycle.
 * Route with specific community, detach peer, verify in-sync peer
 * still has correct community on subsequent routes.
 *
 * Flow:
 * 1. Both peers JOINED_RUNNING, receive route with community 1401:1
 * 2. Detach peer3
 * 3. Inject route with community 1402:2 -> only peer4 receives it
 * 4. Verify peer4 sees correct community on new route
 */
TEST_P(UpdateGroupLifecycleTest, CommunityCorrectnessThruDetach) {
  XLOG(INFO, "=== TEST: CommunityCorrectnessThruDetach ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  // note: not duplication - waiting for v4 and v6 EOR
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Both peers receive route with specific community */
  injectLocalRoutesAtRuntime({"14.1.0.0/16"}, {"1401:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("14.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "14.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1401:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "14.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1401:1"));
  XLOG(INFO, "Checkpoint 1: both peers received route with community 1401:1");

  /* Detach peer3 via frequency threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"14.2.0.0/16"}, {"1402:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("14.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "14.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1402:1"));
  injectLocalRoutesAtRuntime({"14.3.0.0/16"}, {"1403:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("14.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "14.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1403:1"));
  injectLocalRoutesAtRuntime({"14.4.0.0/16"}, {"1404:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("14.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "14.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1404:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  XLOG(INFO, "Checkpoint 2: peer3 detached");

  /* Inject route with different community, verify peer4 sees correct value */
  injectLocalRoutesAtRuntime({"14.5.0.0/16"}, {"1405:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("14.5.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "14.5.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1405:99"));
  XLOG(INFO, "Checkpoint 3: peer4 received route with correct community");

  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: CommunityCorrectnessThruDetach ===");
}

/*
 * No ghost routes after detach.
 * Withdraw prefixes while peer is detached, verify in-sync peer
 * does NOT have those withdrawn prefixes (no ghost routes).
 *
 * Flow:
 * 1. Both peers receive 2 routes
 * 2. Detach peer3
 * 3. Withdraw one route -> peer4 gets withdrawal
 * 4. Verify peer4 properly reflects the withdrawal (no ghost)
 */
TEST_P(UpdateGroupLifecycleTest, NoGhostRoutesAfterDetach) {
  XLOG(INFO, "=== TEST: NoGhostRoutesAfterDetach ===");

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

  /* Both peers receive two routes */
  injectLocalRoutesAtRuntime({"14.10.0.0/16"}, {"1410:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("14.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "14.10.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1410:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "14.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1410:1"));

  /* Detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"14.11.0.0/16"}, {"1411:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("14.11.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "14.11.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1411:1"));
  injectLocalRoutesAtRuntime({"14.12.0.0/16"}, {"1412:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("14.12.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "14.12.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1412:1"));
  injectLocalRoutesAtRuntime({"14.13.0.0/16"}, {"1413:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("14.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "14.13.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1413:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  XLOG(INFO, "Checkpoint 1: peer3 detached");

  /* Withdraw the pre-detach route -> peer4 gets withdrawal, no ghost */
  withdrawLocalRoutesAtRuntime({"14.10.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "14.10.0.0", 16, kPeerAddr4));
  XLOG(INFO, "Checkpoint 2: peer4 received withdrawal, no ghost route");

  /* Withdraw a post-detach route too -> peer4 gets it */
  withdrawLocalRoutesAtRuntime({"14.11.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "14.11.0.0", 16, kPeerAddr4));
  XLOG(INFO, "Checkpoint 3: second withdrawal confirmed");

  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: NoGhostRoutesAfterDetach ===");
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
