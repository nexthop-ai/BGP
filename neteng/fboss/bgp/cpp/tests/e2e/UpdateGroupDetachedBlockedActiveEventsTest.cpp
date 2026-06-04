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

/* E2E tests: P-DB (DETACHED_BLOCKED) x active event coverage.
 * Prefix range: 26.x.0.0/16. All tests use freq-detach pattern:
 * 2 peers → JOINED_RUNNING → block+fill → DETACHED_BLOCKED → trigger event.
 *
 * Tests: E-UNBLOCK (resume after unblock), E-SLOW-FREQ (no double),
 * E-PL-DRAIN (N/A blocked), E-POLICY-CHG (simulate),
 * E-MRAI-FIRE (group processes, peer stays blocked)
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* P-DB x E-UNBLOCK -- unblock detached+blocked peer → recovery */
TEST_P(UpdateGroupMultiPeerTest, DetachedBlocked_Unblock) {
  XLOG(INFO, "=== TEST: DetachedBlocked_Unblock ===");
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  /* Large queue for fast peer, initialize infrastructure */
  setupSlowPeerComponents(10, 8, 0);
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr4);
  /* Small queue for slow peer -- fills naturally without blockPeer */
  setDefaultQueueSizes(3, 2, 0);
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Detach peer3 via freq threshold (1 block = detach) */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  /* Inject 3 routes -- peer3's small queue fills naturally */
  injectLocalRoutesAtRuntime({"26.1.0.0/16"}, {"2601:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2601:1"));
  injectLocalRoutesAtRuntime({"26.2.0.0/16"}, {"2602:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2602:1"));
  injectLocalRoutesAtRuntime({"26.3.0.0/16"}, {"2603:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2603:1"));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Drain peer3's queue -- peer recovers via DFP acceptance to JOINED_RUNNING.
   * With empty CL (no routes injected while detached), acceptance is immediate.
   */
  drainPeerQueueCompletely(peerId3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));

  /* Both peers operational after recovery */
  injectLocalRoutesAtRuntime({"26.4.0.0/16"}, {"2604:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2604:1"));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: DetachedBlocked_Unblock ===");
}
/* P-DB x E-PL-DRAIN -- N/A: blocked peer can't drain PL */
TEST_P(UpdateGroupMultiPeerTest, DetachedBlocked_PlDrainNoop) {
  XLOG(INFO, "=== TEST: DetachedBlocked_PlDrainNoop ===");
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  /* Large queue for fast peer, initialize infrastructure */
  setupSlowPeerComponents(10, 8, 0);
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr4);
  /* Small queue for slow peer -- fills naturally without blockPeer */
  setDefaultQueueSizes(3, 2, 0);
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  /* Inject 3 routes -- peer3's small queue fills naturally */
  injectLocalRoutesAtRuntime({"26.20.0.0/16"}, {"2620:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2620:1"));
  injectLocalRoutesAtRuntime({"26.21.0.0/16"}, {"2621:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2621:1"));
  injectLocalRoutesAtRuntime({"26.22.0.0/16"}, {"2622:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.22.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.22.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2622:1"));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* N/A: PL drain can't happen while blocked. Route goes to CL for peer3. */
  injectLocalRoutesAtRuntime({"26.23.0.0/16"}, {"2623:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.23.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.23.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2623:1"));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerQueueBlocked(peerId3));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: DetachedBlocked_PlDrainNoop ===");
}

/* P-DB x E-POLICY-CHG -- simulate policy change via withdraw+re-inject */
TEST_P(UpdateGroupMultiPeerTest, DetachedBlocked_PolicyChange) {
  XLOG(INFO, "=== TEST: DetachedBlocked_PolicyChange ===");
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  /* Large queue for fast peer, initialize infrastructure */
  setupSlowPeerComponents(10, 8, 0);
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr4);
  /* Small queue for slow peer -- fills naturally without blockPeer */
  setDefaultQueueSizes(3, 2, 0);
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Inject a shared route before detachment */
  injectLocalRoutesAtRuntime({"26.30.0.0/16"}, {"2630:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.30.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2630:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2630:1"));

  /* Detach peer3 via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  /* Inject 3 routes -- peer3's small queue fills naturally */
  injectLocalRoutesAtRuntime({"26.31.0.0/16"}, {"2631:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2631:1"));
  injectLocalRoutesAtRuntime({"26.32.0.0/16"}, {"2632:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.32.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.32.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2632:1"));
  injectLocalRoutesAtRuntime({"26.33.0.0/16"}, {"2633:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.33.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.33.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2633:1"));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Simulate policy change: withdraw old route, inject different prefix */
  withdrawLocalRoutesAtRuntime({"26.30.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "26.30.0.0", 16, kPeerAddr4));
  injectLocalRoutesAtRuntime({"26.34.0.0/16"}, {"2634:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.34.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.34.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2634:1"));

  /* Peer3 stays DB -- CL accumulates policy changes */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: DetachedBlocked_PolicyChange ===");
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
