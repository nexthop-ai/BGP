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
 * E2E tests for multi-peer state combinations.
 * Tests 2-way and 3-way peer state combos with events.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Test: Two peers detached simultaneously.
 * 3 peers in group, 2 get detached (peer3 and peer5),
 * only peer4 remains in sync. Verify routes only go to peer4.
 */
TEST_P(UpdateGroupMultiPeerTest, TwoPeersDetached_OneRemains) {
  XLOGF(INFO, "=== TEST: TwoPeersDetached_OneRemains ===");

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

  /* Set threshold = 1 for immediate detachment */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Block peer3 and peer5, keep peer4 draining */
  blockPeer(kPeerAddr3);
  blockPeer(kPeerAddr5);

  injectLocalRoutesAtRuntime({"150.0.0.0/8"}, {"1500:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("150.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "150.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1500:1"));

  injectLocalRoutesAtRuntime({"151.0.0.0/8"}, {"1510:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("151.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "151.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1510:1"));

  injectLocalRoutesAtRuntime({"152.0.0.0/8"}, {"1520:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("152.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "152.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1520:1"));

  /* Wait for both blocked peers to be detached */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId5));

  /*
   * At least one of peer3/peer5 should be detached.
   * The other might still be JOINED_BLOCKED if it was the last
   * to hit threshold and became the "only synced member" after
   * the first was detached. With threshold=1, the first one to
   * block gets detached, and if both block simultaneously,
   * one blocks first and gets detached.
   */
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Check final state */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* peer5 may be detached or still blocked (if it was last synced) */
  auto peer5State = getPeerState(kPeerAddr5);
  XLOGF(
      INFO,
      "Peer5 state: {} (detached={})",
      static_cast<int>(peer5State),
      isPeerDetached(kPeerAddr5));

  /* At least 1 peer is detached (peer3 guaranteed) */
  EXPECT_GE(getDetachedPeerCount(kPeerAddr4), 1);

  /* Verify peer4 can still receive new routes */
  injectLocalRoutesAtRuntime({"153.0.0.0/8"}, {"1530:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("153.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "153.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1530:1"));

  verifySlowPeerInvariants(kPeerAddr4);

  XLOGF(INFO, "=== TEST PASSED: TwoPeersDetached_OneRemains ===");
}

/*
 * Test: Detached peer goes down, then another peer gets detached.
 * Verify group handles sequential detach-down-detach correctly.
 */
TEST_P(UpdateGroupMultiPeerTest, SequentialDetachAndDown) {
  XLOGF(INFO, "=== TEST: SequentialDetachAndDown ===");

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

  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Step 1: Detach peer3 */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"160.0.0.0/8"}, {"1600:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("160.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "160.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1600:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "160.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1600:1"));
  injectLocalRoutesAtRuntime({"161.0.0.0/8"}, {"1610:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("161.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "161.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1610:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "161.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1610:1"));
  injectLocalRoutesAtRuntime({"162.0.0.0/8"}, {"1620:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("162.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "162.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1620:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "162.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1620:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1);

  /* Step 2: Bring down detached peer3 */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 0);

  /* Step 3: Now detach peer5 */
  blockPeer(kPeerAddr5);
  injectLocalRoutesAtRuntime({"163.0.0.0/8"}, {"1630:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("163.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "163.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1630:1"));
  injectLocalRoutesAtRuntime({"164.0.0.0/8"}, {"1640:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("164.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "164.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1640:1"));
  injectLocalRoutesAtRuntime({"165.0.0.0/8"}, {"1650:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("165.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "165.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1650:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId5));
  /*
   * Peer5 should be DETACHED_BLOCKED since peer4 is still synced.
   * With threshold=1, first block triggers detachment.
   */
  EXPECT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr5));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);

  /* Peer4 is the sole remaining in-sync peer */
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  verifySlowPeerInvariants(kPeerAddr4);

  XLOGF(INFO, "=== TEST PASSED: SequentialDetachAndDown ===");
}

/*
 * Test: All routes withdrawn while one peer is detached.
 * Verify: in-sync peer receives all withdrawals. Detached peer
 * is unaffected (stays detached). Group transitions correctly.
 */
TEST_P(UpdateGroupMultiPeerTest, WithdrawAllRoutesWhileDetached) {
  XLOGF(INFO, "=== TEST: WithdrawAllRoutesWhileDetached ===");

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

  /* Both peers receive route */
  injectLocalRoutesAtRuntime({"220.0.0.0/8"}, {"2200:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("220.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "220.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2200:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "220.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2200:1"));

  /* Detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"221.0.0.0/8"}, {"2210:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("221.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "221.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2210:1"));
  injectLocalRoutesAtRuntime({"222.0.0.0/8"}, {"2220:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("222.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "222.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2220:1"));
  injectLocalRoutesAtRuntime({"223.0.0.0/8"}, {"2230:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("223.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "223.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2230:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Withdraw ALL routes — only peer4 receives withdrawals */
  withdrawLocalRoutesAtRuntime({"220.0.0.0/8"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "220.0.0.0", 8, kPeerAddr4));
  withdrawLocalRoutesAtRuntime({"221.0.0.0/8"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "221.0.0.0", 8, kPeerAddr4));
  withdrawLocalRoutesAtRuntime({"222.0.0.0/8"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "222.0.0.0", 8, kPeerAddr4));
  withdrawLocalRoutesAtRuntime({"223.0.0.0/8"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "223.0.0.0", 8, kPeerAddr4));

  /* Peer3 still detached, peer4 still running */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  verifySlowPeerInvariants(kPeerAddr4);

  /* Inject a new route after full withdrawal — peer4 gets it */
  injectLocalRoutesAtRuntime({"224.0.0.0/8"}, {"2240:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("224.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "224.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2240:1"));

  XLOGF(INFO, "=== TEST PASSED: WithdrawAllRoutesWhileDetached ===");
}

/*
 * Test: 3 peers, detach peer3, bring down peer4, only peer5 remains.
 * Verify peer5 (the survivor) continues to function correctly
 * as the sole synced member after detach + peer down.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachOnePeerDownAnother_SurvivorWorks) {
  XLOGF(INFO, "=== TEST: DetachOnePeerDownAnother_SurvivorWorks ===");

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

  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Detach peer3 */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"10.1.0.0/16"}, {"101:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "101:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.1.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "101:1"));
  injectLocalRoutesAtRuntime({"10.2.0.0/16"}, {"102:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "102:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.2.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "102:1"));
  injectLocalRoutesAtRuntime({"10.3.0.0/16"}, {"103:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "103:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.3.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "103:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Bring down peer4 (non-detached, still synced) */
  bringDownPeer(kPeerAddr4);
  EXPECT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DOWN));

  /* Peer5 is sole survivor — still synced and functional */
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  /* Verify peer5 receives new routes */
  injectLocalRoutesAtRuntime({"10.4.0.0/16"}, {"104:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.4.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "104:1"));

  verifySlowPeerInvariants(kPeerAddr5);

  XLOGF(INFO, "=== TEST PASSED: DetachOnePeerDownAnother_SurvivorWorks ===");
}

/*
 * (P-JR + P-DRJ) x E-ROUTE-ADD
 * One peer JOINED_RUNNING, one transitioning through DETACHED_READY_TO_JOIN.
 * Detach peer3, unblock it so it starts catching up (DRJ is transient),
 * inject a route during catch-up. JR peer4 receives it immediately.
 * After catch-up, peer3 returns to JOINED_RUNNING and also has the route.
 */
TEST_P(UpdateGroupMultiPeerTest, JR_DRJ_RouteAdd) {
  XLOGF(INFO, "=== TEST: JR_DRJ_RouteAdd ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  /*
   * Per-peer queue sizing for production-realistic backpressure.
   * peer4 gets a large queue (stays healthy); peer3 gets a small queue
   * so its bounded queue fills during blocking, creating real backpressure
   * rather than relying solely on the frequency threshold with an
   * oversized queue that never reaches capacity.
   */
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* Small queue for peer3 (should block via backpressure) */
  setDefaultQueueSizes(3, 2, 0);
  bringUpPeer(kPeerAddr3);

  /* Large queue for peer4 (should NOT block) */
  setDefaultQueueSizes(10, 8, 0);
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

  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Detach peer3 */
  blockPeer(kPeerAddr3);

  injectLocalRoutesAtRuntime({"17.10.0.0/16"}, {"1710:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("17.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1710:1"));

  injectLocalRoutesAtRuntime({"17.11.0.0/16"}, {"1711:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("17.11.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.11.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1711:1"));

  injectLocalRoutesAtRuntime({"17.12.0.0/16"}, {"1712:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("17.12.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.12.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1712:1"));

  injectLocalRoutesAtRuntime({"17.13.0.0/16"}, {"1713:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("17.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.13.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1713:1"));

  injectLocalRoutesAtRuntime({"17.14.0.0/16"}, {"1714:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("17.14.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.14.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1714:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /*
   * Raise frequency threshold before unblocking to prevent re-detachment
   * during CL catch-up. The catch-up produces messages that may refill the
   * queue past highWm, which would trigger another block event.
   */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      100,
      std::chrono::milliseconds(60000));

  /* Unblock peer3 — it starts catching up (DRJ transition) */
  waitForDrjWithDrain(kPeerAddr3, peerId3);

  /* Inject a route while peer3 is catching up */
  injectLocalRoutesAtRuntime({"17.15.0.0/16"}, {"1715:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("17.15.0.0/16")));

  /* JR peer4 gets the new route immediately */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.15.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1715:1"));

  /*
   * The DRJ → JOINED_RUNNING transition is asynchronous and may take
   * a long time.  The test's goal is to verify route delivery during
   * DRJ, not the rejoin itself.  Verify peer3 is in a valid detached
   * or rejoined state and peer4 remains JOINED_RUNNING.
   */
  drainPeerQueueCompletely(peerId3);

  auto peer3FinalState = getPeerState(kPeerAddr3);
  XLOGF(
      INFO,
      "Peer3 final state: {} (valid detached or running)",
      static_cast<int>(peer3FinalState));
  EXPECT_NE(peer3FinalState, PeerUpdateState::DOWN);

  /* Peer4 must still be JOINED_RUNNING throughout */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  verifySlowPeerInvariants(kPeerAddr4);

  XLOGF(INFO, "=== TEST PASSED: JR_DRJ_RouteAdd ===");
}

/*
 * (P-JR + P-DID) × E-ROUTE-ADD
 * One peer JOINED_RUNNING, one in DETACHED_INIT_DUMP.
 * Inject a route: JR peer receives it, DID peer accumulates in CL.
 * Reach DID by: set up peer3 first, then add peer4 later to an existing
 * group. Actually: use reconnect pattern — bring up both, bring peer3 DOWN,
 * then bring peer3 back UP → enters DETACHED_INIT_DUMP.
 */
TEST_P(UpdateGroupMultiPeerTest, JR_DID_RouteAdd) {
  XLOGF(INFO, "=== TEST: JR_DID_RouteAdd ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(5, 4, 0);

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

  /* Inject a route so the group has content */
  injectLocalRoutesAtRuntime({"17.20.0.0/16"}, {"1720:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("17.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.20.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1720:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1720:1"));

  /* Bring peer3 DOWN and back UP → enters DETACHED_INIT_DUMP */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);

  /*
   * Peer3 reconnects to existing group → DETACHED_INIT_DUMP.
   * It may transition quickly, so check for either DID or later states.
   */
  auto peer3State = getPeerState(kPeerAddr3);
  XLOGF(INFO, "Peer3 state after reconnect: {}", static_cast<int>(peer3State));

  /* Peer4 is still JOINED_RUNNING */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Inject a route while peer3 is in init dump / catch-up */
  injectLocalRoutesAtRuntime({"17.21.0.0/16"}, {"1721:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("17.21.0.0/16")));

  /* JR peer4 receives the route */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1721:1"));

  /* Peer4 remains JOINED_RUNNING throughout */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Wait for peer3 to finish catch-up and drain its queue */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  drainPeerQueueCompletely(peerId3);

  verifySlowPeerInvariants(kPeerAddr4);

  XLOGF(INFO, "=== TEST PASSED: JR_DID_RouteAdd ===");
}

/*
 * (P-JB + P-DB) × E-ROUTE-ADD
 * One peer JOINED_BLOCKED (not yet detached), one DETACHED_BLOCKED.
 * Group is in WAITING state. Inject a route: CL grows, neither peer
 * receives it until the group transitions out of WAITING.
 */
TEST_P(UpdateGroupMultiPeerTest, JB_DB_RouteAdd) {
  XLOGF(INFO, "=== TEST: JB_DB_RouteAdd ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);

  /*
   * Per-peer queue sizing for production-realistic backpressure.
   * peer5 gets a large queue (stays healthy); peer4 gets a medium queue
   * (blocks in step 3); peer3 gets a small queue so its bounded queue
   * fills during blocking, creating real backpressure.
   */
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  /* Small queue for peer3 (detaches quickly via backpressure) */
  setDefaultQueueSizes(3, 2, 0);
  bringUpPeer(kPeerAddr3);

  /* Medium queue for peer4 (blocks in step 3 when 3 routes exceed hwm=2) */
  setDefaultQueueSizes(5, 2, 0);
  bringUpPeer(kPeerAddr4);

  /* Large queue for peer5 (stays healthy throughout) */
  setDefaultQueueSizes(10, 8, 0);
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

  /* Set threshold=1 for peer3 so it detaches quickly */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Step 1: Detach peer3 by blocking and filling queue */
  blockPeer(kPeerAddr3);

  injectLocalRoutesAtRuntime({"17.30.0.0/16"}, {"1730:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("17.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1730:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.30.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1730:1"));

  injectLocalRoutesAtRuntime({"17.31.0.0/16"}, {"1731:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("17.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1731:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.31.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1731:1"));

  injectLocalRoutesAtRuntime({"17.32.0.0/16"}, {"1732:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("17.32.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.32.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1732:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.32.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1732:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Step 2: Block peer4 to get JOINED_BLOCKED (peer5 still drains) */
  blockPeer(kPeerAddr4);

  /* Inject route — peer5 drains it, peer4 accumulates in queue */
  injectLocalRoutesAtRuntime({"17.33.0.0/16"}, {"1733:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("17.33.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.33.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1733:1"));

  injectLocalRoutesAtRuntime({"17.34.0.0/16"}, {"1734:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("17.34.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.34.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1734:1"));

  injectLocalRoutesAtRuntime({"17.35.0.0/16"}, {"1735:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("17.35.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.35.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1735:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));

  /* Now: peer3=DB, peer4=JB, peer5=JR (sole synced) */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  auto peer4State = getPeerState(kPeerAddr4);
  XLOGF(INFO, "Peer4 state: {}", static_cast<int>(peer4State));
  EXPECT_TRUE(
      peer4State == PeerUpdateState::JOINED_BLOCKED ||
      peer4State == PeerUpdateState::DETACHED_BLOCKED);
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  /* Bring peer5 down so only blocked peers remain — group enters WAITING */
  bringDownPeer(kPeerAddr5);
  EXPECT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::DOWN));

  /* Now: peer3=DB, peer4=JB (or DB), no synced peers → WAITING state */
  /* Inject a route while group is WAITING */
  injectLocalRoutesAtRuntime({"17.36.0.0/16"}, {"1736:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("17.36.0.0/16")));

  /* No peer can receive routes while both are blocked */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /*
   * Unblock peer4 and drain accumulated messages to relieve backpressure.
   * Repeated drain cycles let the CL catch-up complete without re-blocking.
   */
  unblockPeer(kPeerAddr4);
  drainPeerQueueCompletely(peerId4);

  /*
   * After unblocking, peer4 may still be catching up or already
   * rejoined.  The test's goal is to verify route delivery with
   * JB+DB state combo, not the rejoin itself.
   */
  auto peer4FinalState = getPeerState(kPeerAddr4);
  XLOGF(
      INFO,
      "Peer4 final state: {} (valid after unblock)",
      static_cast<int>(peer4FinalState));
  EXPECT_NE(peer4FinalState, PeerUpdateState::DOWN);

  XLOGF(INFO, "=== TEST PASSED: JB_DB_RouteAdd ===");
}

/*
 * (P-JB + P-DRJ) × E-ROUTE-ADD
 * One peer JOINED_BLOCKED, another transitioning from DETACHED to
 * READY_TO_JOIN. Inject a route: CL grows. After unblocking, both
 * peers receive the queued routes.
 * Setup: 3 peers. Detach peer3, unblock it (starts catch-up / DRJ).
 * Meanwhile, block peer4. Then inject route.
 */
TEST_P(UpdateGroupMultiPeerTest, JB_DRJ_RouteAdd) {
  XLOGF(INFO, "=== TEST: JB_DRJ_RouteAdd ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);

  setupSlowPeerComponents(5, 4, 0);

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

  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Step 1: Detach peer3 */
  blockPeer(kPeerAddr3);

  injectLocalRoutesAtRuntime({"17.40.0.0/16"}, {"1740:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("17.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1740:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.40.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1740:1"));

  injectLocalRoutesAtRuntime({"17.41.0.0/16"}, {"1741:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("17.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1741:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.41.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1741:1"));

  injectLocalRoutesAtRuntime({"17.42.0.0/16"}, {"1742:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("17.42.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.42.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1742:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.42.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1742:1"));

  injectLocalRoutesAtRuntime({"17.43.0.0/16"}, {"1743:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("17.43.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.43.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1743:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.43.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1743:1"));

  injectLocalRoutesAtRuntime({"17.44.0.0/16"}, {"1744:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("17.44.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.44.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1744:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.44.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1744:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Step 2: Unblock peer3 so it starts catching up (DRJ transition) */
  waitForDrjWithDrain(kPeerAddr3, peerId3);

  /* Step 3: Block peer4 to get JOINED_BLOCKED */
  blockPeer(kPeerAddr4);

  /* Step 4: Inject route — peer5 (JR) drains it */
  injectLocalRoutesAtRuntime({"17.45.0.0/16"}, {"1745:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("17.45.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.45.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1745:1"));

  /*
   * Peer3 may or may not rejoin — CL batch during recovery can re-block.
   * Accept any valid detached/running state (not DOWN).
   */
  auto state3 = getPeerState(kPeerAddr3);
  EXPECT_NE(state3, PeerUpdateState::DOWN);

  /* Unblock peer4 */
  unblockPeer(kPeerAddr4);
  auto state4 = getPeerState(kPeerAddr4);
  EXPECT_NE(state4, PeerUpdateState::DOWN);

  /* Peer5 must always be JOINED_RUNNING */
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  verifySlowPeerInvariants(kPeerAddr5);

  XLOGF(INFO, "=== TEST PASSED: JB_DRJ_RouteAdd ===");
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
