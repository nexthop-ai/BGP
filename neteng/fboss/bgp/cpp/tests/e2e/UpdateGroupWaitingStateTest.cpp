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
 * E2E tests: Group Waiting State x Event Matrix
 * Tests for G-WAITING group state against various events.
 *
 * Test plan:
 * https://docs.google.com/document/d/11lBp_Q_i6UYocI3meYbI3sUShZzZsu6Qlq8iSCdVXRc
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * G-WAITING x E-ROUTE-ADD
 * Route arrives while group is in WAITING state (PL draining).
 * The new route should be queued as a CL item and processed after
 * the current PL drain completes.
 *
 * Strategy: Block peer3 to cause WAITING state, inject new routes.
 * Drain peer4 inline so it doesn't block. The new route gets queued
 * in CL and delivered to peer4 after PL drain.
 */
TEST_P(UpdateGroupDetachmentTest, GWaiting_RouteAdd) {
  XLOG(INFO, "=== TEST: GWaiting_RouteAdd ===");

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

  /* Block peer3 to get the group into WAITING */
  blockPeer(kPeerAddr3);

  /* Inject first route: group enters WAITING because peer3 is blocked */
  injectLocalRoutesAtRuntime({"12.9.0.0/16"}, {"1209:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("12.9.0.0/16")));
  /* Drain peer4 to prevent it blocking */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "12.9.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1209:1"));

  /* Inject another route while group is WAITING (peer3 still blocked) */
  injectLocalRoutesAtRuntime({"12.10.0.0/16"}, {"1210:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("12.10.0.0/16")));
  /* Drain peer4 */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "12.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1210:1"));

  /* Peer3 should be blocked (still hasn't drained) */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Unblock peer3 so group can finish */
  unblockPeer(kPeerAddr3);

  /* After unblock, peer3 should get the routes and return to running */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));

  XLOG(INFO, "=== TEST PASSED: GWaiting_RouteAdd ===");
}
/*
 * G-WAITING x E-SLOW-FREQ
 * Frequency threshold exceeded during PL drain -> peer gets detached.
 * After detachment, the group should unblock and continue.
 */
TEST_P(UpdateGroupDetachmentTest, GWaiting_SlowFreq) {
  XLOG(INFO, "=== TEST: GWaiting_SlowFreq ===");

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

  /* Set frequency threshold = 1 for immediate detach on block */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Block peer3, inject routes, drain peer4 */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"12.13.0.0/16"}, {"1213:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("12.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "12.13.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1213:1"));

  injectLocalRoutesAtRuntime({"12.14.0.0/16"}, {"1214:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("12.14.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "12.14.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1214:1"));

  injectLocalRoutesAtRuntime({"12.15.0.0/16"}, {"1215:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("12.15.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "12.15.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1215:1"));

  /* Peer3 should have been detached */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));

  /* Group should no longer be blocked on peer3 */
  auto group = getUpdateGroupForPeer(kPeerAddr3);
  ASSERT_NE(group, nullptr);
  EXPECT_FALSE(group->hasBlockedPeers())
      << "After detachment, group should not have blocked peers";

  verifySlowPeerInvariants(kPeerAddr3);

  /* Peer4 should still be running and receiving routes */
  injectLocalRoutesAtRuntime({"12.16.0.0/16"}, {"1216:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("12.16.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "12.16.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1216:1"));

  XLOG(INFO, "=== TEST PASSED: GWaiting_SlowFreq ===");
}

/*
 * G-WAITING x E-PEER-DOWN
 * Peer goes down during PL drain (group in WAITING).
 * Group should update bitmaps and continue serving remaining peers.
 */
TEST_P(UpdateGroupDetachmentTest, GWaiting_PeerDown) {
  XLOG(INFO, "=== TEST: GWaiting_PeerDown ===");

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

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId5));
  EXPECT_TRUE(waitForEoR(peerId5));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));
  EXPECT_EQ(getGroupMemberCount(kPeerAddr3), 3);

  /* Block peer3 to get group into WAITING */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"12.17.0.0/16"}, {"1217:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("12.17.0.0/16")));
  /* Drain peer4 and peer5 */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "12.17.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1217:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "12.17.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1217:1"));

  injectLocalRoutesAtRuntime({"12.18.0.0/16"}, {"1218:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("12.18.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "12.18.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1218:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "12.18.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1218:1"));

  /* Peer3 should be blocked */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Bring down peer3 while group is in WAITING */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Group should continue with peer4 and peer5 */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));
  EXPECT_EQ(getGroupMemberCount(kPeerAddr4), 2);

  /* Verify remaining peers still receive routes */
  injectLocalRoutesAtRuntime({"12.19.0.0/16"}, {"1219:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("12.19.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "12.19.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1219:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "12.19.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1219:1"));

  XLOG(INFO, "=== TEST PASSED: GWaiting_PeerDown ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupDetachmentTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
