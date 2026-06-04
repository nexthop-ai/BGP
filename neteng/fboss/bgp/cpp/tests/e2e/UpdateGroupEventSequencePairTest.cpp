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
 * E2E tests for event sequence pair interleavings.
 * E-BLOCK then E-ROUTE-ADD — blocked peer misses route, CL has it
 * E-DETACH then E-ROUTE-WD — withdrawal with lazy clone
 * E-POLICY-CHG then E-PEER-DOWN — re-eval, peer dies mid-eval
 *
 * Prefix range: 36.x.0.0/16 (20-49)
 * Fixture: UpdateGroupMultiPeerTest
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Block peer3, then inject a route. The blocked peer can't
 * receive it via PL — the route goes to CL for peer3 and is delivered
 * to peer4 normally. After unblock, peer3 receives the queued route.
 */
TEST_P(UpdateGroupMultiPeerTest, BlockThenRouteAdd) {
  XLOG(INFO, "=== TEST: BlockThenRouteAdd ===");

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

  /* Block peer3 FIRST, before any route injection */
  blockPeer(kPeerAddr3);

  /* Inject 3 routes with different communities to fill the queue */
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("36.{}.0.0/16", 20 + i);
    auto community = fmt::format("362{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    /* Drain fast peer immediately */
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("36.{}.0.0", 20 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Peer3 should be blocked with routes queued */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* Unblock peer3 — it receives the queued routes */
  unblockPeer(kPeerAddr3, 0, 0);
  for (int i = 0; i < 3; i++) {
    auto community = fmt::format("362{}:1", i);
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("36.{}.0.0", 20 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
  }

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Verify continued operation — inject one more route */
  injectLocalRoutesAtRuntime({"36.23.0.0/16"}, {"3623:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.23.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.23.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3623:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.23.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3623:1"));

  verifySlowPeerInvariants(kPeerAddr3);
  XLOG(INFO, "=== TEST PASSED: BlockThenRouteAdd ===");
}

/*
 * Detach peer3 via freq threshold, then withdraw a shared route.
 * The withdrawal goes to CL for the detached peer (lazy clone preserves
 * its view). Peer4 (in-sync) receives the withdrawal normally.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachThenRouteWithdraw) {
  XLOG(INFO, "=== TEST: DetachThenRouteWithdraw ===");

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

  /* Inject a shared route — both peers receive it */
  injectLocalRoutesAtRuntime({"36.30.0.0/16"}, {"3630:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.30.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3630:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3630:1"));

  /* Trigger detachment on peer3 via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);

  /* Fill queue with 3 routes to trigger block + detach */
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("36.{}.0.0/16", 31 + i);
    auto community = fmt::format("363{}:1", 1 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("36.{}.0.0", 31 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Now withdraw the shared route — peer4 receives withdrawal */
  withdrawLocalRoutesAtRuntime({"36.30.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "36.30.0.0", 16, kPeerAddr4));

  /* Peer3 is detached — withdrawal goes to CL silently */
  /* Verify peer4 continues to work normally */
  injectLocalRoutesAtRuntime({"36.34.0.0/16"}, {"3634:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.34.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.34.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3634:1"));

  /* Peer3 still detached */
  ASSERT_TRUE(waitForPeerStateAny(
      kPeerAddr3,
      {PeerUpdateState::DETACHED_BLOCKED,
       PeerUpdateState::DETACHED_READY_TO_JOIN}));

  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: DetachThenRouteWithdraw ===");
}

/*
 * Simulate policy change (withdraw + re-inject with different
 * prefix), then bring peer3 DOWN mid-evaluation. The withdrawal and
 * new route are partially in-flight when the peer goes down.
 * Verify no crash and peer4 continues normally.
 */
TEST_P(UpdateGroupMultiPeerTest, PolicyChangeThenPeerDown) {
  XLOG(INFO, "=== TEST: PolicyChangeThenPeerDown ===");

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

  /* Inject shared route — both receive it */
  injectLocalRoutesAtRuntime({"36.40.0.0/16"}, {"3640:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.40.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3640:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3640:1"));

  /* Simulate policy change: withdraw the old route */
  withdrawLocalRoutesAtRuntime({"36.40.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "36.40.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "36.40.0.0", 16, kPeerAddr4));

  /* Inject replacement with different prefix + attributes */
  injectLocalRoutesAtRuntime({"36.41.0.0/16"}, {"3641:1"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.41.0.0/16")));

  /* Bring peer3 DOWN right after the new route is injected */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Peer4 should still receive the replacement route */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3641:1"));

  /* Verify peer4 continues to operate normally */
  injectLocalRoutesAtRuntime({"36.42.0.0/16"}, {"3642:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.42.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.42.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3642:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));
  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: PolicyChangeThenPeerDown ===");
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
