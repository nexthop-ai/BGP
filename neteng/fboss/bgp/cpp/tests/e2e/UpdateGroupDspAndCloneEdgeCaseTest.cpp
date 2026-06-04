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

/* E2E tests: DSP group-slower-than-peer and lazy clone edge cases.
 * Prefix range: 32.x.0.0/16.
 *
 * DSP with group slower than peer -- peer finishes CL before accept
 * Three updates same prefix -- only first triggers clone (Case 1)
 * Clone then CL updates per-peer entry -- attributes diverge
 * Clone preserves pathId -- add-path scenarios (2 distinct prefixes)
 * Clone when radix tree has no subtree for prefix -- edge case
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* DSP with group slower than peer -- peer finishes CL before group
 * accepts. Detach, inject CL items, unblock. DSP consumer may outpace group
 * acceptance. Verify no crash and valid recovery state.
 */
TEST_P(UpdateGroupMultiPeerTest, DspPeerFinishesClBeforeGroupAccepts) {
  XLOGF(INFO, "=== TEST: DspPeerFinishesClBeforeGroupAccepts ===");

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
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  /* Inject a shared route -- both peers receive before detachment */
  injectLocalRoutesAtRuntime({"32.1.0.0/16"}, {"3201:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3201:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3201:1"));

  /* Freq-detach peer3: threshold=1 block cycle */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);

  /* Fill queue: 3 routes with different communities */
  for (int i = 2; i <= 4; i++) {
    auto prefix = fmt::format("32.{}.0.0/16", i);
    auto community = fmt::format("32{:02d}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("32.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Inject multiple CL items while detached -- these are what DSP must
   * process. Inject rapidly so peer's CL consumer may outpace group. */
  for (int i = 5; i <= 8; i++) {
    auto prefix = fmt::format("32.{}.0.0/16", i);
    auto community = fmt::format("32{:02d}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("32.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Unblock -- DSP processes CL items. Peer may finish CL before group
   * completes acceptance (group acceptance is async). No crash expected. */
  unblockPeer(kPeerAddr3);

  /* After unblock + CL drain, peer may have rejoined or re-blocked.
   * The exact state depends on queue drain timing. Any non-DOWN state
   * is valid -- the test goal is no crash during DSP processing. */
  auto state = getPeerState(kPeerAddr3);
  EXPECT_NE(state, PeerUpdateState::DOWN);

  /* Verify peer4 continues functioning after DSP processing */
  injectLocalRoutesAtRuntime({"32.9.0.0/16"}, {"3209:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.9.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.9.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3209:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);
  XLOGF(INFO, "=== TEST PASSED: DspPeerFinishesClBeforeGroupAccepts ===");
}

/*
 * Clone when radix tree has no subtree for prefix -- edge case.
 * Isolated prefix with no radix siblings. Clone handles fresh lookup path.
 */
TEST_P(UpdateGroupLazyCloneTest, CloneNoRadixSubtreeForPrefix) {
  XLOGF(INFO, "=== TEST: CloneNoRadixSubtreeForPrefix ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(5, 4, 0);

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
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  /* Inject an isolated prefix in a unique /24 range -- no radix siblings */
  injectLocalRoutesAtRuntime({"32.40.0.0/16"}, {"3240:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.40.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3240:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3240:1"));

  /* Freq-detach peer3 with queue (5,4,0) */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);

  for (int i = 41; i <= 45; i++) {
    auto prefix = fmt::format("32.{}.0.0/16", i);
    auto community = fmt::format("32{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("32.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Case 4 clone on the isolated prefix -- radix tree has no other
   * entries in this subtree. Clone logic must create per-peer entry
   * from scratch without relying on existing subtree structure. */
  injectLocalRoutesAtRuntime({"32.40.0.0/16"}, {"3246:1"}, 160);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3246:1"));
  verifySlowPeerInvariants(kPeerAddr3);

  /* Update again -- Case 1, per-peer entry exists, no re-clone */
  injectLocalRoutesAtRuntime({"32.40.0.0/16"}, {"3247:1"}, 170);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3247:1"));
  verifySlowPeerInvariants(kPeerAddr3);

  /* Withdraw -- Case 1, per-peer entry persists */
  withdrawLocalRoutesAtRuntime({"32.40.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "32.40.0.0", 16, kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  XLOGF(INFO, "=== TEST PASSED: CloneNoRadixSubtreeForPrefix ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupMultiPeerTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupLazyCloneTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
