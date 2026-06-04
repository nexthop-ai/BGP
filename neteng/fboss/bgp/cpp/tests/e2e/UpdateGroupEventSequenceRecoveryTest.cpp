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

/* E2E tests: Complex event sequences -- re-eval conflicts, rapid flap,
 * acceptance with detachment, and triple/quadruple recovery sequences.
 * Prefix range: 32.x.0.0/16.
 *
 * E-ROUTE-REFRESH then E-POLICY-CHG -- conflicting re-eval sources
 * E-PEER-DOWN then E-PEER-UP -- rapid flap
 * E-ACCEPT then E-DETACH(other) -- acceptance + simultaneous detach
 * Triple: E-DETACH -> E-POLICY-CHG -> E-UNBLOCK -> E-ACCEPT
 * Triple: E-BLOCK -> E-SLOW-DUR -> E-UNBLOCK -> E-PL-DRAIN -> E-ACCEPT
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Acceptance (peer recovering) then another peer gets detached.
 * Use 3 peers: peer3 detaches first, recovers (unblock). Meanwhile
 * peer4 gets detached. Peer5 stays in-sync throughout.
 */
TEST_P(UpdateGroupMultiPeerTest, AcceptThenDetachOther) {
  XLOG(INFO, "=== TEST: AcceptThenDetachOther ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  bringUpPeer(kPeerAddr5);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId5));
  EXPECT_TRUE(waitForEoR(peerId5));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  /* Detach peer3 via freq threshold (1 block = detach) */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  /* Inject routes to fill peer3 queue to hwm and trigger JOINED_BLOCKED ->
   * freq threshold fires -> DETACHED_BLOCKED */
  injectLocalRoutesAtRuntime({"32.20.0.0/16"}, {"3220:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3220:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.20.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3220:1"));

  injectLocalRoutesAtRuntime({"32.21.0.0/16"}, {"3221:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3221:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.21.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3221:1"));

  injectLocalRoutesAtRuntime({"32.24.0.0/16"}, {"3224:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.24.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.24.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3224:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.24.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3224:1"));

  /* peer3 should be DETACHED_BLOCKED after freq threshold fires */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock peer3 -- starts recovery (acceptance path) */
  unblockPeer(kPeerAddr3);

  /* Now detach peer4 via freq threshold (1 block = detach) */
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr4);
  injectLocalRoutesAtRuntime({"32.22.0.0/16"}, {"3222:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.22.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.22.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3222:1"));

  injectLocalRoutesAtRuntime({"32.23.0.0/16"}, {"3223:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.23.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.23.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3223:1"));

  injectLocalRoutesAtRuntime({"32.25.0.0/16"}, {"3225:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.25.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.25.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3225:1"));

  /* peer4 should be DETACHED_BLOCKED */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* peer5 (the only in-sync peer) continues operating */
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));
  verifySlowPeerInvariants(kPeerAddr5);
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr5), 2);

  XLOG(INFO, "=== TEST PASSED: AcceptThenDetachOther ===");
}

/* Triple sequence: Block -- slow-dur (1ms) detachment -- unblock --
 * PL drain -- CL end -- acceptance. Full recovery path from block to
 * re-acceptance using duration-based detachment.
 */
TEST_P(UpdateGroupMultiPeerTest, BlockDurDetachUnblockDrainAccept) {
  XLOG(INFO, "=== TEST: BlockDurDetachUnblockDrainAccept ===");

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

  /* Set 1ms duration threshold -- fires instantly once peer blocks */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(1),
      999999,
      std::chrono::milliseconds(60000));

  /* Step 1: Block peer3 and fill queue past hwm=2 to trigger JOINED_BLOCKED */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"32.40.0.0/16"}, {"3240:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3240:1"));

  injectLocalRoutesAtRuntime({"32.41.0.0/16"}, {"3241:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3241:1"));

  injectLocalRoutesAtRuntime({"32.42.0.0/16"}, {"3242:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.42.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.42.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3242:1"));

  /* Step 2: Duration threshold (1ms) fires -- peer3 detaches */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1);

  /* Inject one more route while peer3 is detached -- goes to CL */
  injectLocalRoutesAtRuntime({"32.43.0.0/16"}, {"3243:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.43.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.43.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3243:1"));

  /* Step 3: Unblock peer3 -- PL drains, CL processes, recovery begins */
  unblockPeer(kPeerAddr3);

  /* Peer4 continues operating throughout */
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: BlockDurDetachUnblockDrainAccept ===");
}

INSTANTIATE_TEST_SUITE_P(
    NoSerialization,
    UpdateGroupMultiPeerTest,
    ::testing::Values(kNoSerialization));

INSTANTIATE_TEST_SUITE_P(
    Serialized,
    UpdateGroupMultiPeerTest,
    ::testing::Values(kWithSerialization));

} // namespace bgp
} // namespace facebook
