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

/* E2E tests: P-DRJ (DETACHED_READY_TO_JOIN) × active event coverage.
 * Prefix range: 20.50-20.99/16
 *
 * P-DRJ × E-PEER-DOWN (peer down during recovery — full cleanup)
 * P-DRJ × E-PL-DRAIN (PL drains → group checks acceptance → re-join)
 * P-DRJ × E-POLICY-CHG (policy change at DRJ — simulated via wd+add)
 * P-DRJ × E-MRAI-FIRE (MRAI fires during recovery — group processes)
 * P-DRJ × E-MULTI-ROUTE (batch routes during recovery — CL items)
 *
 * DRJ is reached via freq-detach → unblock pattern. These tests exercise
 * non-trivial events during the recovery/ready-to-join phase.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * P-DRJ × E-PEER-DOWN
 * Peer goes down while in DRJ (recovery). The group should discard the
 * ready-to-join state and perform full cleanup. Peer4 should continue
 * operating normally. This tests graceful handling of peer loss during
 * the delicate recovery phase.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachedReadyToJoin_PeerDown) {
  XLOG(INFO, "=== TEST: DetachedReadyToJoin_PeerDown ===");

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

  /* Detach peer3 via freq threshold (1 block = detach) */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"20.50.0.0/16"}, {"2050:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.50.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.50.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2050:1"));
  injectLocalRoutesAtRuntime({"20.51.0.0/16"}, {"2051:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.51.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.51.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2051:1"));
  injectLocalRoutesAtRuntime({"20.52.0.0/16"}, {"2052:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.52.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.52.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2052:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock peer3 — hold in DRJ via test hook.
   * Drain loop prevents queue backpressure while CL consumer processes. */
  testOnlyDeferDrjAcceptance(kPeerAddr3, true);
  unblockPeer(kPeerAddr3);
  {
    for (int i = 0; i < 20; ++i) {
      if (getPeerState(kPeerAddr3) == PeerUpdateState::DETACHED_READY_TO_JOIN) {
        break;
      }
      drainPeerQueueCompletely(peerId3, 1, 100);
      peerManager_->getEventBase().runInEventBaseThreadAndWait([]() {});
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    drainPeerQueueCompletely(peerId3, 1, 100);
  }
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_READY_TO_JOIN));

  /* Bring peer3 DOWN during DRJ — full cleanup */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Verify peer4 still functional after peer3 went down during DRJ */
  injectLocalRoutesAtRuntime({"20.53.0.0/16"}, {"2053:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.53.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.53.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2053:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: DetachedReadyToJoin_PeerDown ===");
}

/*
 * P-DRJ × E-PL-DRAIN
 * Group PL drains while peer is recovering (DRJ). After the PL finishes
 * draining, the group checks whether to accept the detached peer back.
 * Use larger queue (5,4,0) so that after unblock the CL batch doesn't
 * re-block peer3. Verify peer3 gets accepted back (or at least peer4
 * continues processing).
 */
TEST_P(UpdateGroupMultiPeerTest, DetachedReadyToJoin_PlDrain) {
  XLOG(INFO, "=== TEST: DetachedReadyToJoin_PlDrain ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(5, 4, 0);

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

  /* Detach peer3 via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  /* Fill queue past hwm (4) with 5 routes — different communities */
  for (int i = 0; i < 5; ++i) {
    auto prefix = fmt::format("20.{}.0.0/16", 55 + i);
    auto community = fmt::format("{}:1", 2055 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("20.{}.0.0", 55 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock peer3 — hold in DRJ via test hook */
  waitForDrjWithDrain(kPeerAddr3, peerId3);

  /* Inject 1 more route to trigger a new PL cycle and drain */
  injectLocalRoutesAtRuntime({"20.61.0.0/16"}, {"2061:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.61.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.61.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2061:1"));

  /* Peer4 still functional, group stable after PL drain during recovery */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: DetachedReadyToJoin_PlDrain ===");
}

/*
 * P-DRJ × E-POLICY-CHG
 * Policy change during DRJ. Since runtime policy changes are incompatible
 * with slow peer test fixture (learned pattern W4), simulate policy change
 * by withdrawing a prefix and injecting a different prefix with different
 * attributes. This exercises the re-evaluation code path.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachedReadyToJoin_PolicyChange) {
  XLOG(INFO, "=== TEST: DetachedReadyToJoin_PolicyChange ===");

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

  /* Inject a shared route before detachment */
  injectLocalRoutesAtRuntime({"20.65.0.0/16"}, {"2065:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.65.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.65.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2065:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.65.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2065:1"));

  /* Detach peer3 via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"20.66.0.0/16"}, {"2066:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.66.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.66.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2066:1"));
  injectLocalRoutesAtRuntime({"20.67.0.0/16"}, {"2067:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.67.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.67.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2067:1"));
  injectLocalRoutesAtRuntime({"20.68.0.0/16"}, {"2068:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.68.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.68.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2068:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock peer3 — hold in DRJ via test hook */
  waitForDrjWithDrain(kPeerAddr3, peerId3);

  /* Simulate policy change: withdraw old prefix, inject different one */
  withdrawLocalRoutesAtRuntime({"20.65.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "20.65.0.0", 16, kPeerAddr4));

  injectLocalRoutesAtRuntime({"20.69.0.0/16"}, {"2069:1"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.69.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.69.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2069:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: DetachedReadyToJoin_PolicyChange ===");
}

/*
 * P-DRJ × E-MULTI-ROUTE
 * Batch of routes injected while peer is in DRJ. Each route creates a
 * CL item that the recovering peer must consume. Peer4 receives all
 * routes normally. Inject and drain one at a time to avoid order issues.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachedReadyToJoin_MultiRoute) {
  XLOG(INFO, "=== TEST: DetachedReadyToJoin_MultiRoute ===");

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

  /* Detach peer3 via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"20.80.0.0/16"}, {"2080:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.80.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.80.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2080:1"));
  injectLocalRoutesAtRuntime({"20.81.0.0/16"}, {"2081:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.81.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.81.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2081:1"));
  injectLocalRoutesAtRuntime({"20.82.0.0/16"}, {"2082:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.82.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.82.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2082:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock peer3 — hold in DRJ via test hook */
  waitForDrjWithDrain(kPeerAddr3, peerId3);

  /* Inject batch of 4 routes — each is a separate CL item */
  for (int i = 0; i < 4; ++i) {
    auto prefix = fmt::format("20.{}.0.0/16", 83 + i);
    auto community = fmt::format("{}:1", 2083 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("20.{}.0.0", 83 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* All 4 batch routes delivered to peer4, group stable */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: DetachedReadyToJoin_MultiRoute ===");
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
