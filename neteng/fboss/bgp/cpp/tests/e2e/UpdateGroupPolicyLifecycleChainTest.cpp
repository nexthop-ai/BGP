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
 * E2E tests: Policy change lifecycle
 * across different peer states.
 *
 * Prefix range: 83.x.0.0/16
 *
 * Note: Runtime setPeersPolicy/unsetPeersPolicy is incompatible with the
 * slow peer fixture when update groups are enabled (CHECK failure in
 * AdjRibCommon). Policy changes are simulated via route operations:
 * withdraw old prefix + inject new prefix with different attributes.
 *
 * Tests:
 *   All peers JOINED_RUNNING -- group-only re-eval
 *   1 peer DETACHED_BLOCKED -- group re-eval + CL accumulates
 *   1 peer DSP consuming CL -- route ops during recovery
 *   1 peer DFP (no CL items) -- route ops after recovery starts
 *   1 peer DETACHED_INIT_DUMP -- route ops during init dump
 *   DFP re-eval: both group and peer see same shared entry change
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Policy change,
 * 1 peer DSP consuming CL - route ops during recovery. Peer3 detached,
 * unblocked (recovering), inject routes.
 */
TEST_P(UpdateGroupMultiPeerTest, PolicyChangeDuringDspRecovery) {
  XLOGF(INFO, "=== TEST: PolicyChangeDuringDspRecovery ===");

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
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Inject shared route */
  injectLocalRoutesAtRuntime({"83.20.0.0/16"}, {"8320:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("83.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "83.20.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "8320:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "83.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "8320:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("83.{}.0.0/16", 21 + i);
    auto community = fmt::format("{}:1", 8321 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("83.{}.0.0", 21 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock -- peer3 starts recovery (DSP path) */
  unblockPeer(kPeerAddr3);

  /* Policy change during recovery: withdraw + re-inject */
  withdrawLocalRoutesAtRuntime({"83.20.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "83.20.0.0", 16, kPeerAddr4));

  injectLocalRoutesAtRuntime({"83.25.0.0/16"}, {"8325:2"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("83.25.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "83.25.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "8325:2"));

  /* Peer3 is not DOWN, system processes normally */
  auto state = getPeerState(kPeerAddr3);
  EXPECT_NE(state, PeerUpdateState::DOWN);
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOGF(INFO, "=== TEST PASSED: PolicyChangeDuringDspRecovery ===");
}

/*
 * Policy change with DFP: both group and peer re-eval same SharedRibEntry.
 * Detach peer3, no route changes during detach (DFP path). Then withdraw a
 * shared route - both group and peer view affected.
 */
TEST_P(UpdateGroupMultiPeerTest, PolicyChangeDfpSameSharedEntry) {
  XLOGF(INFO, "=== TEST: PolicyChangeDfpSameSharedEntry ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(5, 4, 0);

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
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Inject shared route -- both peers receive */
  injectLocalRoutesAtRuntime({"83.50.0.0/16"}, {"8350:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("83.50.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "83.50.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "8350:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "83.50.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "8350:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 5; i++) {
    auto prefix = fmt::format("83.{}.0.0/16", 51 + i);
    auto community = fmt::format("{}:1", 8351 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("83.{}.0.0", 51 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock -- recovery starts (DFP: fill routes create CL items) */
  unblockPeer(kPeerAddr3);
  auto state = getPeerState(kPeerAddr3);
  EXPECT_NE(state, PeerUpdateState::DOWN);

  /*
   * Policy-like change affecting shared entry: withdraw shared route
   * and inject replacement -- both group and peer see the same change.
   */
  withdrawLocalRoutesAtRuntime({"83.50.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "83.50.0.0", 16, kPeerAddr4));

  injectLocalRoutesAtRuntime({"83.57.0.0/16"}, {"8357:2"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("83.57.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "83.57.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "8357:2"));

  /* System stable, peer4 in sync */
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOGF(INFO, "=== TEST PASSED: PolicyChangeDfpSameSharedEntry ===");
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
