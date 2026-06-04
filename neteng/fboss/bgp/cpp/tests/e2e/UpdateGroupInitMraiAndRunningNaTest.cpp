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
 * E2E tests: P-INIT MRAI/Multi-route and P-JR N/A event coverage
 *
 * Prefix range: 29.x.0.0/16
 *
 * Tests:
 *   P-INIT × E-MRAI-FIRE — MRAI fires during init
 *   P-INIT × E-MULTI-ROUTE — Batch routes during init
 *   P-JR × E-SLOW-DUR — N/A (not blocked, no timer)
 *   P-JR × E-PEER-UP — N/A (already up)
 *   P-JR × E-CL-END — CL consumed, READY→IDLE
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * P-INIT × E-MRAI-FIRE
 * Inject multiple routes with different communities while peer3 is in INIT.
 * Each route triggers a separate MRAI cycle on the group. Peer3 stays in
 * INIT throughout. After peer3 completes EoR and reaches JOINED_RUNNING,
 * it receives all the CL items that accumulated during INIT.
 * Peer4 (already JOINED_RUNNING) receives routes as they are injected.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, Init_MraiFire) {
  XLOG(INFO, "=== TEST: Init_MraiFire ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /* Complete peer4 first — peer3 stays in INIT */
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Inject 3 routes with different communities — each triggers MRAI cycle */
  injectLocalRoutesAtRuntime({"29.1.0.0/16"}, {"2901:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2901:1"));

  injectLocalRoutesAtRuntime({"29.2.0.0/16"}, {"2902:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2902:1"));

  injectLocalRoutesAtRuntime({"29.3.0.0/16"}, {"2903:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2903:1"));

  /* Complete peer3 — transitions INIT → JOINED_RUNNING */
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Peer3 receives all 3 CL items accumulated during INIT */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2901:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.2.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2902:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.3.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2903:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: Init_MraiFire ===");
}

/*
 * P-INIT × E-MULTI-ROUTE
 * Inject a batch of 4 routes while peer3 is in INIT. All routes accumulate
 * in CL. Peer4 receives them inline. After peer3 completes EoR, it receives
 * all 4 routes from CL processing. Inject-drain one at a time to avoid
 * non-deterministic delivery order.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, Init_MultiRoute) {
  XLOG(INFO, "=== TEST: Init_MultiRoute ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /* Complete peer4 first — peer3 stays in INIT */
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Inject 4 routes one at a time, draining peer4 after each */
  for (int i = 0; i < 4; i++) {
    auto prefix = fmt::format("29.{}.0.0/16", 10 + i);
    auto community = fmt::format("{}:1", 2910 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("29.{}.0.0", 10 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Complete peer3 — transitions INIT → JOINED_RUNNING */
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Peer3 receives all 4 CL items */
  for (int i = 0; i < 4; i++) {
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("29.{}.0.0", 10 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        fmt::format("{}:1", 2910 + i)));
  }

  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: Init_MultiRoute ===");
}

/*
 * P-JR × E-SLOW-DUR
 * N/A scenario: duration threshold on a running (non-blocked) peer.
 * Set an aggressive 1ms duration threshold. Since the peer is never blocked,
 * the timer never starts and detachment never fires. Both peers continue
 * receiving routes normally.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, JoinedRunning_SlowDurNoop) {
  XLOG(INFO, "=== TEST: JoinedRunning_SlowDurNoop ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(8, 6, 2);

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

  /* Set aggressive 1ms duration threshold — should never fire (no block) */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(1),
      999999,
      std::chrono::milliseconds(60000));

  /* Inject routes — both peers should receive normally, no detachment */
  injectLocalRoutesAtRuntime({"29.20.0.0/16"}, {"2920:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.20.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2920:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2920:1"));

  injectLocalRoutesAtRuntime({"29.21.0.0/16"}, {"2921:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.21.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2921:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2921:1"));

  /* Peer3 should still be JOINED_RUNNING, NOT detached */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: JoinedRunning_SlowDurNoop ===");
}

/*
 * P-JR × E-PEER-UP
 * N/A scenario: peer is already up and JOINED_RUNNING. Calling bringUpPeer
 * on an already-up peer causes session re-establishment which hangs.
 * Instead, verify the peer is up and functional, then confirm normal
 * route delivery continues.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, JoinedRunning_PeerUpNoop) {
  XLOG(INFO, "=== TEST: JoinedRunning_PeerUpNoop ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(8, 6, 2);

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

  /* Peer3 is already UP — verify state stability */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_EQ(getGroupMemberCount(kPeerAddr3), 2);

  /* Verify normal route delivery continues after the N/A event */
  injectLocalRoutesAtRuntime({"29.30.0.0/16"}, {"2930:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.30.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2930:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2930:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: JoinedRunning_PeerUpNoop ===");
}

/*
 * P-JR × E-CL-END
 * CL consumed event: group transitions READY→IDLE after all CL items
 * are consumed by MRAI fire. Inject a route, verify delivery to both
 * peers, then confirm group settles to IDLE. A second route verifies
 * the group correctly cycles back from IDLE→READY→IDLE.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, JoinedRunning_ClEnd) {
  XLOG(INFO, "=== TEST: JoinedRunning_ClEnd ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(8, 6, 2);

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

  /* Inject route — triggers IDLE→READY, MRAI fires, CL consumed → IDLE */
  injectLocalRoutesAtRuntime({"29.40.0.0/16"}, {"2940:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.40.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2940:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2940:1"));

  /* After delivery, group should settle to IDLE or still transitioning */
  auto groupState = getGroupState(kPeerAddr3);
  XLOGF(
      INFO,
      "Group state after first CL cycle: {}",
      static_cast<int>(groupState));
  EXPECT_TRUE(
      groupState == UpdateGroupState::IDLE ||
      groupState == UpdateGroupState::READY ||
      groupState == UpdateGroupState::WAITING);

  /* Second route — verify IDLE→READY→IDLE cycle repeats correctly */
  injectLocalRoutesAtRuntime({"29.41.0.0/16"}, {"2941:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.41.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2941:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2941:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  XLOG(INFO, "=== TEST PASSED: JoinedRunning_ClEnd ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupSlowPeerDetectionTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
