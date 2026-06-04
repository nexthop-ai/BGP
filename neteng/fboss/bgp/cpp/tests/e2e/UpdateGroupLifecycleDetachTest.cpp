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
 * E2E tests: Lifecycle Detach Chain
 * Tests for full detach lifecycle and post-detach route behavior.
 *
 * Fixture: UpdateGroupLifecycleTest
 *
 * Tests implemented:
 *   FullDetachWithdrawPeerDown: detect -> detach -> withdraw -> peer down
 *   PostDetachRoutesAndWithdrawals: routes after detachment skip detached peer
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Test: Full lifecycle - detect, detach, withdraw, peer down.
 * Verify routes, state, and invariants at each checkpoint.
 *
 * Lifecycle:
 * 1. Both peers JOINED_RUNNING, receive routes
 * 2. Peer3 blocks -> JOINED_BLOCKED -> DETACHED_BLOCKED
 * 3. Group sends new routes only to peer4
 * 4. Group withdraws routes, peer4 receives withdrawals
 * 5. Peer3 goes down while still DETACHED_BLOCKED
 * 6. Verify clean shutdown
 */
TEST_P(UpdateGroupLifecycleTest, FullDetachWithdrawPeerDown) {
  XLOG(INFO, "=== TEST: FullDetachWithdrawPeerDown ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* NOTE this is NOT duplicate, waiting for both v4 and V6 EOR */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Checkpoint 1: Both peers receive a route */
  injectLocalRoutesAtRuntime({"170.0.0.0/8"}, {"1700:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("170.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "170.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1700:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "170.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1700:1"));
  XLOG(INFO, "Checkpoint 1 PASSED: both peers received route 170");

  /* Checkpoint 2: Detach peer3 via frequency (threshold=1) */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"171.0.0.0/8"}, {"1710:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("171.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "171.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1710:1"));
  injectLocalRoutesAtRuntime({"172.0.0.0/8"}, {"1720:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("172.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "172.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1720:1"));
  injectLocalRoutesAtRuntime({"173.0.0.0/8"}, {"1730:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("173.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "173.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1730:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));
  XLOG(INFO, "Checkpoint 2 PASSED: peer3 detached");

  /* Checkpoint 3: Withdraw a route, only peer4 receives it */
  withdrawLocalRoutesAtRuntime({"172.0.0.0/8"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "172.0.0.0", 8, kPeerAddr4));
  XLOG(INFO, "Checkpoint 3 PASSED: peer4 received withdrawal");

  /* Checkpoint 4: Peer3 goes down while DETACHED_BLOCKED */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 0);
  XLOG(INFO, "Checkpoint 4 PASSED: peer3 down, detached bits cleaned");

  /* Checkpoint 5: Peer4 still receives routes after peer3 cleanup */
  injectLocalRoutesAtRuntime({"174.0.0.0/8"}, {"1740:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("174.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "174.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1740:1"));

  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "Checkpoint 5 PASSED: peer4 functional after peer3 cleanup");

  XLOG(INFO, "=== TEST PASSED: FullDetachWithdrawPeerDown ===");
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
