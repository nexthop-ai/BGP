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
 * E2E tests: Lazy Clone PL Consistency and Null Queue Edge Cases
 * Tests clone with PL having the same prefix (radix consistency)
 * and clone gracefully skipping a DOWN peer with null queue.
 *
 * Prefix range: 13.70-13.79/16
 * Fixture: UpdateGroupLazyCloneTest
 *
 * Tests: CloneWithPLSamePrefix, CloneWithNullBoundedQueue
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Clone with group PL having the same prefix -- PL entry vs
 * radix entry consistency. After detaching peer3, update a shared
 * route that was already delivered via PL. Clone (Case 4) fires for
 * the radix entry. A second update hits Case 1 (per-peer exists).
 */
TEST_P(UpdateGroupLazyCloneTest, CloneWithPLSamePrefix) {
  XLOG(INFO, "=== TEST: CloneWithPLSamePrefix ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr4);
  setDefaultQueueSizes(3, 2, 0);
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Shared route -- both peers receive */
  injectLocalRoutesAtRuntime({"13.70.0.0/16"}, {"1370:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.70.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.70.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1370:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.70.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1370:1"));

  /* Detach peer3 via freq threshold=1 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Inject 3 fill routes — peer3's small queue (3,2,0) fills naturally */
  injectLocalRoutesAtRuntime({"13.71.0.0/16"}, {"1371:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.71.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.71.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1371:1"));
  injectLocalRoutesAtRuntime({"13.72.0.0/16"}, {"1372:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.72.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.72.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1372:1"));
  injectLocalRoutesAtRuntime({"13.73.0.0/16"}, {"1373:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.73.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.73.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1373:1"));

  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* First update -- Case 4 clone fires for the radix entry */
  injectLocalRoutesAtRuntime({"13.70.0.0/16"}, {"1370:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.70.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.70.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1370:99"));

  verifySlowPeerInvariants(kPeerAddr3);
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Second update -- Case 1, per-peer entry exists from first clone */
  injectLocalRoutesAtRuntime({"13.70.0.0/16"}, {"1370:88"}, 250);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.70.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.70.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1370:88"));

  verifySlowPeerInvariants(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  XLOG(INFO, "=== TEST PASSED: CloneWithPLSamePrefix ===");
}

/*
 * Clone fires but peer's bounded queue is null -- graceful
 * skip. Detach peer3, bring it DOWN (queue destroyed), then update
 * a shared route. Group handles absent peer without crash.
 */
TEST_P(UpdateGroupLazyCloneTest, CloneWithNullBoundedQueue) {
  XLOG(INFO, "=== TEST: CloneWithNullBoundedQueue ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr4);
  setDefaultQueueSizes(3, 2, 0);
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Shared route */
  injectLocalRoutesAtRuntime({"13.75.0.0/16"}, {"1375:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.75.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.75.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1375:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.75.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1375:1"));

  /* Detach peer3 via freq threshold=1 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Inject 3 fill routes — peer3's small queue (3,2,0) fills naturally */
  injectLocalRoutesAtRuntime({"13.76.0.0/16"}, {"1376:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.76.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.76.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1376:1"));
  injectLocalRoutesAtRuntime({"13.77.0.0/16"}, {"1377:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.77.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.77.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1377:1"));
  injectLocalRoutesAtRuntime({"13.78.0.0/16"}, {"1378:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.78.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.78.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1378:1"));

  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Bring detached peer3 DOWN -- bounded queue destroyed */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Update shared route -- peer3 is DOWN (null queue), no crash */
  injectLocalRoutesAtRuntime({"13.75.0.0/16"}, {"1375:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.75.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.75.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1375:99"));

  /* Verify group still works with peer4 only */
  injectLocalRoutesAtRuntime({"13.79.0.0/16"}, {"1379:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.79.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.79.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1379:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: CloneWithNullBoundedQueue ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupLazyCloneTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
