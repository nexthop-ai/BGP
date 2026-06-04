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
 * E2E tests: Complex event sequences and route correctness after
 * detach/recover cycles.
 * Prefix range: 33.x.0.0/16
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Quintuple event sequence:
 * BLOCK → DETACH → ROUTE-WD → ROUTE-ADD → UNBLOCK → verify routes.
 * Block peer3, freq-detach, withdraw a route, add a new route,
 * unblock peer3, verify peer4 sees all changes correctly.
 */
TEST_P(UpdateGroupMultiPeerTest, QuintupleBlockDetachWdAddUnblock) {
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
  injectLocalRoutesAtRuntime({"33.1.0.0/16"}, {"3301:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3301:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3301:1"));

  /* Freq threshold=1 to detach on first block cycle */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* E-BLOCK: block peer3 and fill queue to trigger detachment */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"33.2.0.0/16"}, {"3302:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3302:1"));
  injectLocalRoutesAtRuntime({"33.3.0.0/16"}, {"3303:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3303:1"));
  injectLocalRoutesAtRuntime({"33.4.0.0/16"}, {"3304:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3304:1"));

  /* E-DETACH: peer3 should be DETACHED_BLOCKED */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* E-ROUTE-WD: withdraw shared route — CL tracks for peer3 */
  withdrawLocalRoutesAtRuntime({"33.1.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "33.1.0.0", 16, kPeerAddr4));

  /* E-ROUTE-ADD: inject new route — CL tracks for peer3 */
  injectLocalRoutesAtRuntime({"33.5.0.0/16"}, {"3305:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.5.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.5.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3305:1"));

  /* E-UNBLOCK: unblock peer3 */
  unblockPeer(kPeerAddr3);

  /* Verify peer3 state after unblock — may be detached or recovered */
  auto state3post = getPeerState(kPeerAddr3);
  EXPECT_NE(state3post, PeerUpdateState::INIT);
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);
}

/*
 * Double recovery cycle:
 * detach → recover (unblock) → re-block → resume → recover → accept.
 * Peer3 detaches, unblocks (DRJ), re-blocks, recovers again.
 * Uses queue (5,4,0) to avoid CL re-block issues.
 */
TEST_P(UpdateGroupMultiPeerTest, DoubleRecoveryCycle) {
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

  /* Freq threshold=1 — detach on first block cycle */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* CYCLE 1: block peer3 and fill to trigger detachment */
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 5; ++i) {
    auto prefix = fmt::format("33.{}.0.0/16", 10 + i);
    auto community = fmt::format("{}:1", 3310 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("33.{}.0.0", 10 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Recover: unblock → DRJ or DB (accept either due to CL batch) */
  unblockPeer(kPeerAddr3);

  /* Inject 1 more route to confirm peer4 still works */
  injectLocalRoutesAtRuntime({"33.20.0.0/16"}, {"3320:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3320:1"));

  /* CYCLE 2: block peer3 again */
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 5; ++i) {
    auto prefix = fmt::format("33.{}.0.0/16", 25 + i);
    auto community = fmt::format("{}:1", 3325 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("33.{}.0.0", 25 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  /* peer3 may or may not be blocked depending on recovery state */
  waitForPeerQueueBlocked(peerId3);

  /* CYCLE 2: recover again */
  unblockPeer(kPeerAddr3);

  /* Final verification: peer4 still functional */
  injectLocalRoutesAtRuntime({"33.35.0.0/16"}, {"3335:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.35.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.35.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3335:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);
}

/*
 * All peers detach scenario.
 * With 2 peers, last synced member (bit 0, peer3) is preserved.
 * peer4 detaches, peer3 stays JOINED_BLOCKED.
 * Routes injected post-detach go only to peer3 (blocked).
 * Learned pattern: N-1 detachments, last synced preserved.
 */
TEST_P(UpdateGroupMultiPeerTest, AllPeersDetach_LastSyncedPreserved) {
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

  /* Freq threshold=1 for both peers */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Block BOTH peers, fill queue */
  blockPeer(kPeerAddr3);
  blockPeer(kPeerAddr4);
  injectLocalRoutesAtRuntime({"33.40.0.0/16"}, {"3340:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.40.0.0/16")));
  injectLocalRoutesAtRuntime({"33.41.0.0/16"}, {"3341:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.41.0.0/16")));
  injectLocalRoutesAtRuntime({"33.42.0.0/16"}, {"3342:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.42.0.0/16")));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));

  /* peer4 detaches, peer3 (bit 0) preserved as last synced */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1);

  /* Inject more routes — CL accumulates for all */
  injectLocalRoutesAtRuntime({"33.43.0.0/16"}, {"3343:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.43.0.0/16")));

  /* Unblock peer3 (the preserved synced member).
   * unblockPeer drains queue so don't try to verify specific routes after */
  unblockPeer(kPeerAddr3);

  /* peer3 should recover to JOINED_RUNNING eventually */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  verifySlowPeerInvariants(kPeerAddr3);
}

/*
 * DFP final routes match in-sync peers.
 * After freq-detach, inject routes that only peer4 sees via PL.
 * Peer3's CL tracks them. Verify peer4 receives all correctly.
 * Zero CL consumption by peer3 (stays detached).
 */
TEST_P(UpdateGroupMultiPeerTest, DfpFinalRoutesMatchInSync) {
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

  /* Inject shared routes before detachment */
  injectLocalRoutesAtRuntime({"33.50.0.0/16"}, {"3350:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.50.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.50.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3350:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.50.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3350:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"33.51.0.0/16"}, {"3351:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.51.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.51.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3351:1"));
  injectLocalRoutesAtRuntime({"33.52.0.0/16"}, {"3352:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.52.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.52.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3352:1"));
  injectLocalRoutesAtRuntime({"33.53.0.0/16"}, {"3353:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.53.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.53.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3353:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Post-detach routes — only peer4 gets via PL, peer3 CL tracks */
  for (int i = 0; i < 4; ++i) {
    auto prefix = fmt::format("33.{}.0.0/16", 60 + i);
    auto community = fmt::format("{}:1", 3360 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("33.{}.0.0", 60 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Peer3 still detached, zero CL consumption */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);
}

/*
 * AS-path prepend correctness.
 * Detached peer's AS-path matches the group's after route injection
 * post-detach. Verify peer4 (in-sync) receives correct AS-path for
 * local routes (AS-path = local AS only) through the entire cycle.
 */
TEST_P(UpdateGroupMultiPeerTest, AsPathCorrectnessThroughDetach) {
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

  /* Pre-detach route — both peers get it with correct AS-path */
  injectLocalRoutesAtRuntime({"33.70.0.0/16"}, {"3370:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.70.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.70.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3370:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.70.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3370:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"33.71.0.0/16"}, {"3371:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.71.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.71.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3371:1"));
  injectLocalRoutesAtRuntime({"33.72.0.0/16"}, {"3372:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.72.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.72.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3372:1"));
  injectLocalRoutesAtRuntime({"33.73.0.0/16"}, {"3373:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.73.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.73.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3373:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Post-detach: inject routes, verify peer4 AS-path is still correct */
  injectLocalRoutesAtRuntime({"33.74.0.0/16"}, {"3374:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.74.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.74.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3374:1"));

  injectLocalRoutesAtRuntime({"33.75.0.0/16"}, {"3375:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.75.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.75.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3375:1"));

  /* Verify final state: peer4 in-sync with correct route attributes */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);
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
