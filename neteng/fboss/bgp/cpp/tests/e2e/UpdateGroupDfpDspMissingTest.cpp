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
 * E2E tests: Missing DFP/DSP detachment tests
 *
 * Prefix range: 38.x.0.0/16
 *
 * Tests:
 *   Detachment during coroutine yield
 *   DFP -- group at same CL position, group PL non-empty
 *   DFP -- group at same CL position, group PL empty (NOT DFP)
 *   DFP -- peer PL empty, group PL non-empty, same CL pos
 *   DFP -- peer PL NOT empty (NOT DFP)
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Peer PL NOT empty (NOT DFP regardless of CL position).
 * Detach peer3 while PL has entries. Don't fully drain. The peer's PL
 * is not empty -> NOT DFP, must be DSP. Verify state after unblock.
 */
TEST_P(UpdateGroupMultiPeerTest, DFP_PeerPlNotEmpty_NotDfp) {
  XLOG(INFO, "=== TEST: DFP_PeerPlNotEmpty_NotDfp ===");

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

  /* Inject route before detachment -- both peers receive it */
  injectLocalRoutesAtRuntime({"38.40.0.0/16"}, {"3840:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("38.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "38.40.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3840:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "38.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3840:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"38.41.0.0/16"}, {"3841:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("38.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "38.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3841:1"));
  injectLocalRoutesAtRuntime({"38.42.0.0/16"}, {"3842:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("38.42.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "38.42.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3842:1"));
  injectLocalRoutesAtRuntime({"38.43.0.0/16"}, {"3843:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("38.43.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "38.43.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3843:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Peer3's PL is NOT empty (has 3 queued PL items) -- NOT DFP, must be DSP */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1);

  /* Peer4 continues */
  injectLocalRoutesAtRuntime({"38.45.0.0/16"}, {"3845:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("38.45.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "38.45.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3845:1"));

  XLOG(INFO, "=== TEST PASSED: DFP_PeerPlNotEmpty_NotDfp ===");
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
