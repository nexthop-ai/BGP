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
 * E2E tests: P-DOWN idempotent and P-INIT EoR / P-JR N/A events.
 *
 * Prefix range: 39.x.0.0/16
 *
 * Tests:
 *   P-DOWN × E-PEER-DOWN — already down, idempotent
 *   P-INIT × E-EOR — EoR during init, queued for processing
 *   P-JR × E-UNBLOCK — N/A (already running/unblocked)
 *   P-JR × E-SLOW-FREQ — N/A (not blocked)
 *   P-JR × E-PL-DRAIN — PL drained, peer received all routes
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* P-DOWN × E-PEER-DOWN
 * bringDownPeer on an already-DOWN peer is truly idempotent — no crash. */
TEST_P(UpdateGroupMultiPeerTest, PeerDown_PeerDown_Idempotent) {
  XLOG(INFO, "=== TEST: PeerDown_PeerDown_Idempotent ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

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

  /* Bring peer3 DOWN */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Bring peer3 DOWN again — should be idempotent, no crash */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* peer4 still works */
  injectLocalRoutesAtRuntime({"39.1.0.0/16"}, {"3901:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("39.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "39.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3901:1"));

  XLOG(INFO, "=== TEST PASSED: PeerDown_PeerDown_Idempotent ===");
}

/* P-INIT × E-EOR
 * EoR received during init. Both peers are in INIT, inject a route (CL item),
 * then send EoR to both. Both transition to JOINED_RUNNING and receive the
 * CL item. */
TEST_P(UpdateGroupSlowPeerDetectionTest, Init_Eor) {
  XLOG(INFO, "=== TEST: Init_Eor ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /* Inject a route while both peers in INIT — goes to CL */
  injectLocalRoutesAtRuntime({"39.2.0.0/16"}, {"3902:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("39.2.0.0/16")));

  /* Send EoR to both peers — transitions INIT → JOINED_RUNNING */
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Both peers receive the CL item from init */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "39.2.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3902:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "39.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3902:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: Init_Eor ===");
}

/* P-JR × E-UNBLOCK
 * unblockPeer on a peer that is already JOINED_RUNNING (not blocked) is
 * a no-op. */
TEST_P(UpdateGroupMultiPeerTest, JoinedRunning_Unblock_Noop) {
  XLOG(INFO, "=== TEST: JoinedRunning_Unblock_Noop ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

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

  /* unblockPeer on a non-blocked peer — should be no-op */
  unblockPeer(kPeerAddr3);
  EXPECT_FALSE(isPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Route delivery still works */
  injectLocalRoutesAtRuntime({"39.3.0.0/16"}, {"3903:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("39.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "39.3.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3903:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "39.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3903:1"));

  XLOG(INFO, "=== TEST PASSED: JoinedRunning_Unblock_Noop ===");
}

/* P-JR × E-SLOW-FREQ
 * Aggressive freq threshold (1 block) set on a running peer that never blocks.
 * Peer stays JOINED_RUNNING because no block event fires. */
TEST_P(UpdateGroupMultiPeerTest, JoinedRunning_SlowFreq_NeverFires) {
  XLOG(INFO, "=== TEST: JoinedRunning_SlowFreq_NeverFires ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

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

  /* Set aggressive freq threshold — but peer never blocks */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Inject routes — both peers receive normally, no blocking */
  injectLocalRoutesAtRuntime({"39.4.0.0/16"}, {"3904:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("39.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "39.4.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3904:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "39.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3904:1"));

  /* Peer3 stays JOINED_RUNNING — no detachment */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));

  XLOG(INFO, "=== TEST PASSED: JoinedRunning_SlowFreq_NeverFires ===");
}

/* P-JR × E-PL-DRAIN
 * Group PL drained — peer received all routes. Use small queue to make PL
 * drain observable. After routes injected, both peers receive them. */
TEST_P(UpdateGroupMultiPeerTest, JoinedRunning_PlDrain) {
  XLOG(INFO, "=== TEST: JoinedRunning_PlDrain ===");

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

  /* Inject route — triggers PL creation and drain */
  injectLocalRoutesAtRuntime({"39.5.0.0/16"}, {"3905:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("39.5.0.0/16")));

  /* Both peers receive the route (PL drained to both) */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "39.5.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3905:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "39.5.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3905:1"));

  /* Both peers still in sync */
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  XLOG(INFO, "=== TEST PASSED: JoinedRunning_PlDrain ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupMultiPeerTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupSlowPeerDetectionTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
