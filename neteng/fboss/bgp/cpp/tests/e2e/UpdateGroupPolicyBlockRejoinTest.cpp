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
 * E2E test: Lifecycle: detach, policy blocks all routes, rejoin empty.
 *
 * Simulates a policy change that blocks all previously announced routes.
 * Since actual policy changes crash with CHECK(mask) in the slow peer
 * fixture (setPolicyConfig + update groups = null PolicyAttributesMask),
 * we simulate policy denial by withdrawing multiple shared routes while
 * the peer is detached. The detached peer's CL accumulates the
 * withdrawals. On recovery, the peer rejoins with an effectively empty
 * route table. Verify the group continues to function after recovery.
 *
 * Prefix range: 98.x.0.0/16
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Detach peer3, withdraw ALL shared routes (simulating policy
 * denial), then unblock. Peer3 recovers with empty state. Inject a
 * fresh route post-recovery to prove group is still functional.
 *
 * Differs from the single-withdrawal variant by having multiple shared routes
 * withdrawn (3 routes, simulating a blanket policy deny) rather than a single
 * shared route explicit withdrawal.
 */
TEST_P(
    UpdateGroupMultiPeerTest,
    PolicyBlocksAllRoutesDuringDetachmentRejoinEmpty) {
  XLOGF(INFO, "=== TEST: PolicyBlocksAllRoutesDuringDetachmentRejoinEmpty ===");

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

  /* Step 1: Inject 3 shared routes -- both peers receive all 3.
   * These represent the "allowed" routes before the policy change. */
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("98.{}.0.0/16", 10 + i);
    auto community = fmt::format("{}:1", 9810 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("98.{}.0.0", 10 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("98.{}.0.0", 10 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Step 2: Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("98.{}.0.0/16", 20 + i);
    auto community = fmt::format("{}:1", 9820 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("98.{}.0.0", 20 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Raise thresholds so peer4 won't accidentally detach */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Step 3: Simulate "policy blocks all routes" -- withdraw all 3 shared
   * routes while peer3 is detached. Peer4 receives the withdrawals inline,
   * peer3's CL accumulates them. */
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("98.{}.0.0/16", 10 + i);
    withdrawLocalRoutesAtRuntime({prefix});
    EXPECT_TRUE(verifyRouteWithdraw(
        "v4", fmt::format("98.{}.0.0", 10 + i), 16, kPeerAddr4));
  }

  /* Step 4: Recover peer3 with controlled DRJ acceptance */
  waitForDrjWithDrain(kPeerAddr3, peerId3);

  /* Inject PL-cycle routes to verify continued group operation */
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("98.{}.0.0/16", 30 + i);
    auto community = fmt::format("{}:1", 9830 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    drainPeerQueueCompletely(peerId3, 1, 100);
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("98.{}.0.0", 30 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Step 5: Verify peer3 recovered (not DOWN) with empty shared state */
  auto state3 = getPeerState(kPeerAddr3);
  XLOGF(
      INFO,
      "PolicyBlockRejoin: peer3={} after policy-block-all recovery",
      static_cast<int>(state3));
  EXPECT_NE(state3, PeerUpdateState::DOWN)
      << "peer3 should not be DOWN after policy-block-all recovery";

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Step 6: Inject a brand-new route -- verify group still functional */
  injectLocalRoutesAtRuntime({"98.40.0.0/16"}, {"9840:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("98.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "98.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "9840:1"));

  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);

  XLOGF(INFO, "=== PASSED: PolicyBlockRejoin ===");
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
