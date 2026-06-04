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

/* E2E tests: Duration timer edge cases during concurrent activity.
 * Prefix range: 30.1-30.23/16.
 *
 * Duration timer fires during policy re-evaluation
 * Duration timer fires during group state transition
 * Duration threshold very large — never fires
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Duration timer fires during policy re-evaluation.
 * Simulate policy re-evaluation (withdraw + re-inject different prefix)
 * while a duration timer is active. Timer fires concurrently — verify
 * detachment happens cleanly and peer4 receives the policy-change routes. */
TEST_P(UpdateGroupMultiPeerTest, DurationTimer_DuringPolicyReeval) {
  XLOG(INFO, "=== TEST: DurationTimer_DuringPolicyReeval ===");

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

  /* Inject a shared route before blocking */
  injectLocalRoutesAtRuntime({"30.1.0.0/16"}, {"3001:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3001:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3001:1"));

  /* Set aggressive 1ms duration threshold */
  setSlowPeerThresholds(
      kPeerAddr3, std::chrono::milliseconds(1), 100, std::chrono::seconds(60));

  /* Block peer3 and simulate policy re-evaluation: withdraw old prefix */
  blockPeer(kPeerAddr3);
  withdrawLocalRoutesAtRuntime({"30.1.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "30.1.0.0", 16, kPeerAddr4));

  /* Inject new prefix (different attributes — simulates re-evaluation) */
  injectLocalRoutesAtRuntime({"30.2.0.0/16"}, {"3002:1"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3002:1"));

  /* Fill queue to ensure blocking */
  injectLocalRoutesAtRuntime({"30.3.0.0/16"}, {"3003:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3003:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Duration timer (1ms) should have fired — peer3 detached */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Peer4 continues normally */
  injectLocalRoutesAtRuntime({"30.4.0.0/16"}, {"3004:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3004:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: DurationTimer_DuringPolicyReeval ===");
}

/* Duration timer fires during group state transition.
 * Block peer3, inject routes to push group READY→WAITING, set 1ms duration.
 * Timer fires during the WAITING transition — verify clean detachment. */
TEST_P(UpdateGroupMultiPeerTest, DurationTimer_DuringStateTransition) {
  XLOG(INFO, "=== TEST: DurationTimer_DuringStateTransition ===");

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

  /* Set aggressive duration threshold */
  setSlowPeerThresholds(
      kPeerAddr3, std::chrono::milliseconds(1), 100, std::chrono::seconds(60));

  /* Block peer3, fill queue with 3 routes to trigger blocking + WAITING */
  blockPeer(kPeerAddr3);

  for (int i = 10; i <= 12; i++) {
    auto prefix = fmt::format("30.{}.0.0/16", i);
    auto community = fmt::format("30{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("30.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Duration timer (1ms) fires during/after WAITING transition */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Peer4 continues normally post-detach */
  injectLocalRoutesAtRuntime({"30.13.0.0/16"}, {"3013:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.13.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3013:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: DurationTimer_DuringStateTransition ===");
}

/* Duration threshold set to very large value (600s) — never fires.
 * Block peer3 with a 600s threshold. Peer stays JOINED_BLOCKED. Unblock and
 * verify normal recovery. */
TEST_P(UpdateGroupMultiPeerTest, DurationThreshold_VeryLarge_NeverFires) {
  XLOG(INFO, "=== TEST: DurationThreshold_VeryLarge_NeverFires ===");

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

  /* Very large duration — should never fire */
  setSlowPeerThresholds(
      kPeerAddr3, std::chrono::seconds(600), 100, std::chrono::seconds(60));

  /* Block and fill queue */
  blockPeer(kPeerAddr3);

  for (int i = 20; i <= 22; i++) {
    auto prefix = fmt::format("30.{}.0.0/16", i);
    auto community = fmt::format("30{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("30.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Peer3 should be JOINED_BLOCKED, NOT detached */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* Unblock without draining — verifyRouteAdd reads the routes */
  unblockPeer(kPeerAddr3, 0, 0);
  for (int i = 20; i <= 22; i++) {
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("30.{}.0.0", i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        fmt::format("30{}:1", i)));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Both peers receive new route after recovery */
  injectLocalRoutesAtRuntime({"30.23.0.0/16"}, {"3023:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.23.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.23.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3023:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.23.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3023:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);
  XLOG(INFO, "=== TEST PASSED: DurationThreshold_VeryLarge_NeverFires ===");
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
