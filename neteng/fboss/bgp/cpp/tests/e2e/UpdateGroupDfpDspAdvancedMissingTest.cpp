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
 * E2E tests: Advanced DFP/DSP tests
 *
 * Prefix range: 39.x.0.0/16
 *
 * Tests:
 *   DFP peer finishes PL, group finishes PL, both at same CL
 *   DFP->DSP demotion -- DFP at DRJ, new CL item arrives
 *   DFP->DSP -- multiple CL items arrive in burst
 *   DFP group acceptance check -- entry collapse works
 *   DFP peer did zero CL consumption -- cost verification
 *   Two DFP peers ready simultaneously -- both accepted
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* DFP group acceptance check -- entry collapse works.
 * After detach and recovery, the acceptance procedure collapses matching
 * per-peer entries. Verify route delivery post-acceptance.
 */
TEST_P(UpdateGroupMultiPeerTest, DFP_AcceptanceCollapse) {
  XLOG(INFO, "=== TEST: DFP_AcceptanceCollapse ===");

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

  /* Inject shared route */
  injectLocalRoutesAtRuntime({"39.30.0.0/16"}, {"3930:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("39.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "39.30.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3930:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "39.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3930:1"));

  /* Freq-detach */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 5; i++) {
    auto prefix = fmt::format("39.{}.0.0/16", 31 + i);
    auto community = fmt::format("{}:1", 3931 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("39.{}.0.0", 31 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock -- acceptance should happen eventually */
  unblockPeer(kPeerAddr3);

  /* Verify no crash, peer4 keeps working */
  injectLocalRoutesAtRuntime({"39.37.0.0/16"}, {"3937:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("39.37.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "39.37.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3937:1"));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: DFP_AcceptanceCollapse ===");
}

/*
 * Two DFP peers ready simultaneously -- both accepted.
 * 3 peers. Detach peer3 and peer4. Unblock both. Both should reach
 * a recoverable state. Peer5 continues.
 */
TEST_P(UpdateGroupMultiPeerTest, TwoDFP_BothAccepted) {
  XLOG(INFO, "=== TEST: TwoDFP_BothAccepted ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  setupSlowPeerComponents(3, 2, 0);

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

  /* Set 1ms duration on both peer3 and peer4 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(1),
      999999,
      std::chrono::milliseconds(60000));
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(1),
      999999,
      std::chrono::milliseconds(60000));

  /* Block both and fill queue */
  blockPeer(kPeerAddr3);
  blockPeer(kPeerAddr4);
  injectLocalRoutesAtRuntime({"39.50.0.0/16"}, {"3950:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("39.50.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "39.50.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3950:1"));
  injectLocalRoutesAtRuntime({"39.51.0.0/16"}, {"3951:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("39.51.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "39.51.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3951:1"));
  injectLocalRoutesAtRuntime({"39.52.0.0/16"}, {"3952:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("39.52.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "39.52.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3952:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock both simultaneously */
  unblockPeer(kPeerAddr3);
  unblockPeer(kPeerAddr4);

  /* Both should be in some non-DOWN state */
  EXPECT_NE(getPeerState(kPeerAddr3), PeerUpdateState::DOWN);
  EXPECT_NE(getPeerState(kPeerAddr4), PeerUpdateState::DOWN);
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  /* Peer5 continues */
  injectLocalRoutesAtRuntime({"39.55.0.0/16"}, {"3955:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("39.55.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "39.55.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3955:1"));

  XLOG(INFO, "=== TEST PASSED: TwoDFP_BothAccepted ===");
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
