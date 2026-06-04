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
 * E2E test for event sequence: E-DETACH then E-ROUTE-WD.
 * Detach a peer, then withdraw a route — withdrawal triggers
 * lazy clone for the detached peer's diverged view.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * E-DETACH then E-ROUTE-WD — withdrawal with lazy clone.
 * Setup: 2 peers in JOINED_RUNNING, inject a shared route, then detach
 * peer3 via freq threshold. After detachment, withdraw the shared route.
 * The withdrawal triggers a Case 4 lazy clone for peer3's diverged view
 * (preserving the route in peer3's per-peer entry). Peer4 receives the
 * withdrawal normally. Then inject a new route to confirm group continues.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachThenWithdraw_LazyClone) {
  XLOG(INFO, "=== TEST: DetachThenWithdraw_LazyClone ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Drain 2 EoRs per peer (v4+v6) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Inject a shared route BEFORE detachment — both peers receive it */
  injectLocalRoutesAtRuntime({"31.1.0.0/16"}, {"3110:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3110:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3110:1"));

  /* Freq-detach peer3: threshold=1 block cycle */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);

  /* Fill queue with 3 routes (different communities) to trigger block */
  injectLocalRoutesAtRuntime({"31.2.0.0/16"}, {"3120:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3120:1"));

  injectLocalRoutesAtRuntime({"31.3.0.0/16"}, {"3130:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3130:1"));

  injectLocalRoutesAtRuntime({"31.4.0.0/16"}, {"3140:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3140:1"));

  /* Peer3 should be DETACHED_BLOCKED (freq threshold=1) */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Now withdraw the shared route (31.1.0.0/16).
   * This triggers lazy clone (Case 4) for peer3's diverged view:
   * the shared RIB entry is cloned to peer3's per-peer entry before
   * the withdrawal is applied to the shared entry. */
  withdrawLocalRoutesAtRuntime({"31.1.0.0/16"});

  /* Peer4 receives the withdrawal normally */
  EXPECT_TRUE(verifyRouteWithdraw("v4", "31.1.0.0", 16, kPeerAddr4));

  /* Verify peer4 still running, peer3 still detached */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerStateAny(
      kPeerAddr3,
      {PeerUpdateState::DETACHED_BLOCKED,
       PeerUpdateState::DETACHED_READY_TO_JOIN}));

  /* Inject another route to confirm group continues working */
  injectLocalRoutesAtRuntime({"31.5.0.0/16"}, {"3150:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.5.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.5.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3150:1"));

  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: DetachThenWithdraw_LazyClone ===");
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
