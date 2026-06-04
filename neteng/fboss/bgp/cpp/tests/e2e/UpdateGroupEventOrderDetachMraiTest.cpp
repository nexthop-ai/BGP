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
 * E2E tests: Event ordering -- detach, MRAI, policy, peer-down, accept
 * interactions.
 * Prefix range: 58.x.0.0/16
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * E-PEER-DOWN then E-PEER-UP -- rapid flap
 * Bring peer3 down then immediately back up. Verify peer4 continues receiving
 * routes throughout the flap. The reconnecting peer enters DETACHED_INIT_DUMP
 * (existing group). Drain its init dump queue, then verify peer4 still works.
 */
TEST_P(UpdateGroupMultiPeerTest, PeerDownThenPeerUp_RapidFlap) {
  XLOG(INFO, "=== TEST: PeerDownThenPeerUp_RapidFlap ===");

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

  /* Inject a shared route so both peers have state */
  injectLocalRoutesAtRuntime({"58.60.0.0/16"}, {"5860:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("58.60.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "58.60.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "5860:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "58.60.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5860:1"));

  /* EVENT 1: Bring peer3 DOWN */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Verify peer4 continues receiving routes while peer3 is DOWN */
  injectLocalRoutesAtRuntime({"58.61.0.0/16"}, {"5861:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("58.61.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "58.61.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5861:1"));

  /* EVENT 2: Bring peer3 back UP immediately -- rapid flap */
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);

  /*
   * Reconnecting peer enters DETACHED_INIT_DUMP (existing group).
   * Wait for init dump to fill queue and trigger freq detection
   * → DETACHED_BLOCKED, then drain.
   */
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED, 200));
  drainPeerQueueCompletely(peerId3);

  /* Verify peer4 still works after the flap -- inject another route */
  injectLocalRoutesAtRuntime({"58.62.0.0/16"}, {"5862:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("58.62.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "58.62.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5862:1"));

  /* Peer4 must remain in-sync throughout */
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
}

/*
 * E-ACCEPT then E-DETACH(other)
 * 3-peer group. Freq-detach peer3, unblock it so it starts recovery (DRJ),
 * then freq-detach peer4. Verify: peer3's recovery proceeds independently,
 * peer4 detaches cleanly, peer5 (sole in-sync) continues receiving routes.
 * Accept either JOINED_RUNNING or valid detached state for peer3 after
 * recovery (CL batch may re-block with small queue).
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

  /* Inject a shared route so all 3 peers have baseline state */
  injectLocalRoutesAtRuntime({"58.70.0.0/16"}, {"5870:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("58.70.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "58.70.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "5870:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "58.70.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5870:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "58.70.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "5870:1"));

  /*
   * Step 1: Freq-detach peer3 using threshold-raise pattern.
   * Set freq=1 to detach peer3, then raise to 999999 to protect peer4.
   */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("58.{}.0.0/16", 71 + i);
    auto c = fmt::format("{}:1", 5871 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("58.{}.0.0", 71 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("58.{}.0.0", 71 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Raise thresholds to protect peer4 from detachment during peer3 recovery */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Step 2: Unblock peer3 to start recovery (DRJ) */
  unblockPeer(kPeerAddr3);

  /*
   * Step 3: Now freq-detach peer4 while peer3 is recovering.
   * Lower threshold back to freq=1 for peer4's detachment.
   */
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr4);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("58.{}.0.0/16", 80 + i);
    auto c = fmt::format("{}:1", 5880 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    /* Only peer5 (sole in-sync) receives via PL */
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("58.{}.0.0", 80 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr4));

  /* Verify: peer5 (sole in-sync) continues receiving routes */
  injectLocalRoutesAtRuntime({"58.90.0.0/16"}, {"5890:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("58.90.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "58.90.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "5890:1"));

  /* Verify: peer3 is in a valid state (DRJ, DB, or possibly accepted) */
  auto peer3State = getPeerState(kPeerAddr3);
  EXPECT_NE(peer3State, PeerUpdateState::DOWN);

  /* Verify: peer4 is detached, peer5 in-sync */
  EXPECT_TRUE(isPeerDetached(kPeerAddr4));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupMultiPeerTest,
    ::testing::Values(kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
