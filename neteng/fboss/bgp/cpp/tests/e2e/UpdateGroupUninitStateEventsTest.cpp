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
 * E2E tests: G-UNINIT state x Event Matrix
 * Tests for G-UNINITIALIZED group state against all 15 events.
 *
 * Stack A fixtures: UpdateGroupDetachmentTest (prefix range 12.x)
 * Stack B fixtures: UpdateGroupMultiPeerTest (prefix range 74.x)
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * G-UNINIT x E-ROUTE-WD
 * Inject then withdraw a route during UNINIT. Both CL items (add + withdraw)
 * are consumed after EoR. Normal operation resumes.
 */
TEST_P(UpdateGroupMultiPeerTest, Uninit_RouteWithdraw) {
  XLOG(INFO, "=== TEST: Uninit_RouteWithdraw ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /* Inject and withdraw during UNINIT */
  injectLocalRoutesAtRuntime({"74.5.0.0/16"}, {"7405:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("74.5.0.0/16")));
  withdrawLocalRoutesAtRuntime({"74.5.0.0/16"});

  /* Complete both peers */
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Drain init dump + CL items */
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  /* Consume the 74.5.0.0/16 withdrawal that may arrive after drain
   * (serialized mode adds latency to message delivery) */
  EXPECT_TRUE(verifyRouteWithdraw("v4", "74.5.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "74.5.0.0", 16, kPeerAddr4));

  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Verify normal post-UNINIT operation */
  injectLocalRoutesAtRuntime({"74.6.0.0/16"}, {"7406:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("74.6.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "74.6.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "7406:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "74.6.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "7406:1"));

  XLOG(INFO, "=== TEST PASSED: Uninit_RouteWithdraw ===");
}

/*
 * G-UNINIT x E-BLOCK
 * Peer blocks during initial dump with small queue. Block peer3 before EoR,
 * inject routes to fill queue. After EoR+unblock, verify recovery.
 */
TEST_P(UpdateGroupMultiPeerTest, Uninit_Block) {
  XLOG(INFO, "=== TEST: Uninit_Block ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /* Complete peer4 first */
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  /* Block peer3 during INIT - before EoR */
  blockPeer(kPeerAddr3);

  /* Inject 3 routes to fill peer3's queue */
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("74.{}.0.0/16", 10 + i);
    auto community = fmt::format("{}:1", 7410 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("74.{}.0.0", 10 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Complete peer3 EoR */
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));

  /* Unblock peer3 - drains queued messages */
  unblockPeer(kPeerAddr3);
  /* After unblock during INIT, use waitForPeerState instead of waitForEoR */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Verify peer4 works */
  injectLocalRoutesAtRuntime({"74.15.0.0/16"}, {"7415:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("74.15.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "74.15.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "7415:1"));

  XLOG(INFO, "=== TEST PASSED: Uninit_Block ===");
}

/*
 * G-UNINIT x E-SLOW-DUR
 * Duration timer during init dump. Block peer3 during UNINIT with 1ms
 * duration threshold. The threshold may not fire during init dump PL drain.
 * Verify no crash; peer4 completes normally.
 */
TEST_P(UpdateGroupMultiPeerTest, Uninit_SlowDur) {
  XLOG(INFO, "=== TEST: Uninit_SlowDur ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /* Set aggressive duration threshold AFTER peers are up */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(1),
      999999,
      std::chrono::milliseconds(60000));

  /* Complete peer4 first */
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  /* Complete peer3 */
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));

  /* Check peer3 state - may be JR or detached depending on timing */
  auto state3 = getPeerState(kPeerAddr3);
  EXPECT_NE(state3, PeerUpdateState::DOWN);

  /* Peer4 works */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  injectLocalRoutesAtRuntime({"74.40.0.0/16"}, {"7440:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("74.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "74.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "7440:1"));

  XLOG(INFO, "=== TEST PASSED: Uninit_SlowDur ===");
}

/*
 * G-UNINIT x E-MRAI-FIRE
 * MRAI during UNINIT - group processes init dump, MRAI cycles are internal.
 * Inject multiple routes with different communities (each triggers MRAI).
 * After EoR, peer4 receives all inline, peer3 gets CL items.
 */
TEST_P(UpdateGroupMultiPeerTest, Uninit_MraiFire) {
  XLOG(INFO, "=== TEST: Uninit_MraiFire ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /* Complete peer4 first */
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  /* Inject 3 routes during UNINIT - each triggers separate MRAI cycle */
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("74.{}.0.0/16", 63 + i);
    auto community = fmt::format("{}:1", 7463 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    /* Drain peer4 inline to avoid queue buildup */
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("74.{}.0.0", 63 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Complete peer3 - gets all 3 routes from CL after EoR */
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Drain peer3 init dump + CL items */
  drainPeerQueueCompletely(peerId3);

  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: Uninit_MraiFire ===");
}

/*
 * G-UNINIT x E-MULTI-ROUTE
 * Batch of 4 routes injected during UNINIT. All go to CL. After EoR,
 * both peers receive all routes. Inject-drain one at a time for peer4
 * (which completes first) to avoid order issues.
 */
TEST_P(UpdateGroupMultiPeerTest, Uninit_MultiRoute) {
  XLOG(INFO, "=== TEST: Uninit_MultiRoute ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /* Complete peer4 first */
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  /* Inject 4 routes during UNINIT, drain peer4 after each */
  for (int i = 0; i < 4; i++) {
    auto prefix = fmt::format("74.{}.0.0/16", 75 + i);
    auto community = fmt::format("{}:1", 7475 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("74.{}.0.0", 75 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Complete peer3 - gets all 4 from CL */
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Drain peer3 init dump + CL items */
  drainPeerQueueCompletely(peerId3);

  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Verify continued operation */
  injectLocalRoutesAtRuntime({"74.80.0.0/16"}, {"7480:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("74.80.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "74.80.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "7480:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "74.80.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "7480:1"));

  verifySlowPeerInvariants(kPeerAddr3);

  XLOG(INFO, "=== TEST PASSED: Uninit_MultiRoute ===");
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
