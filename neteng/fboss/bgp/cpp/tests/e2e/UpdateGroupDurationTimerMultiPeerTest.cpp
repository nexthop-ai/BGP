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

/* E2E test: Duration timer fires for 2 peers simultaneously.
 * Prefix range: 26.1-26.4/16.
 *
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Duration timer fires for 2 peers simultaneously.
 * 3 peers (peer3, peer4, peer5). Block peer3 and peer4, set 1ms duration
 * threshold on both. Both should detach independently. peer5 continues. */
TEST_P(UpdateGroupMultiPeerTest, DurationTimerTwoPeersDetach) {
  XLOG(INFO, "=== TEST: DurationTimerTwoPeersDetach ===");

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

  /* Set aggressive 1ms duration threshold on both peer3 and peer4 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(1),
      100,
      std::chrono::milliseconds(60000));
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(1),
      100,
      std::chrono::milliseconds(60000));

  /* Block both peer3 and peer4 */
  blockPeer(kPeerAddr3);
  blockPeer(kPeerAddr4);

  /* Fill queue — each route goes to peer5 only (peer3/peer4 blocked) */
  injectLocalRoutesAtRuntime({"26.1.0.0/16"}, {"2601:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.1.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "2601:1"));
  injectLocalRoutesAtRuntime({"26.2.0.0/16"}, {"2602:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.2.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "2602:1"));
  injectLocalRoutesAtRuntime({"26.3.0.0/16"}, {"2603:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.3.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "2603:1"));

  /* Both peer3 and peer4 should become blocked and then detached */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));

  /* 1ms duration threshold should fire quickly — both detach */
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerDetached(kPeerAddr4));

  /* peer5 continues normally */
  injectLocalRoutesAtRuntime({"26.4.0.0/16"}, {"2604:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.4.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "2604:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));
  verifySlowPeerInvariants(kPeerAddr5);

  XLOG(INFO, "=== TEST PASSED: DurationTimerTwoPeersDetach ===");
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
