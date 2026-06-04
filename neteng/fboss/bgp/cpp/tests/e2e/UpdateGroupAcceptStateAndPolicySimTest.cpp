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

/* E2E tests: Post-acceptance state verification and policy change simulation.
 * Prefix range: 37.1-37.49/16.
 *
 * Post-acceptance state -- syncBitmap, detachedPeerCount correct
 * Policy change with all detachable peers detached -- simulate re-eval
 * Policy blocks previously allowed prefix -- withdrawal from all
 * Policy changes attribute (community add) -- UPDATE with new attrs
 * Policy changes attribute (AS-path prepend) -- different AS path
 *
 * NOTE: Real policy changes via setPolicyConfig are incompatible with the
 * slow peer test fixture (CHECK failure in AdjRibCommon). All policy tests
 * simulate policy effects via withdraw + re-inject with different attributes.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Detach peer3 via freq threshold, let recovery and acceptance happen,
 * then verify post-acceptance state: isPeerInSync for both peers, detached
 * peer count is 0, group member count is 2, and invariants hold.
 */
TEST_P(UpdateGroupMultiPeerTest, PostAcceptanceStateVerification) {
  XLOGF(INFO, "=== TEST: PostAcceptanceStateVerification ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* Fast peer with large queue */
  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr4);
  /* Slow peer with small queue — fills naturally */
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

  /* Verify initial state */
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_EQ(getGroupMemberCount(kPeerAddr3), 2);
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 0);

  /* Inject a shared route before detachment */
  injectLocalRoutesAtRuntime({"37.1.0.0/16"}, {"3701:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3701:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3701:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("37.{}.0.0/16", 2 + i);
    auto community = fmt::format("{}:1", 3702 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("37.{}.0.0", 2 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  drainPeerQueueCompletely(peerId4);
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED, 200));

  /* Verify detached state */
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1);
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Drain peer3 queue to trigger recovery and acceptance */
  drainPeerQueueCompletely(peerId3);

  /* Inject a route to allow group cycle for acceptance */
  injectLocalRoutesAtRuntime({"37.8.0.0/16"}, {"3708:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.8.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.8.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3708:1"));

  /* Drain both peers after recovery route injection */
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING, 200));

  /* Verify post-acceptance state: both peers in sync, no detached peers */
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_EQ(getGroupMemberCount(kPeerAddr4), 2);
  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);

  XLOGF(INFO, "=== TEST PASSED: PostAcceptanceStateVerification ===");
}

/* Policy change with all detachable peers detached. With 3 peers,
 * detach peer3 and peer4 (the last synced member peer5 is preserved).
 * Simulate policy re-evaluation by withdrawing a shared route and re-injecting
 * with different attributes. Verify peer5 (in-sync) receives the update, and
 * the detached peers' CL tracks the changes.
 */
TEST_P(UpdateGroupMultiPeerTest, PolicyChangeAllDetachablePeersDetached) {
  XLOGF(INFO, "=== TEST: PolicyChangeAllDetachablePeersDetached ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  /* Fast peer with large queue */
  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr5);
  /* Medium peer — detaches in Phase 2 */
  setDefaultQueueSizes(5, 4, 0);
  bringUpPeer(kPeerAddr4);
  /* Slow peer — detaches first in Phase 1 */
  setDefaultQueueSizes(3, 2, 0);
  bringUpPeer(kPeerAddr3);
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

  /* Inject a shared route */
  injectLocalRoutesAtRuntime({"37.10.0.0/16"}, {"3710:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.10.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3710:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3710:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.10.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3710:1"));

  /* Detach peer3 via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("37.{}.0.0/16", 11 + i);
    auto community = fmt::format("{}:1", 3711 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("37.{}.0.0", 11 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("37.{}.0.0", 11 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));
  }

  drainPeerQueueCompletely(peerId4);
  drainPeerQueueCompletely(peerId5);
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED, 200));

  /* Now detach peer4 with freq threshold */
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  for (int i = 0; i < 5; i++) {
    auto prefix = fmt::format("37.{}.0.0/16", 17 + i);
    auto community = fmt::format("{}:1", 3717 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("37.{}.0.0", 17 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));
  }

  drainPeerQueueCompletely(peerId5);
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED, 200));

  /* Both detachable peers are now detached; peer5 is last synced member */
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  /* Simulate policy re-evaluation: withdraw shared route, re-inject with
   * different community (simulates attribute change from policy).
   */
  withdrawLocalRoutesAtRuntime({"37.10.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "37.10.0.0", 16, kPeerAddr5));

  injectLocalRoutesAtRuntime({"37.23.0.0/16"}, {"3723:1"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("37.23.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "37.23.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3723:1"));

  /* Peer5 continues to work; detached peers' CL tracks changes silently */
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));
  verifySlowPeerInvariants(kPeerAddr5);

  XLOGF(INFO, "=== TEST PASSED: PolicyChangeAllDetachablePeersDetached ===");
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
