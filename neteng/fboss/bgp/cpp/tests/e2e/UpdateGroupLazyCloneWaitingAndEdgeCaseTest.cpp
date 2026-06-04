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
 * E2E tests: Lazy Clone edge cases — WAITING state ordering,
 * IDLE no-clone, policy re-eval, multi-AF, and acceptance gap.
 *
 * Prefix range: 13.70-13.99/16
 * Fixture: UpdateGroupLazyCloneTest
 *
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Clone during G-WAITING (mid PL drain).
 * Detach peer3, then inject a route that updates a shared prefix.
 * The clone fires during PL processing (WAITING state). Verify
 * peer4 receives the updated route and invariants hold.
 */
TEST_P(UpdateGroupLazyCloneTest, CloneDuringWaitingState) {
  XLOG(INFO, "=== TEST: CloneDuringWaitingState ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Inject shared route — both peers receive */
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

  /* Inject a second shared route */
  injectLocalRoutesAtRuntime({"13.71.0.0/16"}, {"1371:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.71.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.71.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1371:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.71.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1371:1"));

  /* Detach peer3 via freq threshold=1 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
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
  injectLocalRoutesAtRuntime({"13.74.0.0/16"}, {"1374:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.74.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.74.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1374:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Update BOTH shared routes — clone fires for each during WAITING */
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

  injectLocalRoutesAtRuntime({"13.71.0.0/16"}, {"1371:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.71.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.71.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1371:99"));

  verifySlowPeerInvariants(kPeerAddr3);
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  XLOG(INFO, "=== TEST PASSED: CloneDuringWaitingState ===");
}

/* Clone when group has multiple address families.
 * Both IPv4 and IPv6 shared routes exist. After detaching peer3,
 * update the v4 shared route — clone fires for v4. Then update
 * the v6 shared route — clone fires for v6. Both AFs work.
 */
TEST_P(UpdateGroupLazyCloneTest, CloneMultiAddressFamily) {
  XLOG(INFO, "=== TEST: CloneMultiAddressFamily ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(5, 4, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Shared v4 route */
  injectLocalRoutesAtRuntime({"13.86.0.0/16"}, {"1386:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.86.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.86.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1386:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.86.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1386:1"));

  /* Shared v6 route */
  injectLocalRoutesAtRuntime({"2001:db8:1387::/48"}, {"1387:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("2001:db8:1387::/48")));
  EXPECT_TRUE(verifyRouteAdd(
      "v6",
      "2001:db8:1387::",
      48,
      kPeerAddr3,
      kNextHopV6_3.str(),
      "4200000001",
      "1387:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v6",
      "2001:db8:1387::",
      48,
      kPeerAddr4,
      kNextHopV6_4.str(),
      "4200000001",
      "1387:1"));

  /* Detach peer3 via freq threshold=1 with queue (5,4,0) */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 1; i <= 5; i++) {
    auto prefix = fmt::format("13.{}.0.0/16", 86 + i);
    auto community = fmt::format("13{}:1", 86 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("13.{}.0.0", 86 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Update shared v4 route — clone fires for v4 */
  injectLocalRoutesAtRuntime({"13.86.0.0/16"}, {"1386:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.86.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.86.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1386:99"));

  /* Update shared v6 route — clone fires for v6 */
  injectLocalRoutesAtRuntime({"2001:db8:1387::/48"}, {"1387:99"}, 200);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("2001:db8:1387::/48")));
  EXPECT_TRUE(verifyRouteAdd(
      "v6",
      "2001:db8:1387::",
      48,
      kPeerAddr4,
      kNextHopV6_4.str(),
      "4200000001",
      "1387:99"));

  verifySlowPeerInvariants(kPeerAddr3);
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  XLOG(INFO, "=== TEST PASSED: CloneMultiAddressFamily ===");
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
