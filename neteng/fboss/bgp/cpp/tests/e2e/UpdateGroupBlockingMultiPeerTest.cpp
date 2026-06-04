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
 * E2E tests for BGP Update Group blocking - Multi-peer scenarios
 * Tests: SomePeersBlockInGroup
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupDistributionTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Test: Some peers block in group, others continue
 */
TEST_P(UpdateGroupBlockingTest, SomePeersBlockInGroup) {
  XLOG(INFO, "=== TEST: SomePeersBlockInGroup ===");

  /*
   * Dual-stack peers send 2 EoRs each (v4 + v6).
   * Queue (3, 2, 0): highWm=2 allows both EoRs before backpressure.
   * After EoRs drained, routes trigger blocking at highWm=2.
   */
  setDefaultQueueSizes(3, 2, 0);
  XLOG(INFO, "Setting queue sizes: capacity=3, highWm=2, lowWm=0");
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
  XLOG(INFO, "Injecting route 60.0.0.0/8");
  injectLocalRoutesAtRuntime({"60.0.0.0/8"}, {"600:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("60.0.0.0/8")));
  XLOGF(
      INFO,
      "Route 60.0.0.0/8 reached shadowRIB. Peer3 state: {}, Peer4 state: {}",
      (isPeerQueueBlocked(peerId3) ? "BLOCKED" : "UNBLOCKED"),
      (isPeerQueueBlocked(peerId4) ? "BLOCKED" : "UNBLOCKED"));

  XLOG(INFO, "Injecting route 70.0.0.0/8");
  injectLocalRoutesAtRuntime({"70.0.0.0/8"}, {"600:2"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("70.0.0.0/8")));
  XLOGF(
      INFO,
      "Route 70.0.0.0/8 reached shadowRIB. Peer3 state: {}, Peer4 state: {}",
      (isPeerQueueBlocked(peerId3) ? "BLOCKED" : "UNBLOCKED"),
      (isPeerQueueBlocked(peerId4) ? "BLOCKED" : "UNBLOCKED"));

  XLOG(INFO, "Injecting route 80.0.0.0/8");
  injectLocalRoutesAtRuntime({"80.0.0.0/8"}, {"600:3"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("80.0.0.0/8")));
  XLOGF(
      INFO,
      "Route 80.0.0.0/8 reached shadowRIB. Peer3 state: {}, Peer4 state: {}",
      (isPeerQueueBlocked(peerId3) ? "BLOCKED" : "UNBLOCKED"),
      (isPeerQueueBlocked(peerId4) ? "BLOCKED" : "UNBLOCKED"));

  /*
   * Both should be blocked initially. Use waitForPeerQueueBlocked for robust
   * checking since the point-in-time isPeerQueueBlocked check can be racy.
   */
  XLOG(INFO, "Checking if both peers are blocked");
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  XLOG(INFO, "Both peers blocked as expected");

  /* Unblock peer4 by reading (order not guaranteed) */
  XLOG(INFO, "Draining peer4's queue to unblock. Expected Nexthop: 127.5.0.3");
  EXPECT_TRUE(verifyRoutes(
      "v4",
      kPeerAddr4,
      {{.prefix = "60.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = "127.5.0.3",
        .expectedAsPath = "4200000001",
        .expectedCommunity = "600:1"},
       {.prefix = "70.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = "127.5.0.3",
        .expectedAsPath = "4200000001",
        .expectedCommunity = "600:2"},
       {.prefix = "80.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = "127.5.0.3",
        .expectedAsPath = "4200000001",
        .expectedCommunity = "600:3"}}));

  /*
   * peer4 unblocked after draining, peer3 still blocked. Use
   * waitForPeerQueueBlocked for robust checking.
   */
  XLOG(INFO, "Checking peer states after partial drain");
  EXPECT_FALSE(isPeerQueueBlocked(peerId4));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3, 10));
  XLOGF(
      INFO,
      "peer4 unblocked, peer3 still blocked - GOOD. Peer3 state: {}, Peer4 state: {}",
      (isPeerQueueBlocked(peerId3) ? "BLOCKED" : "UNBLOCKED"),
      (isPeerQueueBlocked(peerId4) ? "BLOCKED" : "UNBLOCKED"));

  /* Unblock peer3 (order not guaranteed) */
  XLOG(INFO, "Draining peer3's queue to unblock");
  EXPECT_TRUE(verifyRoutes(
      "v4",
      kPeerAddr3,
      {{.prefix = "60.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = "127.5.0.1",
        .expectedAsPath = "4200000001",
        .expectedCommunity = "600:1"},
       {.prefix = "70.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = "127.5.0.1",
        .expectedAsPath = "4200000001",
        .expectedCommunity = "600:2"},
       {.prefix = "80.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = "127.5.0.1",
        .expectedAsPath = "4200000001",
        .expectedCommunity = "600:3"}}));

  XLOG(INFO, "Checking final peer states");
  EXPECT_FALSE(isPeerQueueBlocked(peerId3));
  XLOGF(
      INFO,
      "Both peers unblocked. Final states - Peer3: {}, Peer4: {}",
      (isPeerQueueBlocked(peerId3) ? "BLOCKED" : "UNBLOCKED"),
      (isPeerQueueBlocked(peerId4) ? "BLOCKED" : "UNBLOCKED"));

  /* Phase 2: Withdraw all routes and verify */
  XLOG(INFO, "Phase 2: Withdrawing all routes");
  withdrawLocalRoutesAtRuntime({"60.0.0.0/8", "70.0.0.0/8", "80.0.0.0/8"});

  std::vector<E2ETestFixture::WithdrawSpec> expectedWithdrawals = {
      {.prefix = "60.0.0.0", .prefixLen = 8},
      {.prefix = "70.0.0.0", .prefixLen = 8},
      {.prefix = "80.0.0.0", .prefixLen = 8}};

  for (const auto& peerAddr : {kPeerAddr3, kPeerAddr4}) {
    EXPECT_TRUE(verifyRouteWithdraws("v4", peerAddr, expectedWithdrawals));
  }

  /* Phase 3: Re-add 2 routes */
  XLOG(INFO, "Phase 3: Re-adding 2 routes");
  injectLocalRoutesAtRuntime({"60.0.0.0/8"}, {"600:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("60.0.0.0/8")));
  injectLocalRoutesAtRuntime({"70.0.0.0/8"}, {"600:2"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("70.0.0.0/8")));

  for (const auto& peerAddr : {kPeerAddr3, kPeerAddr4}) {
    std::vector<E2ETestFixture::VerifySpec> expectedReAdded = {
        {.prefix = "60.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = getExpectedNexthop(peerAddr),
         .expectedAsPath = "4200000001",
         .expectedCommunity = "600:1"},
        {.prefix = "70.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = getExpectedNexthop(peerAddr),
         .expectedAsPath = "4200000001",
         .expectedCommunity = "600:2"}};
    EXPECT_TRUE(verifyRoutes("v4", peerAddr, expectedReAdded));
  }

  /* Phase 4: Change attributes */
  XLOG(INFO, "Phase 4: Changing route attributes (community change)");
  injectLocalRoutesAtRuntime({"60.0.0.0/8"}, {"600:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("60.0.0.0/8")));

  for (const auto& peerAddr : {kPeerAddr3, kPeerAddr4}) {
    std::vector<E2ETestFixture::VerifySpec> expectedChanged = {
        {.prefix = "60.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = getExpectedNexthop(peerAddr),
         .expectedAsPath = "4200000001",
         .expectedCommunity = "600:99"}};
    EXPECT_TRUE(verifyRoutes("v4", peerAddr, expectedChanged));
  }

  XLOG(INFO, "=== TEST PASSED: SomePeersBlockInGroup ===");
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
