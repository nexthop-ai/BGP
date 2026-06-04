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
 * E2E tests for BGP Update Group blocking - Queue behavior
 * Tests: SmokeTestQueueBlocking, SinglePeerBlocks
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupDistributionTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * SMOKE TEST: Queue blocking infrastructure
 */
TEST_P(UpdateGroupBlockingTest, SmokeTestQueueBlocking) {
  XLOG(INFO, "=== SMOKE TEST: Queue Blocking ===");

  /* Small queue: capacity=2 */
  setDefaultQueueSizes(2, 1, 0);

  addPeer(kDefaultPeerSpec3);
  setupComponents();
  bringUpPeer(kPeerAddr3);
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));

  /* Add 3 routes with DIFFERENT communities = 3 BGP UPDATEs */
  XLOG(INFO, "Adding 3 BGP UPDATEs with capacity=2");
  injectLocalRoutesAtRuntime({"11.0.0.0/8"}, {"100:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.0.0.0/8")));
  injectLocalRoutesAtRuntime({"12.0.0.0/8"}, {"100:2"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("12.0.0.0/8")));
  injectLocalRoutesAtRuntime({"13.0.0.0/8"}, {"100:3"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.0.0.0/8")));

  /* Read all routes (order not guaranteed due to async processing) */
  EXPECT_TRUE(verifyRoutes(
      "v4",
      kPeerAddr3,
      {{.prefix = "11.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "100:1"},
       {.prefix = "12.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "100:2"},
       {.prefix = "13.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "100:3"}}));

  XLOG(INFO, "=== SMOKE TEST PASSED ===");
}

/*
 * Test: Single peer blocks and unblocks
 */
TEST_P(UpdateGroupBlockingTest, SinglePeerBlocks) {
  XLOG(INFO, "=== TEST: SinglePeerBlocks ===");

  XLOG(INFO, "Setting queue sizes: capacity=2, blocked_threshold=1");
  setDefaultQueueSizes(2, 1, 0);
  addPeer(kDefaultPeerSpec3);
  setupComponents();
  bringUpPeer(kPeerAddr3);
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));
  XLOG(INFO, "Peer3 established and received EoR");

  /*
   * Add 5 routes with DIFFERENT communities = 5 BGP UPDATEs - peer will block
   */
  XLOG(INFO, "Injecting 5 routes to cause blocking (queue capacity=2)");
  XLOG(INFO, "Injecting route 10.0.0.0/8");
  injectLocalRoutesAtRuntime({"10.0.0.0/8"}, {"500:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.0.0.0/8")));
  XLOGF(
      INFO,
      "Route 10.0.0.0/8 reached shadowRIB. Queue state: {}",
      (isPeerQueueBlocked(peerId3) ? "BLOCKED" : "UNBLOCKED"));

  XLOG(INFO, "Injecting route 20.0.0.0/8");
  injectLocalRoutesAtRuntime({"20.0.0.0/8"}, {"500:2"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.0.0.0/8")));
  XLOGF(
      INFO,
      "Route 20.0.0.0/8 reached shadowRIB. Queue state: {}",
      (isPeerQueueBlocked(peerId3) ? "BLOCKED" : "UNBLOCKED"));

  XLOG(INFO, "Injecting route 30.0.0.0/8");
  injectLocalRoutesAtRuntime({"30.0.0.0/8"}, {"500:3"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.0.0.0/8")));
  XLOGF(
      INFO,
      "Route 30.0.0.0/8 reached shadowRIB. Queue state: {}",
      (isPeerQueueBlocked(peerId3) ? "BLOCKED" : "UNBLOCKED"));

  XLOG(INFO, "Injecting route 40.0.0.0/8");
  injectLocalRoutesAtRuntime({"40.0.0.0/8"}, {"500:4"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("40.0.0.0/8")));
  XLOGF(
      INFO,
      "Route 40.0.0.0/8 reached shadowRIB. Queue state: {}",
      (isPeerQueueBlocked(peerId3) ? "BLOCKED" : "UNBLOCKED"));

  XLOG(INFO, "Injecting route 50.0.0.0/8");
  injectLocalRoutesAtRuntime({"50.0.0.0/8"}, {"500:5"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("50.0.0.0/8")));
  XLOGF(
      INFO,
      "Route 50.0.0.0/8 reached shadowRIB. Queue state: {}",
      (isPeerQueueBlocked(peerId3) ? "BLOCKED" : "UNBLOCKED"));

  /*
   * Use waitForPeerQueueBlocked for robust checking since the point-in-time
   * isPeerQueueBlocked check can be racy.
   */
  XLOG(INFO, "Checking if peer is blocked after all route injections");
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  XLOG(INFO, "Peer blocked as expected");

  /* Read all routes to unblock (order not guaranteed) */
  XLOG(INFO, "Reading all 5 routes to unblock peer");
  EXPECT_TRUE(verifyRoutes(
      "v4",
      kPeerAddr3,
      {{.prefix = "10.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "500:1"},
       {.prefix = "20.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "500:2"},
       {.prefix = "30.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "500:3"},
       {.prefix = "40.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "500:4"},
       {.prefix = "50.0.0.0",
        .prefixLen = 8,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "500:5"}}));

  XLOG(INFO, "All routes verified. Checking queue state...");
  EXPECT_FALSE(isPeerQueueBlocked(peerId3));
  XLOG(INFO, "Peer unblocked after verification. Queue state: UNBLOCKED");

  /* Phase 2: Withdraw all routes and verify */
  XLOG(INFO, "Phase 2: Withdrawing all routes");
  withdrawLocalRoutesAtRuntime(
      {"10.0.0.0/8", "20.0.0.0/8", "30.0.0.0/8", "40.0.0.0/8", "50.0.0.0/8"});

  std::vector<E2ETestFixture::WithdrawSpec> expectedWithdrawals = {
      {.prefix = "10.0.0.0", .prefixLen = 8},
      {.prefix = "20.0.0.0", .prefixLen = 8},
      {.prefix = "30.0.0.0", .prefixLen = 8},
      {.prefix = "40.0.0.0", .prefixLen = 8},
      {.prefix = "50.0.0.0", .prefixLen = 8}};

  EXPECT_TRUE(verifyRouteWithdraws("v4", kPeerAddr3, expectedWithdrawals));

  /* Phase 3: Re-add 2 routes */
  XLOG(INFO, "Phase 3: Re-adding 2 routes");
  injectLocalRoutesAtRuntime({"10.0.0.0/8"}, {"500:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.0.0.0/8")));
  injectLocalRoutesAtRuntime({"20.0.0.0/8"}, {"500:2"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.0.0.0/8")));

  std::vector<E2ETestFixture::VerifySpec> expectedReAdded = {
      {.prefix = "10.0.0.0",
       .prefixLen = 8,
       .expectedNexthop = getExpectedNexthop(kPeerAddr3),
       .expectedAsPath = "4200000001",
       .expectedCommunity = "500:1"},
      {.prefix = "20.0.0.0",
       .prefixLen = 8,
       .expectedNexthop = getExpectedNexthop(kPeerAddr3),
       .expectedAsPath = "4200000001",
       .expectedCommunity = "500:2"}};

  EXPECT_TRUE(verifyRoutes("v4", kPeerAddr3, expectedReAdded));

  /* Phase 4: Change attributes */
  XLOG(INFO, "Phase 4: Changing route attributes (community change)");
  injectLocalRoutesAtRuntime({"10.0.0.0/8"}, {"500:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.0.0.0/8")));

  std::vector<E2ETestFixture::VerifySpec> expectedChanged = {
      {.prefix = "10.0.0.0",
       .prefixLen = 8,
       .expectedNexthop = getExpectedNexthop(kPeerAddr3),
       .expectedAsPath = "4200000001",
       .expectedCommunity = "500:99"}};

  EXPECT_TRUE(verifyRoutes("v4", kPeerAddr3, expectedChanged));

  XLOG(INFO, "=== TEST PASSED ===");
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
