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
 * E2E tests: Clone and Collapse missing tests
 *
 * Prefix range: 43.x.0.0/16
 *
 * Tests:
 *   Clone during G-WAITING (mid PL drain)
 *   Clone during G-IDLE (no activity)
 *   Clone after policy re-eval -- group entry changed
 *   Clone when group has multiple address families
 *   Clone followed immediately by acceptance
 *   Clone fires between acceptance steps
 *   Collapse -- no per-peer entries match, all retained
 *   Collapse -- per-peer entry same prefix different attrs
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Clone followed immediately by acceptance.
 * Detach, trigger clone, then unblock. Orphaned entry risk -- verify
 * acceptance handles it correctly.
 */
TEST_P(UpdateGroupMultiPeerTest, CloneThenAcceptance) {
  XLOG(INFO, "=== TEST: CloneThenAcceptance ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(10, 8, 0);

  /* Per-peer queues: large for fast peer, small for slow peer */
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr4);

  setDefaultQueueSizes(3, 2, 0);
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  injectLocalRoutesAtRuntime({"43.40.0.0/16"}, {"4340:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("43.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "43.40.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "4340:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "43.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "4340:1"));

  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Inject 3 fill routes — peer3's (3,2,0) queue fills naturally */
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("43.{}.0.0/16", 41 + i);
    auto community = fmt::format("{}:1", 4341 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("43.{}.0.0", 41 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Clone fires — withdraw shared route while peer3 is detached */
  withdrawLocalRoutesAtRuntime({"43.40.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "43.40.0.0", 16, kPeerAddr4));

  /* Drain peer3 to trigger acceptance */
  drainPeerQueueCompletely(peerId3);
  auto state = getPeerState(kPeerAddr3);
  EXPECT_NE(state, PeerUpdateState::DOWN);
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: CloneThenAcceptance ===");
}

/*
 * Clone fires between acceptance steps.
 * Test interaction of clone during recovery. After unblock, update a
 * shared route. Verify no crash.
 */
TEST_P(UpdateGroupMultiPeerTest, CloneBetweenAcceptanceSteps) {
  XLOG(INFO, "=== TEST: CloneBetweenAcceptanceSteps ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(10, 8, 0);

  /* Per-peer queues: large for fast peer, small for slow peer */
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr4);

  setDefaultQueueSizes(3, 2, 0);
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  injectLocalRoutesAtRuntime({"43.50.0.0/16"}, {"4350:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("43.50.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "43.50.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "4350:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "43.50.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "4350:1"));

  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Inject 3 fill routes — peer3's (3,2,0) queue fills naturally */
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("43.{}.0.0/16", 51 + i);
    auto community = fmt::format("{}:1", 4351 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("43.{}.0.0", 51 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Drain peer3 to start recovery/acceptance */
  drainPeerQueueCompletely(peerId3);

  /* Withdraw shared route during recovery — clone fires between steps */
  withdrawLocalRoutesAtRuntime({"43.50.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "43.50.0.0", 16, kPeerAddr4));

  /* No crash */
  auto state = getPeerState(kPeerAddr3);
  EXPECT_NE(state, PeerUpdateState::DOWN);
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: CloneBetweenAcceptanceSteps ===");
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
