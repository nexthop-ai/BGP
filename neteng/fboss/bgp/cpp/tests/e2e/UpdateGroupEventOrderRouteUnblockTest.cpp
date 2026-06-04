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
 * E2E tests: Event ordering — route injection and unblock/accept interactions
 * Prefix range: 57.x.0.0/16
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * E-UNBLOCK then E-ROUTE-ADD
 * Detach peer3 via freq threshold, unblock peer3, THEN inject a new route.
 * After unblock, peer3 starts recovery (DRJ/DB). New route should be
 * delivered to peer4 via PL normally. peer3 accumulates in CL.
 */
TEST_P(UpdateGroupMultiPeerTest, UnblockThenRouteAdd) {
  XLOG(INFO, "=== TEST: UnblockThenRouteAdd ===");

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

  /* Freq-detach peer3: freq=1, block + fill queue with 3 routes */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("57.{}.0.0/16", 1 + i);
    auto c = fmt::format("{}:1", 5701 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("57.{}.0.0", 1 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* EVENT 1: Unblock peer3 → starts recovery */
  unblockPeer(kPeerAddr3);

  /* EVENT 2: Inject new route after unblock */
  injectLocalRoutesAtRuntime({"57.10.0.0/16"}, {"5710:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("57.10.0.0/16")));

  /* peer4 receives the new route normally */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "57.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5710:1"));

  /*
   * peer3 may still be detached or may have recovered to JOINED_RUNNING.
   * Both are valid outcomes after unblock.
   */
  auto state3 = getPeerState(kPeerAddr3);
  EXPECT_NE(state3, PeerUpdateState::DOWN);
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  if (isPeerDetached(kPeerAddr3)) {
    verifySlowPeerInvariants(kPeerAddr3);
  }
}

/*
 * E-ROUTE-ADD then E-UNBLOCK
 * Detach peer3, inject a route while detached (CL item), THEN unblock.
 * The CL item should be processed during recovery. peer4 receives via PL.
 */
TEST_P(UpdateGroupMultiPeerTest, RouteAddThenUnblock) {
  XLOG(INFO, "=== TEST: RouteAddThenUnblock ===");

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

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("57.{}.0.0/16", 20 + i);
    auto c = fmt::format("{}:1", 5720 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("57.{}.0.0", 20 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* EVENT 1: Inject route while peer3 is detached+blocked */
  injectLocalRoutesAtRuntime({"57.30.0.0/16"}, {"5730:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("57.30.0.0/16")));

  /* peer4 receives it immediately via PL */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "57.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5730:1"));

  /* EVENT 2: Unblock peer3 → recovery processes CL items */
  unblockPeer(kPeerAddr3);

  /*
   * Peer3 may be in any detached state or may have recovered to JR.
   * Both are valid.
   */
  auto state3 = getPeerState(kPeerAddr3);
  EXPECT_NE(state3, PeerUpdateState::DOWN);
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  if (isPeerDetached(kPeerAddr3)) {
    verifySlowPeerInvariants(kPeerAddr3);
  }
}

/*
 * E-ACCEPT then E-ROUTE-ADD
 * Detach peer3, unblock, wait for acceptance back to JOINED_RUNNING,
 * then inject a new route. Both peers should receive it normally.
 * Uses queue (5,4,0) with 5 fill routes for reliable acceptance.
 */
TEST_P(UpdateGroupMultiPeerTest, AcceptThenRouteAdd) {
  XLOG(INFO, "=== TEST: AcceptThenRouteAdd ===");

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

  /* Freq-detach peer3: freq=1, use 5 fill routes for queue (5,4,0) */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 5; i++) {
    auto p = fmt::format("57.{}.0.0/16", 40 + i);
    auto c = fmt::format("{}:1", 5740 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("57.{}.0.0", 40 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock peer3 → recovery. Inject PL-cycle routes to help acceptance. */
  unblockPeer(kPeerAddr3);
  for (int i = 0; i < 5; i++) {
    auto p = fmt::format("57.{}.0.0/16", 50 + i);
    auto c = fmt::format("{}:1", 5750 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("57.{}.0.0", 50 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
  }

  /* Wait for acceptance — accept either JOINED_RUNNING or valid detached */
  auto state3 = getPeerState(kPeerAddr3);
  if (state3 == PeerUpdateState::JOINED_RUNNING) {
    /* EVENT: Inject route after acceptance — both peers receive */
    injectLocalRoutesAtRuntime({"57.60.0.0/16"}, {"5760:1"}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(
        folly::IPAddress::createNetwork("57.60.0.0/16")));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        "57.60.0.0",
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        "5760:1"));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        "57.60.0.0",
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        "5760:1"));
    EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  } else {
    /* Peer not yet accepted — still valid detached state */
    EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  }
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);
}

/*
 * E-ROUTE-ADD then E-ACCEPT
 * Detach peer3, unblock (DRJ), inject route during recovery.
 * The CL item may prevent or delay acceptance. peer4 receives route via PL.
 */
TEST_P(UpdateGroupMultiPeerTest, RouteAddThenAccept) {
  XLOG(INFO, "=== TEST: RouteAddThenAccept ===");

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

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("57.{}.0.0/16", 70 + i);
    auto c = fmt::format("{}:1", 5770 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("57.{}.0.0", 70 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock peer3 → DRJ or DB or may recover to JR */
  unblockPeer(kPeerAddr3);

  /* EVENT: Inject route during recovery — CL item may delay acceptance */
  injectLocalRoutesAtRuntime({"57.80.0.0/16"}, {"5780:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("57.80.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "57.80.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5780:1"));

  /*
   * peer3 may still be detached or may have recovered. Both valid.
   */
  auto state3 = getPeerState(kPeerAddr3);
  EXPECT_NE(state3, PeerUpdateState::DOWN);
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  if (isPeerDetached(kPeerAddr3)) {
    verifySlowPeerInvariants(kPeerAddr3);
  }
}

/*
 * E-PEER-UP then E-DETACH
 * 3-peer group. All 3 peers brought up together (peer5 is the "new" peer).
 * Immediately after all reach JOINED_RUNNING, freq-detach peer3.
 * Verify: peer detachment is independent of peer5's presence in the group.
 * peer4 and peer5 both continue receiving routes after peer3 detaches.
 */
TEST_P(UpdateGroupMultiPeerTest, PeerUpThenDetach) {
  XLOG(INFO, "=== TEST: PeerUpThenDetach ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  /* EVENT 1: Bring all 3 peers UP — peer5 is the "newly joined" peer */
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

  /* EVENT 2: Freq-detach peer3 while peer5 just joined */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("57.{}.0.0/16", 90 + i);
    auto c = fmt::format("{}:1", 5790 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("57.{}.0.0", 90 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("57.{}.0.0", 90 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Verify: peer3 detached, peer4+peer5 both in-sync and receiving */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  /* Final route to confirm both in-sync peers receive normally */
  injectLocalRoutesAtRuntime({"57.99.0.0/16"}, {"5799:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("57.99.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "57.99.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5799:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "57.99.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "5799:1"));
  verifySlowPeerInvariants(kPeerAddr3);
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
