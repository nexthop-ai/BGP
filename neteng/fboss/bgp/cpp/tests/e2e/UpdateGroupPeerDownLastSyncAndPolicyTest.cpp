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

/* E2E tests: Peer DOWN edge cases — last in-sync peer and policy re-eval.
 * Prefix range: 31.70-31.99/16
 *
 * Last in-sync peer DOWN while detached peers remain — 0 in-sync
 * Peer DOWN during policy re-eval (simulated) — cancel re-eval
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Peer DOWN: last in-sync peer, detached peers remain.
 * 2-peer group: freq-detach peer3 (now detached), then bring peer4
 * (the LAST in-sync peer) DOWN. Group should have 0 in-sync members.
 * Verify no crash and group handles the 0-in-sync state gracefully.
 */
TEST_P(UpdateGroupPeerDownTest, LastInSyncPeerDownDetachedRemain) {
  XLOG(INFO, "=== TEST: LastInSyncPeerDownDetachedRemain ===");

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

  /* Inject a shared route before detachment */
  injectLocalRoutesAtRuntime({"31.70.0.0/16"}, {"3170:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.70.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.70.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3170:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.70.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3170:1"));

  /* Freq-detach peer3: threshold=1 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"31.71.0.0/16"}, {"3171:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.71.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.71.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3171:1"));
  injectLocalRoutesAtRuntime({"31.72.0.0/16"}, {"3172:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.72.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.72.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3172:1"));
  injectLocalRoutesAtRuntime({"31.73.0.0/16"}, {"3173:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.73.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.73.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3173:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);

  /* Peer4 is the LAST in-sync peer. Bring it DOWN. */
  bringDownPeer(kPeerAddr4);
  EXPECT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DOWN));

  /* Group now has 0 in-sync members, only detached peer3 remains.
   * Inject a route — should not crash even with no in-sync peers. */
  injectLocalRoutesAtRuntime({"31.74.0.0/16"}, {"3174:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.74.0.0/16")));

  /* Verify peer3 is still detached (not affected by peer4 going DOWN) */
  auto state3 = getPeerState(kPeerAddr3);
  EXPECT_TRUE(
      state3 == PeerUpdateState::DETACHED_BLOCKED ||
      state3 == PeerUpdateState::DETACHED_READY_TO_JOIN);

  XLOG(INFO, "=== TEST PASSED: LastInSyncPeerDownDetachedRemain ===");
}

/*
 * Peer DOWN during policy re-eval for that peer — cancel re-eval.
 * Cannot use real setPolicyConfig (incompatible with update groups — learned
 * pattern). Instead simulate policy-like re-evaluation by withdrawing a route
 * and rapidly injecting a new one with different attributes while peer3 is
 * blocked. Then bring peer3 DOWN mid-processing. Verify peer4 continues
 * receiving routes normally and no crash occurs from stale re-eval state.
 */
TEST_P(UpdateGroupPeerDownTest, PeerDownDuringPolicyReeval) {
  XLOG(INFO, "=== TEST: PeerDownDuringPolicyReeval ===");

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

  /* Inject initial routes that both peers receive */
  injectLocalRoutesAtRuntime({"31.80.0.0/16"}, {"3180:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.80.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.80.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3180:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.80.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3180:1"));

  /* Block peer3 to simulate slow peer during policy re-eval */
  blockPeer(kPeerAddr3);

  /* Simulate policy re-eval: withdraw old route, inject new one */
  withdrawLocalRoutesAtRuntime({"31.80.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "31.80.0.0", 16, kPeerAddr4));

  /* Inject replacement route with different attributes (different prefix
   * to avoid CL suppression — learned pattern) */
  injectLocalRoutesAtRuntime({"31.81.0.0/16"}, {"3181:1"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.81.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.81.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3181:1"));

  /* Peer3 has CL items pending (withdrawal + new route). Bring it DOWN
   * mid-processing to simulate cancel of policy re-eval. */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Peer4 should continue working normally after peer3 goes DOWN */
  injectLocalRoutesAtRuntime({"31.82.0.0/16"}, {"3182:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.82.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.82.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3182:1"));

  /* Another route to confirm continued stability */
  injectLocalRoutesAtRuntime({"31.83.0.0/16"}, {"3183:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.83.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.83.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3183:1"));

  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: PeerDownDuringPolicyReeval ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupPeerDownTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
