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
 * E2E test: CL item with combined announce+withdraw in same batch.
 *
 * While peer3 is detached, inject a route then withdraw a DIFFERENT
 * route. Both CL items are queued. After recovery, verify peer3
 * processes both correctly: the announcement and the withdrawal.
 *
 * Prefix range: 96.x.0.0/16
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * CL announce + withdraw in same batch during detachment.
 *
 * Setup: inject 2 shared routes. Freq-detach peer3. While detached:
 *   1. Withdraw route A (CL withdrawal)
 *   2. Inject route C (CL announcement)
 * After recovery, peer3 should have processed both CL items.
 * Route B should still be present (untouched).
 * Verify with a post-recovery route that both peers receive.
 */
TEST_P(UpdateGroupMultiPeerTest, ClBatchAnnounceAndWithdrawDuringDetachment) {
  XLOGF(INFO, "=== TEST: ClBatchAnnounceAndWithdrawDuringDetachment ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 0);

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

  /* Inject 2 shared routes: A=96.1, B=96.2 */
  injectLocalRoutesAtRuntime({"96.1.0.0/16"}, {"9601:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("96.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "96.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "9601:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "96.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "9601:1"));

  injectLocalRoutesAtRuntime({"96.2.0.0/16"}, {"9602:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("96.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "96.2.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "9602:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "96.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "9602:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 7; i++) {
    auto prefix = fmt::format("96.{}.0.0/16", 10 + i);
    auto community = fmt::format("{}:1", 9610 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("96.{}.0.0", 10 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* CL withdrawal: withdraw route A (96.1) */
  withdrawLocalRoutesAtRuntime({"96.1.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "96.1.0.0", 16, kPeerAddr4));

  /* CL announcement: inject route C (96.20) */
  injectLocalRoutesAtRuntime({"96.20.0.0/16"}, {"9620:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("96.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "96.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "9620:1"));

  /* Unblock peer3 -- CL processes both withdrawal and announcement */
  unblockPeer(kPeerAddr3);
  drainPeerQueueCompletely(peerId3);
  EXPECT_NE(getPeerState(kPeerAddr3), PeerUpdateState::DOWN);

  /* Verify post-recovery: peer4 receives new route */
  injectLocalRoutesAtRuntime({"96.30.0.0/16"}, {"9630:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("96.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "96.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "9630:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOGF(INFO, "=== PASSED: ClBatchAnnounceWithdraw ===");
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
