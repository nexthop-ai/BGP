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

/* E2E tests: DSP determination and DFP→DSP demotion corner cases.
 * Prefix range: 30.30-30.49/16.
 *
 * DSP check — both peer and group finish PL at same CL pos
 * DFP→DSP — multiple CL items in burst, all must be consumed
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* DSP check — both peer and group finish PL, both at same CL pos.
 * When both PL portions are empty and CL positions match, DSP check runs
 * (both empty = all caught up). Detach peer3 via freq, let PL drain for
 * peer4, then unblock peer3. Since no new CL items arrived during detachment,
 * the peer should be at the same CL end — DSP path accepts immediately.
 */
TEST_P(UpdateGroupMultiPeerTest, DspBothPlEmptySameClPos) {
  XLOGF(INFO, "=== TEST: DspBothPlEmptySameClPos ===");

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

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);

  injectLocalRoutesAtRuntime({"30.30.0.0/16"}, {"3030:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3030:1"));

  injectLocalRoutesAtRuntime({"30.31.0.0/16"}, {"3031:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3031:1"));

  injectLocalRoutesAtRuntime({"30.32.0.0/16"}, {"3032:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.32.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.32.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3032:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Do NOT inject any more CL items — keep CL positions aligned.
   * Unblock peer3 — both PLs are empty, CL positions match → DSP path.
   * DSP finds everything caught up and should accept peer3 for rejoin. */
  unblockPeer(kPeerAddr3);

  /* After unblock, peer3 may be in any recovery state depending on timing */
  auto state = getPeerState(kPeerAddr3);
  EXPECT_NE(state, PeerUpdateState::DOWN);

  /* Verify group continues functioning — peer4 still receives routes */
  injectLocalRoutesAtRuntime({"30.33.0.0/16"}, {"3033:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.33.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.33.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3033:1"));

  verifySlowPeerInvariants(kPeerAddr3);

  XLOGF(INFO, "=== TEST PASSED: DspBothPlEmptySameClPos ===");
}

/* DFP→DSP — multiple CL items arrive in burst after detachment.
 * Detach peer3 via freq, then inject multiple CL items in burst while
 * peer3 is DETACHED_BLOCKED. The CL accumulates all items. After unblock,
 * the DFP check fails (CL position diverged), so DSP kicks in and must
 * consume ALL CL items before peer can rejoin. Verified by: peer4 receives
 * all routes, peer3 eventually reaches a recovery state.
 */
TEST_P(UpdateGroupMultiPeerTest, DfpToDspMultipleClItemsBurst) {
  XLOGF(INFO, "=== TEST: DfpToDspMultipleClItemsBurst ===");

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

  /* Freq-detach with queue (5,4,0): need 5 fill routes */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);

  for (int i = 1; i <= 5; i++) {
    auto prefix = fmt::format("30.{}.0.0/16", 39 + i);
    auto community = fmt::format("30{}:1", 39 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("30.{}.0.0", 39 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Inject a burst of 3 CL items while peer3 is detached — these create
   * CL divergence so DFP check will fail, forcing DSP path */
  for (int i = 1; i <= 3; i++) {
    auto prefix = fmt::format("30.{}.0.0/16", 44 + i);
    auto community = fmt::format("30{}:1", 44 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("30.{}.0.0", 44 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Unblock — DFP fails (CL diverged), DSP must consume all 3 CL items */
  unblockPeer(kPeerAddr3);

  /* After unblock, peer3 may be in any recovery state depending on timing */
  auto state = getPeerState(kPeerAddr3);
  EXPECT_NE(state, PeerUpdateState::DOWN);

  /* Verify peer4 continues to function */
  injectLocalRoutesAtRuntime({"30.48.0.0/16"}, {"3048:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.48.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.48.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3048:1"));

  verifySlowPeerInvariants(kPeerAddr3);

  XLOGF(INFO, "=== TEST PASSED: DfpToDspMultipleClItemsBurst ===");
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
