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
 * E2E tests: Lifecycle withdraw, flap, and cascading recovery scenarios
 * Prefix range: 62.x.0.0/16
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Lifecycle -- peer detaches, new peer joins, old peer recovers.
 * 3-peer group: bring all 3 up, bring peer5 DOWN to simulate "not yet joined".
 * Detach peer3 (freq), bring peer5 back UP (reconnect = "new peer joins"),
 * drain peer5's init dump, then recover peer3 via unblock.
 * Verify peer4 continues throughout, peer5 receives after rejoin,
 * and peer3 reaches a valid state after recovery.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachThenNewPeerJoinsThenRecover) {
  XLOGF(INFO, "=== TEST: DetachThenNewPeerJoinsThenRecover ===");

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

  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);
  drainPeerQueueCompletely(peerId5);

  /* Inject a shared baseline route -- all 3 receive */
  injectLocalRoutesAtRuntime({"62.1.0.0/16"}, {"6201:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("62.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "62.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "6201:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "62.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "6201:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "62.1.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "6201:1"));

  /* Bring peer5 DOWN to simulate "not yet in the group" */
  bringDownPeer(kPeerAddr5);
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::DOWN));

  /* Freq-detach peer3 using threshold-raise pattern */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("62.{}.0.0/16", 10 + i);
    auto c = fmt::format("{}:1", 6210 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    /* Only peer4 is in-sync (peer5 is DOWN) */
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("62.{}.0.0", 10 + i),
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

  /* Bring peer5 back UP -- simulates "new peer joins while peer3 detached".
   * Reconnecting peer enters DETACHED_INIT_DUMP for existing group.
   * Do NOT call sendEoR or waitForEoR -- peer in DID processes asynchronously.
   * Do NOT drainPeerQueueCompletely -- may hang if queue is empty. */
  bringUpPeer(kPeerAddr5);

  /* peer5 is now in the group (likely DETACHED_INIT_DUMP) */
  auto state5AfterRejoin = getPeerState(kPeerAddr5);
  XLOGF(
      INFO,
      "DetachNewPeerRecover: peer5={} after rejoin",
      static_cast<int>(state5AfterRejoin));
  EXPECT_NE(state5AfterRejoin, PeerUpdateState::DOWN)
      << "peer5 should not be DOWN after rejoin";

  /* Inject route -- peer4 receives via PL, peer3 CL accumulates */
  injectLocalRoutesAtRuntime({"62.20.0.0/16"}, {"6220:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("62.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "62.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "6220:1"));

  /* peer3 still detached */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Recover peer3 -- use waitForDrjWithDrain to properly handle
   * the recovery path (defer DRJ, unblock, drain, accept) */
  waitForDrjWithDrain(kPeerAddr3, peerId3);

  /* Inject PL-cycle routes to drive continued group operation */
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("62.{}.0.0/16", 30 + i);
    auto c = fmt::format("{}:1", 6230 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    drainPeerQueueCompletely(peerId3, 1, 100);
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("62.{}.0.0", 30 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
  }

  /* Verify final states */
  auto state3 = getPeerState(kPeerAddr3);
  auto state5 = getPeerState(kPeerAddr5);
  XLOGF(
      INFO,
      "DetachNewPeerRecover: peer3={}, peer5={} after detach+rejoin+recover",
      static_cast<int>(state3),
      static_cast<int>(state5));

  EXPECT_NE(state3, PeerUpdateState::DOWN)
      << "peer3 should not be DOWN after recovery";
  EXPECT_NE(state5, PeerUpdateState::DOWN)
      << "peer5 should not be DOWN after rejoin";
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Final route confirms group still functional */
  injectLocalRoutesAtRuntime({"62.40.0.0/16"}, {"6240:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("62.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "62.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "6240:1"));

  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);
  verifySlowPeerInvariants(kPeerAddr5);

  XLOGF(INFO, "=== PASSED: DetachThenNewPeerJoinsThenRecover ===");
}

/* Lifecycle -- A detaches, B goes down, C sole member, B comes back.
 * 3-peer group: freq-detach peer3 (A), bring peer4 (B) DOWN, peer5 (C)
 * is sole in-sync member. Then bring peer4 (B) back UP -- reconnecting
 * peer enters DETACHED_INIT_DUMP. Verify peer5 continues receiving
 * routes throughout and peer4 rejoins the group (non-DOWN).
 */
TEST_P(UpdateGroupMultiPeerTest, DetachADownBSoleCBReturns) {
  XLOGF(INFO, "=== TEST: DetachADownBSoleCBReturns ===");

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

  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);
  drainPeerQueueCompletely(peerId5);

  /* Inject shared baseline route -- all 3 receive */
  injectLocalRoutesAtRuntime({"62.91.0.0/16"}, {"6291:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("62.91.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "62.91.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "6291:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "62.91.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "6291:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "62.91.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "6291:1"));

  /* Freq-detach peer3 (A) via threshold-raise pattern */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("62.{}.0.0/16", 92 + i);
    auto c = fmt::format("{}:1", 6292 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("62.{}.0.0", 92 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("62.{}.0.0", 92 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Raise thresholds to protect remaining peers */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  /* Bring peer4 (B) DOWN -- now only peer5 (C) is in-sync */
  bringDownPeer(kPeerAddr4);
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DOWN));

  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  /* Inject route while peer5 is sole in-sync member */
  injectLocalRoutesAtRuntime({"62.95.0.0/16"}, {"6295:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("62.95.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "62.95.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "6295:1"));

  /* Bring peer4 (B) back UP -- reconnects into DETACHED_INIT_DUMP.
   * Do NOT call sendEoR or waitForEoR -- peer in DID processes asynchronously.
   * Do NOT drainPeerQueueCompletely -- may hang if queue is empty. */
  bringUpPeer(kPeerAddr4);

  auto state4 = getPeerState(kPeerAddr4);
  XLOGF(
      INFO,
      "DetachADownBSoleC: peer4={} after reconnect",
      static_cast<int>(state4));
  EXPECT_NE(state4, PeerUpdateState::DOWN)
      << "peer4 should rejoin the group after reconnect";

  /* Inject another route -- peer5 continues receiving */
  injectLocalRoutesAtRuntime({"62.96.0.0/16"}, {"6296:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("62.96.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "62.96.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "6296:1"));

  /* Verify final states */
  auto state3 = getPeerState(kPeerAddr3);
  state4 = getPeerState(kPeerAddr4);
  auto state5 = getPeerState(kPeerAddr5);
  XLOGF(
      INFO,
      "DetachADownBSoleC: peer3={}, peer4={}, peer5={} final",
      static_cast<int>(state3),
      static_cast<int>(state4),
      static_cast<int>(state5));

  EXPECT_NE(state3, PeerUpdateState::DOWN)
      << "peer3 (detached) should not be DOWN";
  EXPECT_NE(state4, PeerUpdateState::DOWN)
      << "peer4 should be alive after reconnect";
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  /* Final route confirms group still functional */
  injectLocalRoutesAtRuntime({"62.97.0.0/16"}, {"6297:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("62.97.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "62.97.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "6297:1"));

  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr5);

  XLOGF(INFO, "=== PASSED: DetachADownBSoleCBReturns ===");
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
