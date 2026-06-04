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

/* E2E tests: Detach during READY state, PL clone with mixed ann+wd,
 * and detach with pending out-delay entries.
 * Prefix range: 55.1-55.36/16.
 *
 * Detach peer while group is in READY state
 * PL clone with mixed announcements and withdrawals
 * Detachment with pending out-delay entries
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* PL clone with mixed announcements and withdrawals.
 * Inject a route, then withdraw it. While both ann+wd are in PL, trigger
 * detachment. The PL clone must handle nullptr attrs for withdrawals.
 */
TEST_P(UpdateGroupMultiPeerTest, PlCloneWithMixedAnnounceAndWithdraw) {
  XLOG(INFO, "=== TEST: PlCloneWithMixedAnnounceAndWithdraw ===");

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

  /* Inject a shared route that both peers receive */
  injectLocalRoutesAtRuntime({"55.20.0.0/16"}, {"5520:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("55.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "55.20.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "5520:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "55.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5520:1"));

  /* Set freq threshold, block peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);

  /* Inject a new route (announcement) */
  injectLocalRoutesAtRuntime({"55.21.0.0/16"}, {"5521:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("55.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "55.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5521:1"));

  /* Withdraw the shared route (withdrawal with nullptr attrs in PL) */
  withdrawLocalRoutesAtRuntime({"55.20.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "55.20.0.0", 16, kPeerAddr4));

  /* Inject one more to fill queue and trigger detach */
  injectLocalRoutesAtRuntime({"55.22.0.0/16"}, {"5522:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("55.22.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "55.22.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5522:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* PL clone should handle mixed ann+wd without crash */
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));

  /* Verify peer4 continues working after detach with mixed PL */
  injectLocalRoutesAtRuntime({"55.23.0.0/16"}, {"5523:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("55.23.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "55.23.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5523:1"));

  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: PlCloneWithMixedAnnounceAndWithdraw ===");
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
