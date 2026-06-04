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

/* E2E tests: Peer DOWN during active operations (clones, PL drain, CL).
 * Prefix range: 30.20-30.69/16
 *
 * Peer DOWN with accumulated lazy clones — per-peer entries removed
 * Peer DOWN mid-PL drain — partial drain cleanup
 * Peer DOWN during CL consumption — clean cancellation
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Peer DOWN with accumulated lazy clones.
 * Detach peer3, inject multiple routes post-detach (CL accumulates
 * lazy clone per-peer entries), then bring peer3 DOWN. Verify all
 * per-peer entries are cleaned up and peer4 continues normally.
 */
TEST_P(UpdateGroupPeerDownTest, PeerDownWithAccumulatedClones) {
  XLOG(INFO, "=== TEST: PeerDownWithAccumulatedClones ===");

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

  /* Inject a shared route before detachment (triggers Case 4 clone later) */
  injectLocalRoutesAtRuntime({"30.20.0.0/16"}, {"3020:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.20.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3020:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3020:1"));

  /* Freq-detach peer3 with queue (5,4,0): need 5 fill routes */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 1; i <= 5; i++) {
    auto prefix = fmt::format("30.{}.0.0/16", 20 + i);
    auto community = fmt::format("{}:1", 3020 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("30.{}.0.0", 20 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Inject more routes post-detachment — CL accumulates per-peer entries */
  for (int i = 1; i <= 5; i++) {
    auto prefix = fmt::format("30.{}.0.0/16", 30 + i);
    auto community = fmt::format("{}:1", 3030 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("30.{}.0.0", 30 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Bring DOWN peer3 — should clean up all per-peer entries */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 0);

  /* Peer4 still functional after cleanup */
  injectLocalRoutesAtRuntime({"30.40.0.0/16"}, {"3040:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3040:1"));

  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: PeerDownWithAccumulatedClones ===");
}

/*
 * Peer DOWN mid-PL drain.
 * Block peer3, inject routes to fill PL, then bring peer3 DOWN
 * while PL has pending messages for peer3. Verify partial PL
 * cleanup, no crash, peer4 continues.
 */
TEST_P(UpdateGroupPeerDownTest, PeerDownMidPlDrain) {
  XLOG(INFO, "=== TEST: PeerDownMidPlDrain ===");

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

  /* Set high threshold to prevent detachment */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      100,
      std::chrono::milliseconds(60000));

  /* Block peer3 and fill queue with 3 routes */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"30.50.0.0/16"}, {"3050:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.50.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.50.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3050:1"));
  injectLocalRoutesAtRuntime({"30.51.0.0/16"}, {"3051:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.51.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.51.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3051:1"));
  injectLocalRoutesAtRuntime({"30.52.0.0/16"}, {"3052:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.52.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.52.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3052:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* Bring DOWN peer3 while PL is pending (not drained for peer3) */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Peer4 still functional */
  injectLocalRoutesAtRuntime({"30.53.0.0/16"}, {"3053:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.53.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.53.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3053:1"));

  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: PeerDownMidPlDrain ===");
}

/*
 * Peer DOWN during CL consumption.
 * Block peer3 (group enters WAITING), inject more routes (go to CL),
 * then bring peer3 DOWN. CL processing should complete for peer4
 * once the blocker is removed.
 */
TEST_P(UpdateGroupPeerDownTest, PeerDownDuringClConsumption) {
  XLOG(INFO, "=== TEST: PeerDownDuringClConsumption ===");

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

  /* Set high threshold to prevent detachment */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      100,
      std::chrono::milliseconds(60000));

  /* Block peer3: need 3 routes with distinct communities to fill queue(3,2,0)
   * past hwm=2 */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"30.60.0.0/16"}, {"3060:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.60.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.60.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3060:1"));
  injectLocalRoutesAtRuntime({"30.61.0.0/16"}, {"3061:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.61.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.61.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3061:1"));
  injectLocalRoutesAtRuntime({"30.62.0.0/16"}, {"3062:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.62.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.62.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3062:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Inject CL items while peer3 is blocked */
  injectLocalRoutesAtRuntime({"30.63.0.0/16"}, {"3063:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.63.0.0/16")));

  /* Bring DOWN peer3 directly while blocked with pending CL */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Verify peer4 is still in-sync (no crash from cleanup) */
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: PeerDownDuringClConsumption ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupPeerDownTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
