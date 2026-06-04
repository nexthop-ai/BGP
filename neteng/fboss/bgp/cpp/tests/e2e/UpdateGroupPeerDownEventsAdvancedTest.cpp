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
 * E2E tests: P-DOWN state x Event Matrix (Part 2)
 * Tests for P-DOWN peer state (continued) and P-INIT route-add.
 *
 * Prefix range: 11.x.0.0/16
 *
 * Tests: PeerDown continued (PL drain, CL end, policy, refresh, MRAI, EoR,
 * multi-route) + Init RouteAdd
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

TEST_P(UpdateGroupSlowPeerDetectionTest, PeerDown_PeerDown) {
  XLOGF(INFO, "=== TEST: PeerDown_PeerDown ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(8, 6, 2);

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

  /* Bring peer3 down */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Bring peer3 down AGAIN -- should be idempotent, no crash or double-free */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Peer4 unaffected */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Confirm peer4 still receives routes */
  injectLocalRoutesAtRuntime({"11.87.0.0/16"}, {"1187:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.87.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.87.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1187:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOGF(INFO, "=== TEST PASSED: PeerDown_PeerDown ===");
}
/*
 * P-DOWN x E-CL-END
 * N/A -- no CL consumer for a DOWN peer. Inject and withdraw routes
 * (which generates CL entries) while peer is DOWN. The CL processing
 * skips the DOWN peer gracefully.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, PeerDown_ClEnd) {
  XLOGF(INFO, "=== TEST: PeerDown_ClEnd ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(8, 6, 2);

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

  /* Inject a route while both peers are up */
  injectLocalRoutesAtRuntime({"11.90.0.0/16"}, {"1190:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.90.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.90.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1190:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.90.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1190:1"));

  /* Bring peer3 down */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /*
   * Withdraw the route -- generates a CL entry. CL processing should
   * skip the DOWN peer and only deliver to peer4.
   */
  withdrawLocalRoutesAtRuntime({"11.90.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "11.90.0.0", 16, kPeerAddr4));

  /* Re-inject a different route to exercise CL end processing */
  injectLocalRoutesAtRuntime({"11.91.0.0/16"}, {"1191:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.91.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.91.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1191:1"));

  /* Peer3 stays DOWN through all CL activity */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Peer4 still functional */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOGF(INFO, "=== TEST PASSED: PeerDown_ClEnd ===");
}

/*
 * P-DOWN x E-MRAI-FIRE
 * N/A -- no MRAI timer for DOWN peer. Simulate MRAI-like activity by
 * injecting multiple routes with different communities (each triggers
 * a separate UPDATE and MRAI cycle) while peer is DOWN. Verify DOWN
 * peer is unaffected and group processes normally.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, PeerDown_MraiFire) {
  XLOGF(INFO, "=== TEST: PeerDown_MraiFire ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(8, 6, 2);

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

  /* Bring peer3 down */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /*
   * Inject routes with different communities -- each generates a separate
   * UPDATE message and internally triggers MRAI cycles. The DOWN peer
   * should be skipped by all MRAI processing.
   */
  injectLocalRoutesAtRuntime({"11.96.0.0/16"}, {"1196:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.96.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.96.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1196:1"));

  injectLocalRoutesAtRuntime({"11.97.0.0/16"}, {"1197:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.97.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.97.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1197:1"));

  injectLocalRoutesAtRuntime({"11.98.0.0/16"}, {"1198:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.98.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.98.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1198:1"));

  /* Peer3 stays DOWN through all MRAI cycles */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Peer4 still functional */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOGF(INFO, "=== TEST PASSED: PeerDown_MraiFire ===");
}

/*
 * P-DOWN x E-EOR
 * N/A -- sendEoRToPeer on a DOWN peer is a no-op, no crash.
 * Verify that sending EoR to a DOWN peer doesn't change its state
 * and the group continues to function normally.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, PeerDown_EoR) {
  XLOGF(INFO, "=== TEST: PeerDown_EoR ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(8, 6, 2);

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

  /* Bring peer3 down */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Send EoR to a DOWN peer -- should be a no-op, no crash */
  sendEoRToPeer(peerId3);

  /* Peer3 stays DOWN -- EoR to a DOWN peer has no effect */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Peer4 unaffected -- inject a route to confirm group still works */
  injectLocalRoutesAtRuntime({"11.99.0.0/16"}, {"1199:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.99.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.99.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1199:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOGF(INFO, "=== TEST PASSED: PeerDown_EoR ===");
}

/*
 * P-DOWN x E-MULTI-ROUTE
 * Multiple routes injected while peer is DOWN -- group processes all
 * routes for the remaining peer, DOWN peer stays completely unaffected.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, PeerDown_MultiRoute) {
  XLOGF(INFO, "=== TEST: PeerDown_MultiRoute ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(8, 6, 2);

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

  /* Bring peer3 down */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /*
   * Inject a batch of routes -- each with different community so they
   * generate separate UPDATEs. DOWN peer should be skipped for all.
   * Inject and drain one at a time to avoid ordering issues.
   */
  for (int i = 0; i < 4; ++i) {
    auto prefix = "11." + std::to_string(100 + i) + ".0.0/16";
    auto prefixAddr = "11." + std::to_string(100 + i) + ".0.0";
    auto community = std::to_string(11100 + i) + ":1";
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        prefixAddr,
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Peer3 stays DOWN through all route injections */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Peer4 still functional after receiving batch */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOGF(INFO, "=== TEST PASSED: PeerDown_MultiRoute ===");
}

/*
 * P-INIT x E-ROUTE-ADD
 * Route arrives during init dump -- goes to the CL while peer3 is
 * still in INIT (has not sent EoR yet). After peer3 completes EoR
 * and reaches JOINED_RUNNING, it receives the route from CL processing.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, Init_RouteAdd) {
  XLOGF(INFO, "=== TEST: Init_RouteAdd ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* Bring up both peers but do NOT send EoR yet -- they stay in INIT */
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /* Complete peer4 first so it can receive routes */
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /*
   * Inject a route while peer3 is still in INIT (no EoR sent).
   * The route goes to the CL. Peer4 receives it from PL processing.
   */
  injectLocalRoutesAtRuntime({"11.104.0.0/16"}, {"11104:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("11.104.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.104.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "11104:1"));

  /* Now complete peer3's EoR -- it transitions INIT -> JOINED_RUNNING */
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /*
   * Peer3 should now receive the route that was queued in CL during
   * its init dump phase. The init dump sends it as part of the
   * initial dump or CL processing after joining.
   */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.104.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "11104:1"));

  /* Both peers should be JOINED_RUNNING and in sync */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOGF(INFO, "=== TEST PASSED: Init_RouteAdd ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupSlowPeerDetectionTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
