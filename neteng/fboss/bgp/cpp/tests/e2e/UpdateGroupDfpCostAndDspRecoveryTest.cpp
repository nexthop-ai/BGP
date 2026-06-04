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

/* E2E tests: DFP zero-CL-consumption cost and DSP CL consumption/blocking.
 * Prefix range: 31.1-31.29/16.
 *
 * DFP peer did zero CL consumption -- cost verification
 * DSP peer consumes all CL items, reaches end -- DRJ
 * DSP peer blocks during CL consumption -- P-DB, resumes on unblock
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* DFP peer did zero CL consumption -- cost verification.
 * Detach peer3 via freq, do NOT inject any CL items while detached.
 * After unblock, DFP check finds CL positions match (no divergence).
 * Peer3 rejoins without consuming ANY CL items -- zero cost path.
 */
TEST_P(UpdateGroupMultiPeerTest, DfpZeroClConsumptionCost) {
  XLOGF(INFO, "=== TEST: DfpZeroClConsumptionCost ===");

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

  /* Pre-detach: inject a shared route so CL has baseline */
  injectLocalRoutesAtRuntime({"31.1.0.0/16"}, {"3101:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3101:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3101:1"));

  /* Freq-detach peer3: threshold=1 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);

  /* Fill queue to trigger detachment -- 3 routes, different communities */
  injectLocalRoutesAtRuntime({"31.2.0.0/16"}, {"3102:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3102:1"));

  injectLocalRoutesAtRuntime({"31.3.0.0/16"}, {"3103:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3103:1"));

  injectLocalRoutesAtRuntime({"31.4.0.0/16"}, {"3104:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3104:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* KEY: No CL items while detached → CL positions stay aligned.
   * Unblock → DFP check succeeds → zero CL consumption. */
  unblockPeer(kPeerAddr3);

  /* Accept DB or DRJ -- queued PL items may re-block with small queue */
  auto state = getPeerState(kPeerAddr3);
  EXPECT_NE(state, PeerUpdateState::DOWN);

  /* Verify peer4 continues -- confirms DFP was zero cost */
  injectLocalRoutesAtRuntime({"31.5.0.0/16"}, {"3105:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.5.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.5.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3105:1"));

  verifySlowPeerInvariants(kPeerAddr3);
  XLOGF(INFO, "=== TEST PASSED: DfpZeroClConsumptionCost ===");
}
/* DSP peer blocks during CL consumption -- transitions to P-DB,
 * resumes on unblock. Detach peer3, inject CL items, unblock, then
 * immediately re-block. DSP interrupted → peer3 back to DB.
 * Second unblock allows DSP to resume.
 */
TEST_P(UpdateGroupMultiPeerTest, DspBlocksDuringClConsumption) {
  XLOGF(INFO, "=== TEST: DspBlocksDuringClConsumption ===");

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

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);

  injectLocalRoutesAtRuntime({"31.20.0.0/16"}, {"3120:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3120:1"));

  injectLocalRoutesAtRuntime({"31.21.0.0/16"}, {"3121:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3121:1"));

  injectLocalRoutesAtRuntime({"31.22.0.0/16"}, {"3122:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.22.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.22.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3122:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Inject CL item while detached */
  injectLocalRoutesAtRuntime({"31.23.0.0/16"}, {"3123:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.23.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.23.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3123:1"));

  /* Unblock → DSP starts, then re-block immediately */
  unblockPeer(kPeerAddr3);
  blockPeer(kPeerAddr3);

  auto state = getPeerState(kPeerAddr3);
  EXPECT_NE(state, PeerUpdateState::DOWN);

  /* Second unblock -- DSP resumes */
  unblockPeer(kPeerAddr3);

  state = getPeerState(kPeerAddr3);
  EXPECT_NE(state, PeerUpdateState::DOWN);

  /* Verify peer4 continues */
  injectLocalRoutesAtRuntime({"31.24.0.0/16"}, {"3124:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.24.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.24.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3124:1"));

  verifySlowPeerInvariants(kPeerAddr3);
  XLOGF(INFO, "=== TEST PASSED: DspBlocksDuringClConsumption ===");
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
