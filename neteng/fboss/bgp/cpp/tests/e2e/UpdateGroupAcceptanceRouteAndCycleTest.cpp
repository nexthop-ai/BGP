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

/* E2E tests: Acceptance during route injection, PL drain participation,
 * PL processing, zero in-sync acceptance, and detach-rejoin cycling.
 * Prefix: 31.x/16. */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Acceptance during route injection — route arrives between
 * bitmap steps. Detach peer3 via freq threshold, then unblock so peer3
 * enters DETACHED_READY_TO_JOIN. Meanwhile inject a route (peer4
 * receives it). After group reaches IDLE and accepts peer3, inject
 * another route — both peers should receive it, confirming accepted
 * peer participates in new PL drains.
 */
TEST_P(UpdateGroupMultiPeerTest, AcceptanceDuringRouteInjection) {
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* Per-peer queues: large for fast peer, small for slow peer */
  bringUpPeer(kPeerAddr4);
  setDefaultQueueSizes(3, 2, 0);
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Inject a shared route before detachment */
  injectLocalRoutesAtRuntime({"31.1.0.0/16"}, {"3101:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3101:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3101:1"));

  /* Freq-detach peer3: threshold=1 block */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"31.2.0.0/16"}, {"3102:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3102:1"));
  injectLocalRoutesAtRuntime({"31.3.0.0/16"}, {"3103:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3103:1"));
  injectLocalRoutesAtRuntime({"31.4.0.0/16"}, {"3104:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3104:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock — peer3 should transition toward DRJ */
  unblockPeer(kPeerAddr3);

  /* Inject a route while peer3 is recovering — peer4 gets it via PL */
  injectLocalRoutesAtRuntime({"31.5.0.0/16"}, {"3105:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.5.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.5.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3105:1"));

  /* Wait for acceptance — peer3 should eventually reach JOINED_RUNNING */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));

  /* Consume CL-origin route 31.5 from peer3's queue (pushed during recovery) */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.5.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3105:1"));

  /* Post-acceptance: inject a new route — both peers should receive it */
  injectLocalRoutesAtRuntime({"31.6.0.0/16"}, {"3106:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.6.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.6.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3106:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.6.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3106:1"));

  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);
}

/*
 * Group starts new PL drain after acceptance — accepted peer
 * participates. Detach peer3, inject routes (CL items), unblock.
 * After acceptance, inject a fresh route. Both peers should receive
 * it, proving peer3 is part of the new PL drain cycle.
 */
TEST_P(UpdateGroupMultiPeerTest, AcceptedPeerParticipatesInNewPL) {
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* Per-peer queues: large for fast peer, small for slow peer */
  bringUpPeer(kPeerAddr4);
  setDefaultQueueSizes(3, 2, 0);
  bringUpPeer(kPeerAddr3);
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
  injectLocalRoutesAtRuntime({"31.10.0.0/16"}, {"3110:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3110:1"));
  injectLocalRoutesAtRuntime({"31.11.0.0/16"}, {"3111:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.11.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.11.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3111:1"));
  injectLocalRoutesAtRuntime({"31.12.0.0/16"}, {"3112:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.12.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.12.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3112:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Unblock and wait for acceptance */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));

  /* New PL drain: inject route — both peers should participate */
  injectLocalRoutesAtRuntime({"31.13.0.0/16"}, {"3113:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.13.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3113:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.13.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3113:1"));

  /* Inject a second route to confirm PL drain continues normally */
  injectLocalRoutesAtRuntime({"31.14.0.0/16"}, {"3114:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.14.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.14.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3114:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.14.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3114:1"));

  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);
}

/*
 * Peer DRJ, CL position matches, but PL still being processed.
 * Detach peer3 via freq, inject a CL item, unblock peer3. While CL is
 * being processed peer3 enters DRJ. Wait for acceptance to complete
 * and verify the accepted peer receives subsequent routes correctly.
 * Uses queue (5,4,0) to avoid re-blocking from CL batch.
 */
TEST_P(UpdateGroupMultiPeerTest, AcceptDuringPLProcessing) {
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* Per-peer queues: large for fast peer, capacity=5 for slow peer
   * to hold 3 fill routes + 1 CL item while still triggering blocking */
  bringUpPeer(kPeerAddr4);
  setDefaultQueueSizes(5, 2, 0);
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Shared route before detachment */
  injectLocalRoutesAtRuntime({"31.20.0.0/16"}, {"3120:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.20.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3120:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3120:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"31.21.0.0/16"}, {"3121:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3121:1"));
  injectLocalRoutesAtRuntime({"31.22.0.0/16"}, {"3122:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.22.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.22.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3122:1"));
  injectLocalRoutesAtRuntime({"31.23.0.0/16"}, {"3123:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.23.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.23.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3123:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock peer3 first, then inject CL item while recovering.
   * CL injection before unblock races with the acceptance check because
   * the CL consumer runs asynchronously and may not process items before
   * collapse. Injecting after unblock avoids this race. */
  unblockPeer(kPeerAddr3);

  /* Inject CL item while peer3 is recovering — peer4 gets it via PL */
  injectLocalRoutesAtRuntime({"31.24.0.0/16"}, {"3124:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.24.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.24.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3124:1"));

  /* Wait for acceptance after CL processes 31.24 */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));

  /* Consume CL-origin route 31.24 from peer3's queue */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.24.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3124:1"));

  /* Verify accepted peer works in subsequent PL drain */
  injectLocalRoutesAtRuntime({"31.25.0.0/16"}, {"3125:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.25.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.25.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3125:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.25.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3125:1"));

  verifySlowPeerInvariants(kPeerAddr3);
}

/* Acceptance when 0 in-sync peers — detach peer3 via freq, bring
 * peer4 and peer5 DOWN. Unblock peer3 — becomes sole active member.
 * No CL items: all routes injected before peer3 detaches, and no
 * routes after — ensures clean acceptance without collapse race. */
TEST_P(UpdateGroupMultiPeerTest, AcceptWithZeroInSyncPeers) {
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  /* Per-peer queues: peer4/5 large (never block), peer3 small */
  bringUpPeer(kPeerAddr5);
  bringUpPeer(kPeerAddr4);
  setDefaultQueueSizes(3, 2, 0);
  bringUpPeer(kPeerAddr3);
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

  /* Freq-detach peer3: threshold=1, 3 fill routes > hwm=2 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; ++i) {
    auto prefix = fmt::format("31.{}.0.0/16", 30 + i);
    auto prefixStr = fmt::format("31.{}.0.0", 30 + i);
    auto comm = fmt::format("{}:1", 3130 + i);
    injectLocalRoutesAtRuntime({prefix}, {comm}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        prefixStr,
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        comm));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        prefixStr,
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        comm));
  }

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Bring peer4 and peer5 DOWN — now 0 in-sync peers
   * (1 detached + 2 down) */
  bringDownPeer(kPeerAddr4);
  bringDownPeer(kPeerAddr5);

  /* Unblock peer3 — 0 CL items, immediate acceptance as sole member */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Inject a route — peer3 should receive it as the sole in-sync member */
  injectLocalRoutesAtRuntime({"31.35.0.0/16"}, {"3135:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.35.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.35.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3135:1"));

  verifySlowPeerInvariants(kPeerAddr3);
}

/*
 * Repeat detach-rejoin 5 times — no memory growth from orphaned
 * entries. Cycle peer3 through detach→unblock→accept 5 times using
 * freq threshold=1. Each cycle should complete cleanly with peer3
 * returning to JOINED_RUNNING. After all cycles, inject a final route
 * and verify both peers receive it normally.
 */
TEST_P(UpdateGroupMultiPeerTest, RepeatDetachRejoinCycle) {
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* Per-peer queues: large for fast peer, small for slow peer */
  bringUpPeer(kPeerAddr4);
  setDefaultQueueSizes(3, 2, 0);
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  for (int cycle = 0; cycle < 5; ++cycle) {
    /* Set freq threshold=1 (reset each cycle since acceptance clears it) */
    setSlowPeerThresholds(
        kPeerAddr3,
        std::chrono::milliseconds(600000),
        1,
        std::chrono::milliseconds(60000));

    /* Block and fill queue to trigger freq detachment */
    blockPeer(kPeerAddr3);
    /* 3 fill routes per cycle to block peer3 queue (3,2,0): need >2 items */
    for (int i = 0; i < 3; ++i) {
      auto prefix = fmt::format("31.{}.0.0/16", 40 + cycle * 3 + i);
      auto prefixStr = fmt::format("31.{}.0.0", 40 + cycle * 3 + i);
      auto comm = fmt::format("{}:1", 3140 + cycle * 3 + i);
      injectLocalRoutesAtRuntime({prefix}, {comm}, 150);
      ASSERT_TRUE(
          waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
      EXPECT_TRUE(verifyRouteAdd(
          "v4",
          prefixStr,
          16,
          kPeerAddr4,
          getExpectedNexthop(kPeerAddr4),
          "4200000001",
          comm));
    }

    ASSERT_TRUE(
        waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

    /* Unblock and wait for acceptance back to JOINED_RUNNING */
    unblockPeer(kPeerAddr3);
    ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
    EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  }

  /* After 5 cycles, verify both peers still function normally */
  injectLocalRoutesAtRuntime({"31.60.0.0/16"}, {"3160:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.60.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.60.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3160:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.60.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3160:1"));

  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);
}

INSTANTIATE_TEST_SUITE_P(
    NoSerialization,
    UpdateGroupMultiPeerTest,
    ::testing::Values(kNoSerialization));

INSTANTIATE_TEST_SUITE_P(
    Serialized,
    UpdateGroupMultiPeerTest,
    ::testing::Values(kWithSerialization));

} // namespace bgp
} // namespace facebook
