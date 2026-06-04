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
 * E2E tests: Lifecycle flap and shutdown scenarios for slow peer handling.
 * Prefix range: 63.x.0.0/16
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Lifecycle -- detach peer, then send EoR to the detached peer.
 * Extra sendEoRToPeer on a detached peer should be harmless (no crash,
 * no state corruption). Verify peer4 continues receiving routes normally
 * and peer3 stays in a valid detached state after the extra EoR.
 */
TEST_P(UpdateGroupMultiPeerTest, EoRWhileDetached) {
  XLOGF(INFO, "=== TEST: EoRWhileDetached ===");

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

  /* Inject a shared baseline route -- both peers receive */
  injectLocalRoutesAtRuntime({"63.1.0.0/16"}, {"6301:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("63.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "63.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "6301:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "63.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "6301:1"));

  /* Freq-detach peer3: set freq=1, block, fill queue with 3 routes */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("63.{}.0.0/16", 10 + i);
    auto c = fmt::format("{}:1", 6310 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("63.{}.0.0", 10 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Raise thresholds to protect peer4 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Send extra EoR to the detached peer -- should be harmless no-op */
  sendEoRToPeer(peerId3);

  /* Verify peer3 is still detached and not crashed */
  auto state3 = getPeerState(kPeerAddr3);
  XLOGF(
      INFO,
      "peer3={} after extra EoR while detached",
      static_cast<int>(state3));
  EXPECT_NE(state3, PeerUpdateState::DOWN)
      << "Extra EoR should not crash the detached peer";
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Verify peer4 continues receiving routes normally */
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("63.{}.0.0/16", 20 + i);
    auto c = fmt::format("{}:1", 6320 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("63.{}.0.0", 20 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
  }

  /* Final state checks */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);

  XLOGF(INFO, "=== PASSED: EoRWhileDetached ===");
}

/*
 * Lifecycle -- group destroyed when last peer goes down.
 * Detach peer3, then bring peer4 (last in-sync member) DOWN, then bring
 * peer3 DOWN. Verify no crash and graceful cleanup.
 */
TEST_P(UpdateGroupMultiPeerTest, GroupDestroyedLastPeerDown) {
  XLOGF(INFO, "=== TEST: GroupDestroyedLastPeerDown ===");

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

  /* Inject a shared route so the group has state to clean up */
  injectLocalRoutesAtRuntime({"63.50.0.0/16"}, {"6350:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("63.50.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "63.50.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "6350:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "63.50.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "6350:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("63.{}.0.0/16", 51 + i);
    auto c = fmt::format("{}:1", 6351 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("63.{}.0.0", 51 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Raise thresholds to protect peer4 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Bring last in-sync peer (peer4) DOWN -- group has 0 in-sync members */
  bringDownPeer(kPeerAddr4);
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DOWN));

  /* Bring detached peer (peer3) DOWN -- group is now fully destroyed */
  unblockPeer(kPeerAddr3);
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Both peers should be DOWN, no crash */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DOWN));

  XLOGF(INFO, "=== PASSED: GroupDestroyedLastPeerDown ===");
}

/*
 * Lifecycle -- rapid peer flap: peer3 goes down/up/down/up (2 cycles).
 * After each reconnection, peer3 enters DETACHED_INIT_DUMP. Verify peer4
 * continues receiving routes throughout the flap storm.
 */
TEST_P(UpdateGroupMultiPeerTest, RapidPeerFlap) {
  XLOGF(INFO, "=== TEST: RapidPeerFlap ===");

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

  /* Inject baseline route -- both peers receive */
  injectLocalRoutesAtRuntime({"63.60.0.0/16"}, {"6360:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("63.60.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "63.60.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "6360:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "63.60.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "6360:1"));

  /* Flap cycle 1: DOWN -> UP */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  drainPeerQueueCompletely(peerId3);
  auto state3a = getPeerState(kPeerAddr3);
  XLOGF(
      INFO, "peer3 after flap cycle 1 UP: state={}", static_cast<int>(state3a));
  EXPECT_NE(state3a, PeerUpdateState::DOWN);

  /* Verify peer4 still works -- inject a route during flap */
  injectLocalRoutesAtRuntime({"63.61.0.0/16"}, {"6361:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("63.61.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "63.61.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "6361:1"));

  /* Flap cycle 2: DOWN -> UP */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  drainPeerQueueCompletely(peerId3);
  auto state3b = getPeerState(kPeerAddr3);
  XLOGF(
      INFO, "peer3 after flap cycle 2 UP: state={}", static_cast<int>(state3b));
  EXPECT_NE(state3b, PeerUpdateState::DOWN);

  /* Verify peer4 continues receiving routes after 2 flap cycles */
  injectLocalRoutesAtRuntime({"63.62.0.0/16"}, {"6362:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("63.62.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "63.62.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "6362:1"));

  /* Final state: peer4 in-sync, peer3 alive after 2 flaps */
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_NE(getPeerState(kPeerAddr3), PeerUpdateState::DOWN);

  XLOGF(INFO, "=== PASSED: RapidPeerFlap ===");
}

/*
 * Lifecycle -- shutdown while peer is in detached state.
 * Detach peer3 via frequency threshold, then bring both peers DOWN
 * (simulating graceful shutdown). Verify no crash, no hang, and both
 * peers reach DOWN state cleanly regardless of detachment state.
 */
TEST_P(UpdateGroupMultiPeerTest, ShutdownWhileDetached) {
  XLOGF(INFO, "=== TEST: ShutdownWhileDetached ===");

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

  /* Inject shared route so group has state to clean up during shutdown */
  injectLocalRoutesAtRuntime({"63.70.0.0/16"}, {"6370:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("63.70.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "63.70.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "6370:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "63.70.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "6370:1"));

  /* Freq-detach peer3: freq=1, block, fill queue with 3 routes */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("63.{}.0.0/16", 71 + i);
    auto c = fmt::format("{}:1", 6371 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("63.{}.0.0", 71 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Raise thresholds to protect peer4 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Verify detachment state before shutdown */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Inject more routes while detached to create CL items */
  for (int i = 0; i < 2; i++) {
    auto p = fmt::format("63.{}.0.0/16", 80 + i);
    auto c = fmt::format("{}:1", 6380 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("63.{}.0.0", 80 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
  }

  /*
   * Graceful shutdown: bring detached peer DOWN first, then in-sync peer.
   * This exercises cleanup of detachment state (CL consumer, clone entries)
   * during peer teardown while group still has an in-sync member.
   */
  unblockPeer(kPeerAddr3);
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Verify peer4 is still functional after detached peer's shutdown */
  injectLocalRoutesAtRuntime({"63.85.0.0/16"}, {"6385:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("63.85.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "63.85.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "6385:1"));

  /* Bring last peer DOWN -- full group shutdown */
  bringDownPeer(kPeerAddr4);
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DOWN));

  /* Both peers DOWN, no crash, no hang */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DOWN));

  XLOGF(INFO, "=== PASSED: ShutdownWhileDetached ===");
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
