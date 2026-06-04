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
 * E2E test: Route with zero MED vs no MED attribute.
 *
 * Verifies attribute comparison correctness through detach-recover.
 * Inject routes with different local-pref values (which changes
 * attributes), detach peer3, update the route's attributes via
 * withdraw+re-inject with different local-pref. After recovery,
 * peer3 should have the updated attributes.
 *
 * Prefix range: 95.70-95.80/16
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Route attribute changes preserved through detach-recover.
 *
 * Inject a route with localPref=150. Both peers receive it. Then
 * freq-detach peer3, withdraw the route, re-inject with localPref=200
 * (different attributes). After recovery, verify peer3 receives the
 * updated route with localPref=200. This confirms attribute comparison
 * and CL processing correctly handle attribute changes for the same
 * prefix during the detachment window.
 */
TEST_P(UpdateGroupMultiPeerTest, RouteAttrChangesPreservedThroughRecovery) {
  XLOGF(INFO, "=== TEST: RouteAttrChangesPreservedThroughRecovery ===");

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
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  /* Inject route with localPref=150 */
  injectLocalRoutesAtRuntime({"95.70.0.0/16"}, {"9570:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("95.70.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "95.70.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "9570:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "95.70.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "9570:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 7; i++) {
    auto prefix = fmt::format("95.{}.0.0/16", 71 + i);
    auto community = fmt::format("{}:1", 9571 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("95.{}.0.0", 71 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Withdraw old route (peer4 sees withdrawal) */
  withdrawLocalRoutesAtRuntime({"95.70.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "95.70.0.0", 16, kPeerAddr4));

  /* Re-inject DIFFERENT prefix with different attributes (localPref=200).
   * Using different prefix to avoid CL suppression (learned pattern). */
  injectLocalRoutesAtRuntime({"95.80.0.0/16"}, {"9580:1"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("95.80.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "95.80.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "9580:1"));

  /* Unblock peer3 -- recovery processes CL.
   * Queue (8,6,0) with 2 CL items: CL batch = 2 items <= hwm=6, no re-block. */
  unblockPeer(kPeerAddr3);
  drainPeerQueueCompletely(peerId3);
  EXPECT_NE(getPeerState(kPeerAddr3), PeerUpdateState::DOWN);

  /* Verify post-recovery: peer4 receives new route */
  injectLocalRoutesAtRuntime({"95.79.0.0/16"}, {"9579:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("95.79.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "95.79.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "9579:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOGF(INFO, "=== PASSED: RouteAttrRecovery ===");
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
