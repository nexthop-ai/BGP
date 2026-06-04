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

/* E2E tests: Duration timer edge cases.
 * Prefix range: 25.40-25.49/16.
 *
 * Duration timer fires but peer went DOWN — verify no crash
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Duration timer fires but peer went DOWN before callback.
 * Set aggressive duration threshold (1ms). Block peer3, fill queue
 * to trigger timer start. Immediately bring peer3 DOWN before timer
 * fires (or concurrently). Verify no crash — the timer callback must
 * handle a dead/absent peer gracefully. Peer4 continues normally. */
TEST_P(UpdateGroupMultiPeerTest, DurationTimer_PeerDownBeforeCallback) {
  XLOG(INFO, "=== TEST: DurationTimer_PeerDownBeforeCallback ===");

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

  /* Set very aggressive duration threshold (1ms) — will fire quickly */
  setSlowPeerThresholds(
      kPeerAddr3, std::chrono::milliseconds(1), 100, std::chrono::seconds(60));

  /* Block peer3 and fill queue — starts duration timer */
  blockPeer(kPeerAddr3);

  injectLocalRoutesAtRuntime({"25.40.0.0/16"}, {"2540:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("25.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "25.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2540:1"));

  injectLocalRoutesAtRuntime({"25.41.0.0/16"}, {"2541:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("25.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "25.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2541:1"));

  injectLocalRoutesAtRuntime({"25.42.0.0/16"}, {"2542:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("25.42.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "25.42.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2542:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Immediately bring peer3 DOWN — timer callback (if pending) must
   * handle absent peer gracefully without crash */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Peer4 continues operating normally after peer3 goes DOWN */
  injectLocalRoutesAtRuntime({"25.43.0.0/16"}, {"2543:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("25.43.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "25.43.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2543:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: DurationTimer_PeerDownBeforeCallback ===");
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
