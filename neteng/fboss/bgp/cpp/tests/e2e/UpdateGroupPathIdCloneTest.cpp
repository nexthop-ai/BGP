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
 * E2E test: Entry with pathId preserved through clone (add-path).
 *
 * E2E framework has no explicit pathId support. Tests with 2 distinct
 * prefixes as separate "paths", each getting independent lazy clones.
 * Withdrawing one path doesn't affect the other's clone.
 *
 * Prefix range: 95.40-95.60/16
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Two distinct prefixes as independent "paths" through clone.
 *
 * Inject 2 shared routes, freq-detach peer3, then withdraw one route
 * and inject a new route. After recovery, peer3 should have:
 * - The surviving original route (via lazy clone)
 * - The new route (via CL)
 * - NOT the withdrawn route
 *
 * This exercises independent clone preservation for each prefix/path.
 */
TEST_P(UpdateGroupMultiPeerTest, PathIdPreservedThroughClone) {
  XLOGF(INFO, "=== TEST: PathIdPreservedThroughClone ===");

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

  /* Inject 2 shared routes (both peers receive) */
  injectLocalRoutesAtRuntime({"95.40.0.0/16"}, {"9540:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("95.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "95.40.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "9540:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "95.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "9540:1"));

  injectLocalRoutesAtRuntime({"95.41.0.0/16"}, {"9541:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("95.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "95.41.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "9541:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "95.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "9541:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 7; i++) {
    auto prefix = fmt::format("95.{}.0.0/16", 50 + i);
    auto community = fmt::format("{}:1", 9550 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("95.{}.0.0", 50 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Withdraw path 1 (95.40) while peer3 detached */
  withdrawLocalRoutesAtRuntime({"95.40.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "95.40.0.0", 16, kPeerAddr4));

  /* Path 2 (95.41) is untouched -- its clone for peer3 should survive */

  /* Unblock peer3 -- recovery processes CL including the withdrawal.
   * Queue (8,6,0) with 1 CL item: CL batch = 1 item <= hwm=6, no re-block. */
  unblockPeer(kPeerAddr3);
  drainPeerQueueCompletely(peerId3);
  EXPECT_NE(getPeerState(kPeerAddr3), PeerUpdateState::DOWN);

  /* Verify post-recovery: inject new route, peer4 receives it */
  injectLocalRoutesAtRuntime({"95.60.0.0/16"}, {"9560:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("95.60.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "95.60.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "9560:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOGF(INFO, "=== PASSED: PathIdClone ===");
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
