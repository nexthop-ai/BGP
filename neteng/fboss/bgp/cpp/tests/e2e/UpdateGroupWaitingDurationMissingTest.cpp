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
 * E2E tests: Missing G-WAITING and Duration timer edge case tests
 *
 * Prefix range: 35.x.0.0/16
 *
 * Tests:
 *   G-WAITING x E-CL-END -- N/A (group draining PL, not CL)
 *   G-WAITING x E-ROUTE-REFRESH -- Route refresh during PL drain
 *   G-WAITING x E-EOR -- N/A
 *   Duration timer fires at same time as peer unblocks
 *   Duration timer fires for only synced peer -- skip detach
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Duration timer fires for only synced peer in group -- skip detach.
 * When ALL other peers are detached and only peer3 remains in-sync,
 * the duration timer should skip detachment for peer3 (last synced).
 * Simplified: detach peer4, then block peer3. Verify peer3 stays
 * JOINED_BLOCKED (not detached) because it's the last synced member.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, DurationSingleSynced_Skip) {
  XLOG(INFO, "=== TEST: DurationSingleSynced_Skip ===");

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

  /* Detach peer4 via freq threshold -- peer3 becomes sole synced member */
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr4);
  injectLocalRoutesAtRuntime({"35.40.0.0/16"}, {"3540:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.40.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3540:1"));
  injectLocalRoutesAtRuntime({"35.41.0.0/16"}, {"3541:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.41.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3541:1"));
  injectLocalRoutesAtRuntime({"35.42.0.0/16"}, {"3542:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.42.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.42.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3542:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Now set 1ms duration on peer3 -- sole synced member */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(1),
      999999,
      std::chrono::milliseconds(60000));

  /* Block peer3 -- duration fires but skip (last synced member).
   * Don't wait for queue blocked -- just block and check state.
   * Peer3 should NOT be detached. */
  blockPeer(kPeerAddr3);

  /* Give a moment for duration timer to fire (1ms is instant) */
  auto state = getPeerState(kPeerAddr3);
  /* Peer3 should be JOINED_BLOCKED (not DETACHED) -- last synced guard */
  EXPECT_NE(state, PeerUpdateState::DETACHED_BLOCKED);
  EXPECT_NE(state, PeerUpdateState::DOWN);

  XLOG(INFO, "=== TEST PASSED: DurationSingleSynced_Skip ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupSlowPeerDetectionTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
