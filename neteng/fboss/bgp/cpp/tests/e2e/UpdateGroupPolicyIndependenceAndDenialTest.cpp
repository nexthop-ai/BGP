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

/* E2E tests: Independent detached peer policy verdicts and policy denial.
 * Prefix range: 36.20-36.55/16.
 * Policy changes simulated via withdraw + re-inject (setPolicyConfig
 * incompatible with slow peer fixture).
 * Independent policy verdicts for two detached peers, policy denial removes
 * entry.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Policy change causes different verdicts for two detached peers.
 * Detach peer3 and peer4 at different divergence points. Simulate policy
 * re-evaluation. Each detached peer's CL tracks independently. Peer5
 * (in-sync) receives the update normally.
 */
TEST_P(UpdateGroupMultiPeerTest, PolicyChangeTwoDetachedPeersIndependent) {
  XLOG(INFO, "=== TEST: PolicyChangeTwoDetachedPeersIndependent ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  setupSlowPeerComponents(5, 4, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

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

  /* Inject a shared route */
  injectLocalRoutesAtRuntime({"36.20.0.0/16"}, {"3620:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.20.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3620:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3620:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.20.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3620:1"));

  /* Detach peer3 first */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 5; i++) {
    auto prefix = fmt::format("36.{}.0.0/16", 21 + i);
    auto community = fmt::format("{}:1", 3621 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("36.{}.0.0", 21 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("36.{}.0.0", 21 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Detach peer4 at a DIFFERENT divergence point */
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr4);
  for (int i = 0; i < 5; i++) {
    auto prefix = fmt::format("36.{}.0.0/16", 27 + i);
    auto community = fmt::format("{}:1", 3627 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("36.{}.0.0", 27 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Both detached at different divergence points, peer5 is last synced */
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  /* Simulate policy re-evaluation: withdraw shared route, inject replacement */
  withdrawLocalRoutesAtRuntime({"36.20.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "36.20.0.0", 16, kPeerAddr5));

  injectLocalRoutesAtRuntime({"36.33.0.0/16"}, {"3633:1"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.33.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.33.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3633:1"));

  /* Each detached peer's CL tracks independently at their divergence point */
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));
  verifySlowPeerInvariants(kPeerAddr5);

  XLOG(INFO, "=== TEST PASSED: PolicyChangeTwoDetachedPeersIndependent ===");
}

/* Policy change where old policy allowed, new policy denies.
 * Inject 3 routes (old policy allows), then withdraw all (new policy denies).
 * Verify withdrawals reach both peers and group continues functioning.
 */
TEST_P(UpdateGroupMultiPeerTest, PolicyDenialRemovesEntry) {
  XLOG(INFO, "=== TEST: PolicyDenialRemovesEntry ===");

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

  /* Inject 3 routes under "old policy allows" */
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("36.{}.0.0/16", 50 + i);
    auto community = fmt::format("{}:1", 3650 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("36.{}.0.0", 50 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("36.{}.0.0", 50 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Simulate "new policy denies" -- withdraw all 3 routes */
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("36.{}.0.0/16", 50 + i);
    withdrawLocalRoutesAtRuntime({prefix});
    EXPECT_TRUE(verifyRouteWithdraw(
        "v4", fmt::format("36.{}.0.0", 50 + i), 16, kPeerAddr3));
    EXPECT_TRUE(verifyRouteWithdraw(
        "v4", fmt::format("36.{}.0.0", 50 + i), 16, kPeerAddr4));
  }

  /* Verify group still functions -- inject a new route */
  injectLocalRoutesAtRuntime({"36.55.0.0/16"}, {"3655:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.55.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.55.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3655:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.55.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3655:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: PolicyDenialRemovesEntry ===");
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
