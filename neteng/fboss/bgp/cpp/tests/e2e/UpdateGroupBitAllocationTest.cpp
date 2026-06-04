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
 * E2E tests for BGP Update Group bit position allocation.
 * Tests that AdjRibGroup correctly allocates bit positions when peers
 * register/unregister from update groups.
 *
 * These tests verify the fix in AdjRibGroup.cpp that uses ConsumerBitManager
 * for bit allocation instead of the buggy bitToAdjRibs_.size() logic.
 *
 * Key scenarios tested:
 * 1. Single peer reconnect - verify bit is freed and can be reused
 * 2. Multiple peer flaps - verify bits are correctly managed across flaps
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupDistributionTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Test fixture for Update Group bit allocation tests.
 */
class UpdateGroupBitAllocationTest : public UpdateGroupDistributionTestBase {};

/*
 * Test: Single peer reconnect scenario.
 * Verifies that when a peer goes down and comes back up,
 * the update group correctly handles bit allocation.
 * This is a basic sanity test for the bit allocation fix.
 */
TEST_P(UpdateGroupBitAllocationTest, SinglePeerReconnect) {
  XLOG(INFO, "=== TEST START: SinglePeerReconnect ===");

  /* Add peer config */
  addPeer(kDefaultPeerSpec3);

  /* Add local route to verify peer receives updates */
  addLocalRoute("60.0.0.0/8", {"600:1"}, 100);

  /* Setup components */
  setupComponents();

  /* Wait for route to reach shadowRIB */
  auto routePrefix = folly::IPAddress::createNetwork("60.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(routePrefix))
      << "Route 60.0.0.0/8 did not reach shadowRIB in time";

  /* Bring peer up - should get bit position 0 */
  bringUpPeer(kPeerAddr3);
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);

  /* Verify peer receives route */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "60.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "600:1"));

  /* Wait for EoR */
  EXPECT_TRUE(waitForEoR(peerId3));

  /* Bring peer down - should free bit position 0 */
  XLOG(INFO, "Bringing peer3 down (freeing bit position)");
  bringDownPeer(kPeerAddr3);

  /* Bring peer back up - tests that bit management works on reconnect */
  XLOG(INFO, "Bringing peer3 back up");
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);

  /* Verify peer receives route again (proves it rejoined correctly) */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "60.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "600:1"));

  /* Wait for EoR after reconnect */
  EXPECT_TRUE(waitForEoR(peerId3));

  XLOG(INFO, "=== TEST PASSED: SinglePeerReconnect ===");
}

/*
 * Test: Two peers join, one leaves, verify group continues working.
 * Tests that bit allocation handles peer departure correctly.
 */
TEST_P(UpdateGroupBitAllocationTest, TwoPeersOneLeavesGroupContinues) {
  XLOG(INFO, "=== TEST START: TwoPeersOneLeavesGroupContinues ===");

  /* Add two peers */
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  /* Add local route */
  addLocalRoute("62.0.0.0/8", {"620:1"}, 100);

  setupComponents();

  /* Wait for route to reach shadowRIB */
  auto routePrefix = folly::IPAddress::createNetwork("62.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(routePrefix))
      << "Route 62.0.0.0/8 did not reach shadowRIB in time";

  /* Bring up both peers */
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Verify both peers receive route */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "62.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "620:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "62.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "620:1"));

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));

  /* Bring peer3 down - peer4 should continue working */
  XLOG(INFO, "Bringing peer3 down, peer4 should continue receiving updates");
  bringDownPeer(kPeerAddr3);

  /* Inject a new route - only peer4 should receive it */
  injectLocalRoutesAtRuntime({"63.0.0.0/8"}, {"630:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("63.0.0.0/8")));

  /* Verify peer4 receives the new route */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "63.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "630:1"));

  XLOG(INFO, "=== TEST PASSED: TwoPeersOneLeavesGroupContinues ===");
}

/*
 * Test: Multiple peer flaps verify group stability.
 * Peers go down and come back up multiple times.
 * Verifies that bit positions are correctly managed across flaps.
 */
TEST_P(UpdateGroupBitAllocationTest, MultiplePeerFlaps) {
  XLOG(INFO, "=== TEST START: MultiplePeerFlaps ===");

  /* Add two peers */
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  /* Add local route */
  addLocalRoute("64.0.0.0/8", {"640:1"}, 100);

  setupComponents();

  /* Wait for route to reach shadowRIB */
  auto routePrefix = folly::IPAddress::createNetwork("64.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(routePrefix))
      << "Route 64.0.0.0/8 did not reach shadowRIB in time";

  /* Bring up both peers */
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Verify both peers receive route */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "64.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "640:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "64.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "640:1"));

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));

  /* Flap cycle 1: Both peers down and back up */
  XLOG(INFO, "Flap cycle 1: both peers down then up");
  bringDownPeer(kPeerAddr3);
  bringDownPeer(kPeerAddr4);
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Verify both peers receive routes after flap */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "64.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "640:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "64.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "640:1"));

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));

  /* Verify runtime routes work after flaps */
  injectLocalRoutesAtRuntime({"65.0.0.0/8"}, {"650:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("65.0.0.0/8")));

  std::vector<folly::IPAddress> bothPeers = {kPeerAddr3, kPeerAddr4};
  for (const auto& peer : bothPeers) {
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        "65.0.0.0",
        8,
        peer,
        getExpectedNexthop(peer),
        "4200000001",
        "650:1"));
  }

  XLOG(INFO, "=== TEST PASSED: MultiplePeerFlaps ===");
}

/*
 * Instantiate tests for both serialization modes.
 */
INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupBitAllocationTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
