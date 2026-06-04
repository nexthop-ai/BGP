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
 * [BGP++][UG2 E2E] Reconnect version propagation tests.
 *
 * Tests that ribVersion is correctly propagated through processRibDumpReq
 * when a peer reconnects. Bug 4 (D99316308) found that ribVersion defaulted
 * to 0 during reconnect RIB dump, breaking version-tracking logic.
 *
 * Learning patterns applied:
 *   P9: Always waitForPeerState(DOWN) after bringDownPeer()
 *   P10: Always sendEoRToPeer() after bringUpPeer() on reconnect
 *   P8: Use 3 peers to avoid JOINED_BLOCKED blocking entire group
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Test 4 (Critical): PeerReconnect_RibDumpSetsCorrectCachedVersion
 *
 * After a peer disconnects and reconnects, processRibDumpReq must
 * propagate ribVersion from shadowRibEntry. Without the fix, the
 * peer's lastSeenRibVersion is set to 0, breaking version tracking.
 *
 * Flow:
 * 1. 3 peers joined, inject 3 routes via peer3
 * 2. Record peer4's cached version (expect >= 3)
 * 3. Bring down peer4, bring it back up with EoR
 * 4. Assert getPeerCachedRibVersion(peer4) >= 3 (not 0)
 */
/* TODO: Re-enable when D99316308 (ribVersion in processRibDumpReq) lands.
 * Reconnect RIB dump path needs the fix to propagate ribVersion correctly.
 */
TEST_P(
    UpdateGroupReconnectVersionTest,
    DISABLED_PeerReconnect_RibDumpSetsCorrectCachedVersion) {
  XLOGF(INFO, "=== TEST: PeerReconnect_RibDumpSetsCorrectCachedVersion ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);

  /* Larger queues — reconnect tests don't need backpressure */
  setupSlowPeerComponents(20, 15, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  bringUpPeer(kPeerAddr5);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId5));
  EXPECT_TRUE(waitForEoR(peerId5));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  /* Inject 3 local routes */
  injectLocalRoutesAtRuntime({"10.1.0.0/16"}, {"101:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "101:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "101:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.1.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "101:1"));

  injectLocalRoutesAtRuntime({"10.2.0.0/16"}, {"102:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.2.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "102:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "102:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.2.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "102:1"));

  injectLocalRoutesAtRuntime({"10.3.0.0/16"}, {"103:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.3.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "103:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "103:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.3.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "103:1"));

  /* Record version before reconnect */
  auto versionBefore = getPeerCachedRibVersion(kPeerAddr4);
  XLOGF(INFO, "Peer4 cached RIB version before reconnect: {}", versionBefore);
  EXPECT_GT(versionBefore, 0)
      << "Peer4 should have non-zero version after receiving routes";

  /* P9: Bring down peer4, wait for DOWN */
  bringDownPeer(kPeerAddr4);
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DOWN));

  /* P10: Bring back up with EoR */
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId4);

  /* Wait for peer4 to rejoin */
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING, 100));

  /* Consume EoRs from peer4 */
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  /* Assert version is correctly propagated (not 0) */
  auto versionAfter = getPeerCachedRibVersion(kPeerAddr4);
  XLOGF(INFO, "Peer4 cached RIB version after reconnect: {}", versionAfter);
  EXPECT_GE(versionAfter, versionBefore)
      << "ribVersion must be propagated through RIB dump path, not default to 0";

  XLOGF(
      INFO,
      "=== TEST PASSED: PeerReconnect_RibDumpSetsCorrectCachedVersion ===");
}

/*
 * Test 10 (High): PeerReconnect_VersionChurnDuringRibDump
 *
 * While peer4 is reconnecting (RIB dump in progress), inject more routes.
 * The final cached version must reflect all routes, not just the dump set.
 */
/* TODO: Re-enable when D99316308 lands. */
TEST_P(
    UpdateGroupReconnectVersionTest,
    DISABLED_PeerReconnect_VersionChurnDuringRibDump) {
  XLOGF(INFO, "=== TEST: PeerReconnect_VersionChurnDuringRibDump ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);

  setupSlowPeerComponents(20, 15, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  bringUpPeer(kPeerAddr5);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId5));
  EXPECT_TRUE(waitForEoR(peerId5));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  /* Inject 3 initial routes */
  injectLocalRoutesAtRuntime({"20.0.0.0/8"}, {"200:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "200:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "200:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "20.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "200:1"));

  injectLocalRoutesAtRuntime({"21.0.0.0/8"}, {"210:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("21.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "21.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "210:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "21.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "210:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "21.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "210:1"));

  injectLocalRoutesAtRuntime({"22.0.0.0/8"}, {"220:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("22.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "22.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "220:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "22.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "220:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "22.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "220:1"));

  /* P9: Bring down peer4 */
  bringDownPeer(kPeerAddr4);
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DOWN));

  /* Inject 2 more routes while peer4 is down */
  injectLocalRoutesAtRuntime({"23.0.0.0/8"}, {"230:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("23.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "23.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "230:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "23.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "230:1"));

  injectLocalRoutesAtRuntime({"24.0.0.0/8"}, {"240:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("24.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "24.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "240:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "24.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "240:1"));

  /* P10: Bring up peer4 and send EoR */
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId4);

  /* Inject 2 more routes DURING the RIB dump */
  injectLocalRoutesAtRuntime({"25.0.0.0/8"}, {"250:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("25.0.0.0/8")));

  injectLocalRoutesAtRuntime({"26.0.0.0/8"}, {"260:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.0.0.0/8")));

  /* Wait for peer4 to converge and drain its queue */
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING, 100));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  drainPeerQueueCompletely(peerId4);

  /* Version must reflect ALL routes (original 3 + 2 during down + 2 during dump
   * = 7) */
  auto versionAfter = getPeerCachedRibVersion(kPeerAddr4);
  XLOGF(INFO, "Peer4 cached RIB version after churn: {}", versionAfter);
  EXPECT_GT(versionAfter, 0) << "Version must not be 0 after RIB dump";

  XLOGF(INFO, "=== TEST PASSED: PeerReconnect_VersionChurnDuringRibDump ===");
}

/*
 * Test 13 (Medium): MultiPeerReconnect_AllGetCorrectVersions
 *
 * Bring down 3 peers simultaneously, bring all back up.
 * Each must get correct cached version (not 0).
 */
/* TODO: Re-enable when D99316308 lands. */
TEST_P(
    UpdateGroupReconnectVersionTest,
    DISABLED_MultiPeerReconnect_AllGetCorrectVersions) {
  XLOGF(INFO, "=== TEST: MultiPeerReconnect_AllGetCorrectVersions ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);

  setupSlowPeerComponents(20, 15, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  bringUpPeer(kPeerAddr5);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId5));
  EXPECT_TRUE(waitForEoR(peerId5));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  /* Inject 3 local routes */
  injectLocalRoutesAtRuntime({"30.0.0.0/8"}, {"300:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "300:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "300:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "300:1"));

  injectLocalRoutesAtRuntime({"31.0.0.0/8"}, {"310:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "310:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "310:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "310:1"));

  injectLocalRoutesAtRuntime({"32.0.0.0/8"}, {"320:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "320:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "320:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "320:1"));

  /* P9: Bring down all 3 peers */
  bringDownPeer(kPeerAddr3);
  bringDownPeer(kPeerAddr4);
  bringDownPeer(kPeerAddr5);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DOWN));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::DOWN));

  /* P10: Bring all back up with EoR */
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  bringUpPeer(kPeerAddr5);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  /* Wait for all to rejoin */
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING, 100));
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING, 100));
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING, 100));

  /* Drain EoRs */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId5));
  EXPECT_TRUE(waitForEoR(peerId5));

  /* All 3 must have non-zero cached version */
  auto v3 = getPeerCachedRibVersion(kPeerAddr3);
  auto v4 = getPeerCachedRibVersion(kPeerAddr4);
  auto v5 = getPeerCachedRibVersion(kPeerAddr5);
  XLOGF(
      INFO,
      "Versions after multi-reconnect: peer3={}, peer4={}, peer5={}",
      v3,
      v4,
      v5);

  EXPECT_GT(v3, 0) << "Peer3 version must not be 0 after reconnect";
  EXPECT_GT(v4, 0) << "Peer4 version must not be 0 after reconnect";
  EXPECT_GT(v5, 0) << "Peer5 version must not be 0 after reconnect";

  XLOGF(INFO, "=== TEST PASSED: MultiPeerReconnect_AllGetCorrectVersions ===");
}

/*
 * Test 16 (Medium): DetachedPeer_VersionMonotonicity_RapidReconnect
 *
 * Rapidly reconnect peer4 three times. After each reconnect,
 * cachedRibVersion must never decrease (monotonicity).
 */
/* TODO: Re-enable when D99316308 lands. */
TEST_P(
    UpdateGroupReconnectVersionTest,
    DISABLED_DetachedPeer_VersionMonotonicity_RapidReconnect) {
  XLOGF(INFO, "=== TEST: DetachedPeer_VersionMonotonicity_RapidReconnect ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);

  setupSlowPeerComponents(20, 15, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  bringUpPeer(kPeerAddr5);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId5));
  EXPECT_TRUE(waitForEoR(peerId5));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  /* Inject initial routes */
  injectLocalRoutesAtRuntime({"40.0.0.0/8"}, {"400:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("40.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "40.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "400:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "40.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "400:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "40.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "400:1"));

  uint64_t prevVersion = 0;

  /* 3 rapid reconnect cycles */
  for (int cycle = 1; cycle <= 3; ++cycle) {
    XLOGF(INFO, "Reconnect cycle {}", cycle);

    /* P9: Bring down peer4 */
    bringDownPeer(kPeerAddr4);
    ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DOWN));

    /* Inject a route while peer4 is down */
    std::string prefix = std::to_string(40 + cycle) + ".0.0.0/8";
    std::string community = std::to_string(400 + cycle * 10) + ":1";
    injectLocalRoutesAtRuntime({prefix}, {community}, 100);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    /* Drain from peer3 and peer5 */
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        std::to_string(40 + cycle) + ".0.0.0",
        8,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        std::to_string(40 + cycle) + ".0.0.0",
        8,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));

    /* P10: Bring up and send EoR */
    bringUpPeer(kPeerAddr4);
    sendEoRToPeer(peerId4);

    ASSERT_TRUE(
        waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING, 100));
    EXPECT_TRUE(waitForEoR(peerId4));
    EXPECT_TRUE(waitForEoR(peerId4));
    drainPeerQueueCompletely(peerId4);

    /* Monotonicity check */
    auto currentVersion = getPeerCachedRibVersion(kPeerAddr4);
    XLOGF(
        INFO,
        "Cycle {} version: {} (prev: {})",
        cycle,
        currentVersion,
        prevVersion);
    EXPECT_GE(currentVersion, prevVersion)
        << "Version must never decrease across reconnect cycles";
    prevVersion = currentVersion;
  }

  EXPECT_GT(prevVersion, 0) << "Final version must be non-zero";

  XLOGF(
      INFO,
      "=== TEST PASSED: DetachedPeer_VersionMonotonicity_RapidReconnect ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupReconnectVersionTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
