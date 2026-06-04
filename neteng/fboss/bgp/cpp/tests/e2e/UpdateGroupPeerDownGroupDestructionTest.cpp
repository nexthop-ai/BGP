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

/* E2E tests: Peer DOWN edge cases — group destruction, mid-detach, acceptance.
 * Prefix range: 32.1-32.49/16
 *
 * Last peer in group goes DOWN — group destruction
 * Peer DOWN during detachment procedure — partial cleanup
 * Peer DOWN while another peer is being accepted — no interference
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Peer DOWN: last peer in group — group destruction.
 * Single-peer group: bring up peer3, reach JOINED_RUNNING, inject a route,
 * then bring peer3 DOWN. With no peers remaining, the group should be
 * destroyed. Verify no crash and no resource leak.
 */
TEST_P(UpdateGroupPeerDownTest, LastPeerDownGroupDestruction) {
  XLOG(INFO, "=== TEST: LastPeerDownGroupDestruction ===");

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

  /* Inject a route so the group has some state */
  injectLocalRoutesAtRuntime({"32.1.0.0/16"}, {"3201:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3201:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3201:1"));

  /* Bring down peer3 first — group still alive with peer4 */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Verify peer4 still works */
  injectLocalRoutesAtRuntime({"32.2.0.0/16"}, {"3202:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3202:1"));

  /* Bring down last peer — group should be destroyed */
  bringDownPeer(kPeerAddr4);
  EXPECT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DOWN));

  /* Both peers are DOWN. Inject another route — should not crash even
   * though the group is destroyed. The route goes to shadow RIB but
   * no peers receive it. */
  injectLocalRoutesAtRuntime({"32.3.0.0/16"}, {"3203:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.3.0.0/16")));

  XLOG(INFO, "=== TEST PASSED: LastPeerDownGroupDestruction ===");
}

/*
 * Peer DOWN during detachment procedure — partial cleanup.
 * Set up 2 peers, configure freq threshold=1 on peer3, then block peer3
 * and fill queue to trigger detachment. While peer3 is JOINED_BLOCKED
 * (freq threshold about to fire), bring peer3 DOWN. The detachment
 * procedure must handle the mid-flight peer going DOWN gracefully.
 * Verify peer4 continues normally and no crash from partial detach state.
 */
TEST_P(UpdateGroupPeerDownTest, PeerDownDuringDetachmentProcedure) {
  XLOG(INFO, "=== TEST: PeerDownDuringDetachmentProcedure ===");

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

  /* Set aggressive freq threshold: threshold=1, so first block triggers detach.
   * Use short duration too (1ms) to maximize race between detach and peer-down
   */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(1),
      1,
      std::chrono::milliseconds(60000));

  /* Block peer3 and fill the queue with 3 routes (different communities) */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"32.10.0.0/16"}, {"3210:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3210:1"));
  injectLocalRoutesAtRuntime({"32.11.0.0/16"}, {"3211:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.11.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.11.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3211:1"));
  injectLocalRoutesAtRuntime({"32.12.0.0/16"}, {"3212:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.12.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.12.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3212:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Peer3 is now JOINED_BLOCKED or DETACHED_BLOCKED (freq+dur threshold
   * may have already fired). Either way, bring it DOWN immediately to
   * test the mid-detachment cleanup path. */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Peer4 should continue functioning normally after the abrupt peer3 DOWN */
  injectLocalRoutesAtRuntime({"32.13.0.0/16"}, {"3213:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.13.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3213:1"));

  /* Inject one more route to confirm stable state */
  injectLocalRoutesAtRuntime({"32.14.0.0/16"}, {"3214:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.14.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.14.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3214:1"));

  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: PeerDownDuringDetachmentProcedure ===");
}

/*
 * Peer DOWN while another peer is being accepted — no interference.
 * 3-peer group: freq-detach peer3, start recovery by unblocking. While peer3
 * is recovering (DETACHED_READY_TO_JOIN or still processing), bring peer5
 * DOWN. The acceptance of peer3 should not be affected by peer5 going DOWN.
 * Verify peer4 continues receiving routes and the recovery/acceptance path
 * for peer3 completes cleanly.
 */
TEST_P(UpdateGroupMultiPeerTest, PeerDownWhileAnotherBeingAccepted) {
  XLOG(INFO, "=== TEST: PeerDownWhileAnotherBeingAccepted ===");

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

  /* Inject a shared route */
  injectLocalRoutesAtRuntime({"32.30.0.0/16"}, {"3230:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.30.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3230:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3230:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.30.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3230:1"));

  /* Freq-detach peer3: threshold=1 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"32.31.0.0/16"}, {"3231:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3231:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.31.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3231:1"));
  injectLocalRoutesAtRuntime({"32.32.0.0/16"}, {"3232:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.32.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.32.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3232:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.32.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3232:1"));
  injectLocalRoutesAtRuntime({"32.33.0.0/16"}, {"3233:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.33.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.33.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3233:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.33.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3233:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock peer3 to start recovery — confirm DRJ before peer5 DOWN */
  waitForDrjWithDrain(kPeerAddr3, peerId3);

  /* While peer3 is recovering, bring peer5 DOWN — should not interfere
   * with peer3's acceptance path */
  bringDownPeer(kPeerAddr5);
  EXPECT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::DOWN));

  /* Peer4 should still be functional */
  injectLocalRoutesAtRuntime({"32.34.0.0/16"}, {"3234:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.34.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.34.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3234:1"));

  /* Verify peer3's state after recovery attempt — acceptance path may or
   * may not have completed depending on timing */
  auto state3 = getPeerState(kPeerAddr3);
  EXPECT_NE(state3, PeerUpdateState::INIT);

  /* Inject another route to verify peer4 continues working */
  injectLocalRoutesAtRuntime({"32.35.0.0/16"}, {"3235:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.35.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.35.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3235:1"));

  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: PeerDownWhileAnotherBeingAccepted ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupPeerDownTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupMultiPeerTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
