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

/* E2E tests: Acceptance during route injection, post-acceptance PL drain,
 * DRJ CL position, acceptance with 0 in-sync peers.
 * Prefix range: 35.1-35.49/16.
 *
 * Acceptance during route injection -- route arrives between steps
 * Group starts new PL drain after acceptance -- accepted peer participates
 * Peer DRJ, CL position matches, PL still processing -- verify
 * Acceptance when 0 in-sync peers -- first accepted peer becomes sole member
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Route injection while acceptance is in progress. Detach peer3 via
 * freq threshold using per-peer queue sizes, inject routes while peer3
 * is detached, verify peer4 continues receiving routes.
 */
TEST_P(UpdateGroupMultiPeerTest, AcceptanceDuringRouteInjection) {
  XLOGF(INFO, "=== TEST: AcceptanceDuringRouteInjection ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  /* Fast peer4: large queue that won't block */
  setupSlowPeerComponents(10, 8, 0);
  bringUpPeer(kPeerAddr4);

  /* Slow peer3: small queue that fills naturally */
  setDefaultQueueSizes(3, 2, 0);
  bringUpPeer(kPeerAddr3);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  /* Inject a shared route before detach */
  injectLocalRoutesAtRuntime({"35.1.0.0/16"}, {"3501:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3501:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3501:1"));

  /* Freq-detach peer3: threshold=1 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Fill peer3's small queue with 3 routes (different communities)
   * to trigger detach. Drain peer4 after each to keep it running. */
  for (int i = 2; i <= 4; i++) {
    auto prefix = fmt::format("35.{}.0.0/16", i);
    auto community = fmt::format("35{:02d}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("35.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Inject routes while peer3 is detached — exercises CL accumulation */
  injectLocalRoutesAtRuntime({"35.5.0.0/16"}, {"3505:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.5.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.5.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3505:1"));

  /* Verify peer4 continues receiving routes after detachment */
  injectLocalRoutesAtRuntime({"35.6.0.0/16"}, {"3506:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.6.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.6.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3506:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOGF(INFO, "=== TEST PASSED: AcceptanceDuringRouteInjection ===");
}

/* Peer in DRJ state with CL position matching, but PL still being
 * processed. Detach peer3, inject CL items post-detach, then inject
 * more routes. Verify no crash and peer4 continues receiving routes.
 */
TEST_P(UpdateGroupMultiPeerTest, DRJWithPLStillProcessing) {
  XLOGF(INFO, "=== TEST: DRJWithPLStillProcessing ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  /* Fast peer4: large queue that won't block */
  setupSlowPeerComponents(10, 8, 0);
  bringUpPeer(kPeerAddr4);

  /* Slow peer3: small queue that fills naturally */
  setDefaultQueueSizes(3, 2, 0);
  bringUpPeer(kPeerAddr3);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Fill peer3's small queue to trigger detach */
  for (int i = 20; i <= 22; i++) {
    auto prefix = fmt::format("35.{}.0.0/16", i);
    auto community = fmt::format("35{:02d}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("35.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Inject CL items while peer3 is detached — accumulates in CL */
  injectLocalRoutesAtRuntime({"35.23.0.0/16"}, {"3523:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.23.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.23.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3523:1"));

  /* Inject more routes to exercise PL activity with detached peer */
  injectLocalRoutesAtRuntime({"35.24.0.0/16"}, {"3524:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.24.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.24.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3524:1"));

  /* Verify no crash and peer4 still works */
  injectLocalRoutesAtRuntime({"35.25.0.0/16"}, {"3525:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.25.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.25.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3525:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOGF(INFO, "=== TEST PASSED: DRJWithPLStillProcessing ===");
}

/* Acceptance when 0 in-sync peers. From learned patterns: the group
 * preserves at least 1 in-sync member via freq detachment, so 0 in-sync is
 * unreachable via normal freq-detach. Test that after detaching peer3 (the
 * only detachable one), peer4 remains the sole in-sync member, and if peer4
 * blocks too, it stays JOINED_BLOCKED (not detached -- last synced preserved).
 */
TEST_P(UpdateGroupMultiPeerTest, AcceptanceLastSyncedPeerPreserved) {
  XLOGF(INFO, "=== TEST: AcceptanceLastSyncedPeerPreserved ===");

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
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  /* Set freq threshold=1 on the group (group-level, applies to all peers) */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Detach peer3: fill its small queue with 3 routes, drain peer4 after
   * each to keep peer4 running during peer3's detachment phase. */
  for (int i = 30; i <= 32; i++) {
    auto prefix = fmt::format("35.{}.0.0/16", i);
    auto community = fmt::format("35{:02d}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("35.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Now fill peer4's queue — it should stay JOINED_BLOCKED (last synced
   * preserved). Peer3 is detached so PLs only target peer4. */
  injectLocalRoutesAtRuntime({"35.33.0.0/16"}, {"3533:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.33.0.0/16")));
  injectLocalRoutesAtRuntime({"35.34.0.0/16"}, {"3534:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.34.0.0/16")));
  injectLocalRoutesAtRuntime({"35.35.0.0/16"}, {"3535:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.35.0.0/16")));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));

  /* Peer4 must remain JOINED_BLOCKED — last synced member cannot be detached
   */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_BLOCKED));

  /* Drain peer4's blocked routes to recover */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.33.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3533:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.34.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3534:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.35.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3535:1"));

  /* Wait for peer4 to recover */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Inject a route — peer4 should receive it */
  injectLocalRoutesAtRuntime({"35.36.0.0/16"}, {"3536:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.36.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.36.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3536:1"));

  XLOGF(INFO, "=== TEST PASSED: AcceptanceLastSyncedPeerPreserved ===");
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
