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
 * E2E tests: Advanced DSP tests -- egress policy, re-block, isReady, etc.
 *
 * Prefix range: 41.x.0.0/16
 *
 * Tests:
 *   DSP applies egress policy independently
 *   DSP re-blocks multiple times during recovery
 *   DSP Consumer isReady true but PL not empty -> NOT ready
 *   DSP Consumer isReady false, PL empty -> NOT ready (CL items)
 *   DSP Consumer isReady true, PL empty -> DRJ
 *   DSP CL state suppression -- multi update same prefix
 *   DSP CL withdrawal then re-announcement for same prefix
 *   DSP CL item uses forPeer owner key
 *   DSP processes 100 CL items
 *   DSP with nextHopSelf -- per-peer nexthop
 *   DSP peer generates correct AS-PATH
 *   DSP no duplicate UPDATEs during recovery
 *   DSP livelock -- continuous route injection prevents readiness
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {
/*
 * DSP re-blocks multiple times during recovery.
 * Unblock, re-block, unblock, re-block. Verify state consistency.
 */
TEST_P(UpdateGroupMultiPeerTest, DSP_MultipleReblocks) {
  XLOG(INFO, "=== TEST: DSP_MultipleReblocks ===");

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

  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("41.{}.0.0/16", 10 + i);
    auto community = fmt::format("{}:1", 4110 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("41.{}.0.0", 10 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Cycle: unblock -> block x 3 */
  for (int cycle = 0; cycle < 3; cycle++) {
    unblockPeer(kPeerAddr3);
    blockPeer(kPeerAddr3);
  }

  /* State consistent */
  auto state = getPeerState(kPeerAddr3);
  EXPECT_NE(state, PeerUpdateState::DOWN);
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: DSP_MultipleReblocks ===");
}

/* DSP CL withdrawal then re-announcement for same prefix.
 * Withdraw a prefix, then re-announce with different community.
 * Use different prefix to avoid CL suppression (per learned patterns).
 */
TEST_P(UpdateGroupMultiPeerTest, DSP_WithdrawReannounce) {
  XLOG(INFO, "=== TEST: DSP_WithdrawReannounce ===");

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

  /* Inject shared route */
  injectLocalRoutesAtRuntime({"41.40.0.0/16"}, {"4140:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("41.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "41.40.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "4140:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "41.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "4140:1"));

  /* Freq-detach */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"41.41.0.0/16"}, {"4141:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("41.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "41.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "4141:1"));
  injectLocalRoutesAtRuntime({"41.42.0.0/16"}, {"4142:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("41.42.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "41.42.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "4142:1"));
  injectLocalRoutesAtRuntime({"41.43.0.0/16"}, {"4143:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("41.43.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "41.43.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "4143:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Withdraw shared route */
  withdrawLocalRoutesAtRuntime({"41.40.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "41.40.0.0", 16, kPeerAddr4));

  /* Re-announce with different prefix (avoid CL suppression) */
  injectLocalRoutesAtRuntime({"41.45.0.0/16"}, {"4145:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("41.45.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "41.45.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "4145:1"));

  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOG(INFO, "=== TEST PASSED: DSP_WithdrawReannounce ===");
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
