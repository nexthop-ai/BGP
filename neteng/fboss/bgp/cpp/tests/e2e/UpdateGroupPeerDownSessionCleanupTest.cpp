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
 * E2E tests for peer DOWN session cleanup and multi-peer blocking edge cases.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Peer DOWN, comes back, frequency window NOT carried over.
 * After peer goes DOWN and reconnects, the frequency counter should reset.
 * A single block cycle after reconnect should NOT trigger detachment
 * even with freq threshold=2 (which would have fired if old count carried
 * over).
 */
TEST_P(UpdateGroupPeerDownTest, PeerDownFreqWindowResets) {
  XLOG(INFO, "=== TEST: PeerDownFreqWindowResets ===");

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

  /* Set freq threshold=2, so 2 block cycles triggers detachment */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      2,
      std::chrono::milliseconds(60000));

  /* Block cycle #1 — should NOT detach (count=1, threshold=2) */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"30.1.0.0/16"}, {"3010:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3010:1"));

  /* Unblock — unblockPeer internally drains the bounded queue, so we verify
   * the peer processed the queued route by checking it returns to JR state */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Peer3 goes DOWN — this should reset frequency window */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Bring peer3 back UP — enters DETACHED_INIT_DUMP for existing group */
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);

  /* Drain init dump for reconnecting peer — init dump sends routes before
   * EoR markers, so drainPeerQueueCompletely consumes everything (routes +
   * EoRs) in one pass. Using higher maxRetries since async pipeline needs
   * time to push all init dump messages. */
  drainPeerQueueCompletely(peerId3, /*maxRetries=*/20, /*maxMessages=*/200);

  /* Set freq threshold again on reconnected peer */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      2,
      std::chrono::milliseconds(60000));

  /* Block cycle #1 after reconnect — if freq window carried over, this
   * would be count=2 and trigger detachment. With clean session, count=1. */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"30.2.0.0/16"}, {"3020:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3020:1"));

  /* Peer should NOT be detached — freq window was reset */
  auto state = getPeerState(kPeerAddr3);
  EXPECT_NE(state, PeerUpdateState::DETACHED_BLOCKED)
      << "Freq window should have reset after DOWN/UP cycle";

  unblockPeer(kPeerAddr3);

  XLOG(INFO, "=== TEST PASSED: PeerDownFreqWindowResets ===");
}

/*
 * Peer DOWN from P-JB when another peer also blocked — group stays WAITING.
 * With 3 peers, block peer3 and peer4, bring peer3 DOWN.
 * Group should remain in WAITING because peer4 is still blocked.
 */
TEST_P(UpdateGroupMultiPeerTest, PeerDownWhileAnotherBlocked) {
  XLOG(INFO, "=== TEST: PeerDownWhileAnotherBlocked ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  setupSlowPeerComponents(5, 3, 0);

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

  /* Set high threshold to avoid detachment */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      100,
      std::chrono::milliseconds(60000));
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      100,
      std::chrono::milliseconds(60000));

  /* Block both peer3 and peer4 */
  blockPeer(kPeerAddr3);
  blockPeer(kPeerAddr4);

  /* Inject 3 routes one at a time, verify peer5 gets each before next
   * inject. With queue (5,3,0), 3 routes fills blocked peers to hwm=3. */
  injectLocalRoutesAtRuntime({"30.7.0.0/16"}, {"3070:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.7.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.7.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3070:1"));
  injectLocalRoutesAtRuntime({"30.8.0.0/16"}, {"3080:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.8.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.8.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3080:1"));
  injectLocalRoutesAtRuntime({"30.9.0.0/16"}, {"3090:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.9.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.9.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3090:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));

  /* Bring DOWN peer3 while both are blocked */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Peer4 is still blocked — group should still be in WAITING-like state */
  EXPECT_TRUE(isPeerQueueBlocked(peerId4));

  /* Unblock peer4, then verify peer5 still functional */
  unblockPeer(kPeerAddr4);
  injectLocalRoutesAtRuntime({"30.10.0.0/16"}, {"3100:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.10.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3100:1"));

  XLOG(INFO, "=== TEST PASSED: PeerDownWhileAnotherBlocked ===");
}

/*
 * Peer DOWN: last in-sync peer, detached peers remain.
 * Detach peer3, then bring DOWN peer4 (last in-sync). Group has 0 in-sync.
 * Verify no crash.
 */
TEST_P(UpdateGroupPeerDownTest, LastInSyncPeerDown) {
  XLOG(INFO, "=== TEST: LastInSyncPeerDown ===");

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

  /* Detach peer3 via freq threshold=1 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"30.11.0.0/16"}, {"3110:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.11.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.11.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3110:1"));
  injectLocalRoutesAtRuntime({"30.12.0.0/16"}, {"3120:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.12.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.12.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3120:1"));
  injectLocalRoutesAtRuntime({"30.13.0.0/16"}, {"3130:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.13.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3130:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1);

  /* Now bring DOWN the last in-sync peer (peer4) */
  bringDownPeer(kPeerAddr4);
  EXPECT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DOWN));

  /* Group has 0 in-sync peers, only detached peer3 remains. No crash. */

  /* Bring down peer3 too for clean teardown */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  XLOG(INFO, "=== TEST PASSED: LastInSyncPeerDown ===");
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
