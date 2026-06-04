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
 * E2E tests: P-JOINED_RUNNING and P-JOINED_BLOCKED state × Event coverage
 * Tests peer behavior when events arrive during JOINED states.
 *
 * Prefix range: 17.x.0.0/16
 *
 * Tests:
 *   P-JR × E-ROUTE-REFRESH — Route refresh triggers detached mode
 *   P-JB × E-BLOCK — Already blocked, idempotent, no double-count
 *   P-JB × E-PEER-UP — N/A (already up), verify no crash
 *   P-JB × E-CL-END — N/A (group waiting, not consuming CL)
 *   P-JB × E-ROUTE-REFRESH — Route refresh for blocked peer
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook::bgp {
/*
 * P-JB × E-BLOCK
 * Peer is already JOINED_BLOCKED. Triggering another "block" event
 * should be idempotent — no double-count, no crash. Verify the peer
 * stays in JOINED_BLOCKED and the group stays in WAITING state.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, JoinedBlocked_BlockIdempotent) {
  XLOG(INFO, "=== TEST: JoinedBlocked_BlockIdempotent ===");

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

  /* Block peer3 and fill queue to trigger JOINED_BLOCKED */
  blockPeer(kPeerAddr3);

  injectLocalRoutesAtRuntime({"17.4.0.0/16"}, {"1704:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("17.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1704:1"));

  injectLocalRoutesAtRuntime({"17.5.0.0/16"}, {"1705:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("17.5.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.5.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1705:1"));

  injectLocalRoutesAtRuntime({"17.6.0.0/16"}, {"1706:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("17.6.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.6.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1706:1"));

  /* Peer3 should be JOINED_BLOCKED now */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* Call blockPeer again — should be idempotent */
  blockPeer(kPeerAddr3);
  EXPECT_TRUE(isPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* Group should be in WAITING state */
  EXPECT_EQ(getGroupState(kPeerAddr3), UpdateGroupState::WAITING);

  /* Peer4 remains JOINED_RUNNING */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: JoinedBlocked_BlockIdempotent ===");
}
/*
 * P-JB × E-CL-END
 * N/A scenario: group is in WAITING state because peer3 is blocked.
 * The CL is not being consumed (group waits for PL to drain first).
 * Inject a route — it goes to CL but is NOT delivered to any peer
 * (group is WAITING). Verify the state is stable, then unblock to
 * let everything drain.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, JoinedBlocked_ClEndNoop) {
  XLOG(INFO, "=== TEST: JoinedBlocked_ClEndNoop ===");

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

  /* Block peer3 and fill queue to get JOINED_BLOCKED */
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

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));
  EXPECT_EQ(getGroupState(kPeerAddr3), UpdateGroupState::WAITING);

  /*
   * Inject another route while group is WAITING — goes to CL.
   * Group does NOT consume CL while in WAITING (PL not drained yet).
   * The route goes to shadowRib but is NOT delivered to peers.
   */
  injectLocalRoutesAtRuntime({"17.13.0.0/16"}, {"1713:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("17.13.0.0/16")));

  /* Group still WAITING, peer3 still JOINED_BLOCKED */
  EXPECT_EQ(getGroupState(kPeerAddr3), UpdateGroupState::WAITING);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /*
   * Unblock peer3 — PL drains, then CL item (17.13) is pushed as new PL.
   * After unblocking, peer4 should receive the CL route.
   */
  unblockPeer(kPeerAddr3);
  drainPeerQueueCompletely(peerId3);
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "17.13.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1713:1"));

  XLOG(INFO, "=== TEST PASSED: JoinedBlocked_ClEndNoop ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupSlowPeerDetectionTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace facebook::bgp
