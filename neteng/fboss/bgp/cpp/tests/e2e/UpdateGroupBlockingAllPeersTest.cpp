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
 * E2E tests for BGP Update Group blocking - All peers scenarios
 * Tests: AllPeersInGroupBlock, AllPeersBlockGoDown
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupDistributionTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Test: All peers in group block
 */
TEST_P(UpdateGroupBlockingTest, AllPeersInGroupBlock) {
  XLOG(INFO, "=== TEST: AllPeersInGroupBlock ===");

  /*
   * Dual-stack peers send 2 EoRs each. Queue (3, 2, 0) allows both EoRs.
   */
  setDefaultQueueSizes(3, 2, 0);
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupComponents();

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  XLOG(INFO, "Both peers established and received EoR");

  /* Add routes with DIFFERENT communities = 3 BGP UPDATEs - all peers block */
  XLOG(INFO, "Injecting 3 routes to cause all peers to block");
  XLOG(INFO, "Injecting route 100.0.0.0/8");
  injectLocalRoutesAtRuntime({"100.0.0.0/8"}, {"1000:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("100.0.0.0/8")));
  XLOGF(
      INFO,
      "Route 100.0.0.0/8 reached shadowRIB. Peer3 state: {}, Peer4 state: {}",
      (isPeerQueueBlocked(peerId3) ? "BLOCKED" : "UNBLOCKED"),
      (isPeerQueueBlocked(peerId4) ? "BLOCKED" : "UNBLOCKED"));

  XLOG(INFO, "Injecting route 101.0.0.0/8");
  injectLocalRoutesAtRuntime({"101.0.0.0/8"}, {"1000:2"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("101.0.0.0/8")));
  XLOGF(
      INFO,
      "Route 101.0.0.0/8 reached shadowRIB. Peer3 state: {}, Peer4 state: {}",
      (isPeerQueueBlocked(peerId3) ? "BLOCKED" : "UNBLOCKED"),
      (isPeerQueueBlocked(peerId4) ? "BLOCKED" : "UNBLOCKED"));

  XLOG(INFO, "Injecting route 102.0.0.0/8");
  injectLocalRoutesAtRuntime({"102.0.0.0/8"}, {"1000:3"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("102.0.0.0/8")));
  XLOGF(
      INFO,
      "Route 102.0.0.0/8 reached shadowRIB. Peer3 state: {}, Peer4 state: {}",
      (isPeerQueueBlocked(peerId3) ? "BLOCKED" : "UNBLOCKED"),
      (isPeerQueueBlocked(peerId4) ? "BLOCKED" : "UNBLOCKED"));

  /*
   * Use waitForPeerQueueBlocked for robust checking since the point-in-time
   * isPeerQueueBlocked check can be racy.
   */
  XLOG(INFO, "Verifying both peers are blocked");
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  XLOG(INFO, "All peers blocked");

  /* Unblock all (order not guaranteed) */
  XLOGF(
      INFO,
      "Verifying routes for peer3 (unblocking). Expected Nexthop: {}",
      "127.5.0.1");
  EXPECT_TRUE(verifyRoutes(
      "v4",
      kPeerAddr3,
      {{.prefix = "100.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = "127.5.0.1",
        .expectedAsPath = "4200000001",
        .expectedCommunity = "1000:1"},
       {.prefix = "101.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = "127.5.0.1",
        .expectedAsPath = "4200000001",
        .expectedCommunity = "1000:2"},
       {.prefix = "102.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = "127.5.0.1",
        .expectedAsPath = "4200000001",
        .expectedCommunity = "1000:3"}}));

  XLOG(INFO, "Peer3 routes verified. Verifying peer4 routes...");
  EXPECT_TRUE(verifyRoutes(
      "v4",
      kPeerAddr4,
      {{.prefix = "100.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = "127.5.0.3",
        .expectedAsPath = "4200000001",
        .expectedCommunity = "1000:1"},
       {.prefix = "101.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = "127.5.0.3",
        .expectedAsPath = "4200000001",
        .expectedCommunity = "1000:2"},
       {.prefix = "102.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = "127.5.0.3",
        .expectedAsPath = "4200000001",
        .expectedCommunity = "1000:3"}}));

  XLOG(INFO, "Checking final queue states");
  EXPECT_FALSE(isPeerQueueBlocked(peerId3));
  EXPECT_FALSE(isPeerQueueBlocked(peerId4));
  XLOGF(
      INFO,
      "All peers unblocked. Final states - Peer3: {}, Peer4: {}",
      (isPeerQueueBlocked(peerId3) ? "BLOCKED" : "UNBLOCKED"),
      (isPeerQueueBlocked(peerId4) ? "BLOCKED" : "UNBLOCKED"));

  /* Phase 2: Withdraw all routes and verify */
  XLOG(INFO, "Phase 2: Withdrawing all routes");
  withdrawLocalRoutesAtRuntime({"100.0.0.0/8", "101.0.0.0/8", "102.0.0.0/8"});

  std::vector<E2ETestFixture::WithdrawSpec> expectedWithdrawals = {
      {.prefix = "100.0.0.0", .prefixLen = 8},
      {.prefix = "101.0.0.0", .prefixLen = 8},
      {.prefix = "102.0.0.0", .prefixLen = 8}};

  for (const auto& peerAddr : {kPeerAddr3, kPeerAddr4}) {
    EXPECT_TRUE(verifyRouteWithdraws("v4", peerAddr, expectedWithdrawals));
  }

  /* Phase 3: Re-add 2 routes */
  XLOG(INFO, "Phase 3: Re-adding 2 routes");
  injectLocalRoutesAtRuntime({"100.0.0.0/8"}, {"1000:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("100.0.0.0/8")));
  injectLocalRoutesAtRuntime({"101.0.0.0/8"}, {"1000:2"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("101.0.0.0/8")));

  for (const auto& peerAddr : {kPeerAddr3, kPeerAddr4}) {
    std::vector<E2ETestFixture::VerifySpec> expectedReAdded = {
        {.prefix = "100.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = getExpectedNexthop(peerAddr),
         .expectedAsPath = "4200000001",
         .expectedCommunity = "1000:1"},
        {.prefix = "101.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = getExpectedNexthop(peerAddr),
         .expectedAsPath = "4200000001",
         .expectedCommunity = "1000:2"}};
    EXPECT_TRUE(verifyRoutes("v4", peerAddr, expectedReAdded));
  }

  /* Phase 4: Change attributes */
  XLOG(INFO, "Phase 4: Changing route attributes (community change)");
  injectLocalRoutesAtRuntime({"100.0.0.0/8"}, {"1000:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("100.0.0.0/8")));

  for (const auto& peerAddr : {kPeerAddr3, kPeerAddr4}) {
    std::vector<E2ETestFixture::VerifySpec> expectedChanged = {
        {.prefix = "100.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = getExpectedNexthop(peerAddr),
         .expectedAsPath = "4200000001",
         .expectedCommunity = "1000:99"}};
    EXPECT_TRUE(verifyRoutes("v4", peerAddr, expectedChanged));
  }

  XLOG(INFO, "=== TEST PASSED ===");
}

/*
 * Test: All peers block, all go down
 */
TEST_P(UpdateGroupBlockingTest, AllPeersBlockGoDown) {
  XLOG(INFO, "=== TEST: AllPeersBlockGoDown ===");

  /*
   * Dual-stack peers send 2 EoRs each. Queue (3, 2, 0) allows both EoRs.
   */
  setDefaultQueueSizes(3, 2, 0);
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addLocalRoute("110.0.0.0/8", {"1100:1"}, 150);
  setupComponents();

  auto routePrefix = folly::IPAddress::createNetwork("110.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(routePrefix));
  XLOG(INFO, "Initial route 110.0.0.0/8 reached shadowRIB");

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  XLOG(INFO, "Both peers brought up");

  /* Read initial dump */
  XLOG(INFO, "Reading initial dump routes");
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "110.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1100:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "110.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1100:1"));
  /* Dual-stack peers send 2 EoRs each (v4 + v6) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  XLOG(INFO, "Both peers received initial dump and EoRs");

  /* Add 3 more routes with DIFFERENT communities = 3 BGP UPDATEs - all block */
  XLOG(INFO, "Injecting 3 routes to cause both peers to block");
  XLOG(INFO, "Injecting route 111.0.0.0/8");
  injectLocalRoutesAtRuntime({"111.0.0.0/8"}, {"1110:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("111.0.0.0/8")));
  XLOGF(
      INFO,
      "Route 111.0.0.0/8 reached shadowRIB. Peer3 state: {}, Peer4 state: {}",
      (isPeerQueueBlocked(peerId3) ? "BLOCKED" : "UNBLOCKED"),
      (isPeerQueueBlocked(peerId4) ? "BLOCKED" : "UNBLOCKED"));

  XLOG(INFO, "Injecting route 112.0.0.0/8");
  injectLocalRoutesAtRuntime({"112.0.0.0/8"}, {"1110:2"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("112.0.0.0/8")));
  XLOGF(
      INFO,
      "Route 112.0.0.0/8 reached shadowRIB. Peer3 state: {}, Peer4 state: {}",
      (isPeerQueueBlocked(peerId3) ? "BLOCKED" : "UNBLOCKED"),
      (isPeerQueueBlocked(peerId4) ? "BLOCKED" : "UNBLOCKED"));

  XLOG(INFO, "Injecting route 113.0.0.0/8");
  injectLocalRoutesAtRuntime({"113.0.0.0/8"}, {"1110:3"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("113.0.0.0/8")));
  XLOGF(
      INFO,
      "Route 113.0.0.0/8 reached shadowRIB. Peer3 state: {}, Peer4 state: {}",
      (isPeerQueueBlocked(peerId3) ? "BLOCKED" : "UNBLOCKED"),
      (isPeerQueueBlocked(peerId4) ? "BLOCKED" : "UNBLOCKED"));

  /*
   * Use waitForPeerQueueBlocked for robust checking since the point-in-time
   * isPeerQueueBlocked check can be racy.
   */
  XLOG(INFO, "Verifying both peers are blocked");
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  XLOG(INFO, "All peers blocked as expected");

  /* Bring all down while blocked */
  XLOG(INFO, "Bringing all peers down while blocked");
  bringDownPeer(kPeerAddr3);
  XLOG(INFO, "Peer3 brought down");
  bringDownPeer(kPeerAddr4);
  XLOG(INFO, "Peer4 brought down. Test complete");

  /* Bring all back up */
  XLOG(INFO, "Bringing all peers back up");
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  XLOGF(
      INFO,
      "Verifying routes for peer3. Expected Nexthop: {}",
      getExpectedNexthop(kPeerAddr3));
  /* Should get all routes from shadowRIB (order not guaranteed) */
  EXPECT_TRUE(verifyRoutes(
      "v4",
      kPeerAddr3,
      {{.prefix = "110.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "1100:1"},
       {.prefix = "111.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "1110:1"},
       {.prefix = "112.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "1110:2"},
       {.prefix = "113.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "1110:3"}}));

  EXPECT_TRUE(verifyRoutes(
      "v4",
      kPeerAddr4,
      {{.prefix = "110.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = "127.5.0.3",
        .expectedAsPath = "4200000001",
        .expectedCommunity = "1100:1"},
       {.prefix = "111.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = "127.5.0.3",
        .expectedAsPath = "4200000001",
        .expectedCommunity = "1110:1"},
       {.prefix = "112.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = "127.5.0.3",
        .expectedAsPath = "4200000001",
        .expectedCommunity = "1110:2"},
       {.prefix = "113.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = "127.5.0.3",
        .expectedAsPath = "4200000001",
        .expectedCommunity = "1110:3"}}));

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));

  /*
   * Phase 2: WITHDRAW routes while peers are running.
   * Verify withdrawals are received by both peers.
   */
  XLOG(INFO, "Phase 2: Withdrawing routes 111 and 112");
  withdrawLocalRoutesAtRuntime({"111.0.0.0/8", "112.0.0.0/8"});

  std::vector<WithdrawSpec> expectedWithdraws = {
      {.prefix = "111.0.0.0", .prefixLen = 8},
      {.prefix = "112.0.0.0", .prefixLen = 8}};

  EXPECT_TRUE(verifyRouteWithdraws("v4", kPeerAddr3, expectedWithdraws));
  EXPECT_TRUE(verifyRouteWithdraws("v4", kPeerAddr4, expectedWithdraws));
  XLOG(INFO, "Withdrawals verified for both peers");

  /*
   * Phase 3: RE-ADD withdrawn routes.
   * Verify only the re-added routes are received.
   */
  XLOG(INFO, "Phase 3: Re-adding routes 111 and 112");
  injectLocalRoutesAtRuntime({"111.0.0.0/8"}, {"1110:1"}, 150);
  injectLocalRoutesAtRuntime({"112.0.0.0/8"}, {"1110:2"}, 150);

  EXPECT_TRUE(verifyRoutes(
      "v4",
      kPeerAddr3,
      {{.prefix = "111.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "1110:1"},
       {.prefix = "112.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "1110:2"}}));

  EXPECT_TRUE(verifyRoutes(
      "v4",
      kPeerAddr4,
      {{.prefix = "111.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = "127.5.0.3",
        .expectedAsPath = "4200000001",
        .expectedCommunity = "1110:1"},
       {.prefix = "112.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = "127.5.0.3",
        .expectedAsPath = "4200000001",
        .expectedCommunity = "1110:2"}}));
  XLOG(INFO, "Re-added routes verified for both peers");

  /*
   * Phase 4: CHANGE attributes (change community and local pref).
   * Verify only the changed route is received.
   */
  XLOG(INFO, "Phase 4: Changing attributes for route 113");
  injectLocalRoutesAtRuntime({"113.0.0.0/8"}, {"1110:99"}, 200);

  EXPECT_TRUE(verifyRoutes(
      "v4",
      kPeerAddr3,
      {{.prefix = "113.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "1110:99"}}));

  EXPECT_TRUE(verifyRoutes(
      "v4",
      kPeerAddr4,
      {{.prefix = "113.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = "127.5.0.3",
        .expectedAsPath = "4200000001",
        .expectedCommunity = "1110:99"}}));
  XLOG(INFO, "Attribute change verified for both peers");

  XLOG(INFO, "=== TEST PASSED: AllPeersBlockGoDown ===");
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
