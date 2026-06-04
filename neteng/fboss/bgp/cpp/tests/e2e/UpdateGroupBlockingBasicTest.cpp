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
 * E2E tests for BGP Update Group blocking - Basic scenarios
 * Tests: BlockingAndUnblocking, BlockingPeerDown, PeerBlocksGoesDownComesBack
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupDistributionTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Test 12: Simulate blocking and unblocking, verify peers get updates
 */
TEST_P(UpdateGroupBlockingTest, BlockingAndUnblocking) {
  /* Add peers to configuration */
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  addLocalRoute("31.0.0.0/8", {"311:1"}, 100);

  XLOG(INFO, "=== TEST: Blocking and unblocking ===");

  setupComponents();

  /* Wait for route to reach shadowRIB */
  auto routePrefix = folly::IPAddress::createNetwork("31.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(routePrefix))
      << "Route 31.0.0.0/8 did not reach shadowRIB in time";

  /* Bring up peers */
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /* Send EoR to each peer (ingress EoR) */
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  sendEoRToPeer(peerId4);

  /* Consume route UPDATEs from BOTH peers */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "311:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "311:1"));

  /* Wait for egress EoR from all peers (final PDU) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));

  /* Inject route while peers are active */
  injectLocalRoutesAtRuntime({"32.0.0.0/8"}, {"312:1"}, 150);

  /* Peers should receive update even with egress backpressure */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "312:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "312:1"));

  /* Phase 2: Add more runtime routes */
  XLOG(INFO, "Phase 2: Adding more runtime routes");
  injectLocalRoutesAtRuntime({"34.0.0.0/8"}, {"314:1"}, 150);

  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "314:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "314:1"));

  /* Phase 3: WITHDRAW routes */
  XLOG(INFO, "Phase 3: Withdrawing routes");
  withdrawLocalRoutesAtRuntime({"32.0.0.0/8"});

  EXPECT_TRUE(verifyRouteWithdraw("v4", "32.0.0.0", 8, kPeerAddr3));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "32.0.0.0", 8, kPeerAddr4));

  /* Phase 4: RE-ADD withdrawn route */
  XLOG(INFO, "Phase 4: Re-adding withdrawn route");
  injectLocalRoutesAtRuntime({"32.0.0.0/8"}, {"312:2"}, 150);

  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "312:2"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "312:2"));

  /* Phase 5: CHANGE attributes */
  XLOG(INFO, "Phase 5: Changing route attributes");
  injectLocalRoutesAtRuntime({"34.0.0.0/8"}, {"314:99"}, 150);

  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "314:99"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "314:99"));

  XLOG(INFO, "=== TEST PASSED: Blocking/unblocking ===");
}

/*
 * Test 13: Blocking peer goes down
 */
TEST_P(UpdateGroupBlockingTest, BlockingPeerDown) {
  /* Add peers to configuration */
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  addLocalRoute("33.0.0.0/8", {"313:1"}, 100);

  XLOG(INFO, "=== TEST: Blocking peer going down ===");

  setupComponents();

  /* Wait for route to reach shadowRIB */
  auto routePrefix = folly::IPAddress::createNetwork("33.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(routePrefix))
      << "Route 33.0.0.0/8 did not reach shadowRIB in time";

  /* Bring up peers */
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /* Send EoR to each peer (ingress EoR) */
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  sendEoRToPeer(peerId4);

  /* Consume route UPDATEs from BOTH peers */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "313:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "313:1"));

  /* Wait for egress EoR from all peers (final PDU) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));

  bringDownPeer(kPeerAddr3);

  /* Phase 2: Add routes while peer3 is down (only peer4 receives) */
  XLOG(INFO, "Phase 2: Adding routes while peer3 is down");
  injectLocalRoutesAtRuntime({"35.0.0.0/8"}, {"315:1"}, 150);

  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "315:1"));

  /* Phase 3: WITHDRAW route */
  XLOG(INFO, "Phase 3: Withdrawing route");
  withdrawLocalRoutesAtRuntime({"35.0.0.0/8"});

  EXPECT_TRUE(verifyRouteWithdraw("v4", "35.0.0.0", 8, kPeerAddr4));

  /* Phase 4: RE-ADD withdrawn route */
  XLOG(INFO, "Phase 4: Re-adding withdrawn route");
  injectLocalRoutesAtRuntime({"35.0.0.0/8"}, {"315:2"}, 150);

  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "315:2"));

  /* Phase 5: CHANGE attributes */
  XLOG(INFO, "Phase 5: Changing route attributes");
  injectLocalRoutesAtRuntime({"33.0.0.0/8"}, {"313:99"}, 150);

  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "313:99"));

  XLOG(INFO, "=== TEST PASSED: Blocking peer down ===");
}

/*
 * Test: Peer blocks, goes down, comes back up
 * Tests blocking recovery after peer restart - verifies blocking state
 * is properly cleaned up when peer reconnects.
 */
TEST_P(UpdateGroupBlockingTest, PeerBlocksGoesDownComesBack) {
  XLOG(INFO, "=== TEST: PeerBlocksGoesDownComesBack ===");

  /*
   * Dual-stack peer sends 2 EoRs. Queue (3, 2, 0) allows both EoRs.
   */
  setDefaultQueueSizes(3, 2, 0);
  addPeer(kDefaultPeerSpec3);
  addLocalRoute("90.0.0.0/8", {"900:1"}, 150);
  setupComponents();

  auto routePrefix = folly::IPAddress::createNetwork("90.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(routePrefix));
  XLOG(INFO, "Initial route 90.0.0.0/8 reached shadowRIB");

  bringUpPeer(kPeerAddr3);
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  XLOG(INFO, "Peer3 brought up");

  /* Read initial dump and EoR */
  XLOG(INFO, "Reading initial dump route");
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "90.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "900:1"));
  /* Dual-stack peer sends 2 EoRs (v4 + v6) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  XLOG(INFO, "Initial dump and EoRs received");

  /*
   * Add 3 more routes with DIFFERENT communities = 3 BGP UPDATEs - peer blocks
   */
  XLOG(INFO, "Injecting 3 routes to cause peer blocking");
  XLOG(INFO, "Injecting route 91.0.0.0/8");
  injectLocalRoutesAtRuntime({"91.0.0.0/8"}, {"910:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("91.0.0.0/8")));
  XLOGF(
      INFO,
      "Route 91.0.0.0/8 reached shadowRIB. Queue state: {}",
      (isPeerQueueBlocked(peerId3) ? "BLOCKED" : "UNBLOCKED"));

  XLOG(INFO, "Injecting route 92.0.0.0/8");
  injectLocalRoutesAtRuntime({"92.0.0.0/8"}, {"910:2"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("92.0.0.0/8")));
  XLOGF(
      INFO,
      "Route 92.0.0.0/8 reached shadowRIB. Queue state: {}",
      (isPeerQueueBlocked(peerId3) ? "BLOCKED" : "UNBLOCKED"));

  XLOG(INFO, "Injecting route 93.0.0.0/8");
  injectLocalRoutesAtRuntime({"93.0.0.0/8"}, {"910:3"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("93.0.0.0/8")));
  XLOGF(
      INFO,
      "Route 93.0.0.0/8 reached shadowRIB. Queue state: {}",
      (isPeerQueueBlocked(peerId3) ? "BLOCKED" : "UNBLOCKED"));

  /*
   * Use waitForPeerQueueBlocked for robust checking since the point-in-time
   * isPeerQueueBlocked check can be racy.
   */
  XLOG(INFO, "Verifying peer is blocked");
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  XLOG(INFO, "Peer blocked as expected");

  /* Bring peer down while blocked */
  XLOG(INFO, "Bringing peer down while blocked");
  bringDownPeer(kPeerAddr3);
  XLOG(INFO, "Peer brought down successfully");

  // Bring peer back up
  XLOG(INFO, "Bringing peer back up");
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  XLOGF(
      INFO,
      "Peer brought back up, checking queue state: {}",
      (isPeerQueueBlocked(peerId3) ? "BLOCKED" : "UNBLOCKED"));

  // Should get all routes from shadowRIB (order not guaranteed)
  XLOGF(
      INFO,
      "Verifying routes for peer3 after reconnect. Expected Nexthop: {}",
      getExpectedNexthop(kPeerAddr3));
  EXPECT_TRUE(verifyRoutes(
      "v4",
      kPeerAddr3,
      {{.prefix = "90.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "900:1"},
       {.prefix = "91.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "910:1"},
       {.prefix = "92.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "910:2"},
       {.prefix = "93.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "910:3"}}));
  /* Dual-stack peer sends 2 EoRs after reconnect */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));

  /*
   * Phase 2: WITHDRAW some routes.
   * Verify withdrawals are received - this confirms peer is functioning
   * correctly after recovery.
   */
  XLOG(INFO, "Phase 2: Withdrawing routes 91 and 92");
  withdrawLocalRoutesAtRuntime({"91.0.0.0/8", "92.0.0.0/8"});

  std::vector<WithdrawSpec> expectedWithdraws = {
      {.prefix = "91.0.0.0", .prefixLen = 8},
      {.prefix = "92.0.0.0", .prefixLen = 8}};

  EXPECT_TRUE(verifyRouteWithdraws("v4", kPeerAddr3, expectedWithdraws));
  XLOG(INFO, "Withdrawals verified");

  /*
   * Phase 3: RE-ADD withdrawn routes.
   * Verify only the re-added routes are received.
   */
  XLOG(INFO, "Phase 3: Re-adding routes 91 and 92");
  injectLocalRoutesAtRuntime({"91.0.0.0/8"}, {"910:1"}, 150);
  injectLocalRoutesAtRuntime({"92.0.0.0/8"}, {"910:2"}, 150);

  EXPECT_TRUE(verifyRoutes(
      "v4",
      kPeerAddr3,
      {{.prefix = "91.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "910:1"},
       {.prefix = "92.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "910:2"}}));
  XLOG(INFO, "Re-added routes verified");

  /*
   * Phase 4: CHANGE attributes (change community).
   * Verify only the changed route is received.
   */
  XLOG(INFO, "Phase 4: Changing attributes for route 93");
  injectLocalRoutesAtRuntime({"93.0.0.0/8"}, {"910:99"}, 200);

  EXPECT_TRUE(verifyRoutes(
      "v4",
      kPeerAddr3,
      {{.prefix = "93.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "910:99"}}));
  XLOG(INFO, "Attribute change verified");

  XLOG(INFO, "=== TEST PASSED: PeerBlocksGoesDownComesBack ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupBlockingTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
