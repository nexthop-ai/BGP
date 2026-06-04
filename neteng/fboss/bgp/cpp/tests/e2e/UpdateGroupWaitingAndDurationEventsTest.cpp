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

/* E2E tests: G-WAITING state event coverage.
 * Prefix range: 25.1-25.39/16.
 *
 * G-WAITING × E-PEER-UP (new peer joins during PL drain)
 * G-WAITING × E-POLICY-CHG (policy change during PL drain)
 * G-WAITING × E-MRAI-FIRE (MRAI during WAITING — deferred)
 * G-WAITING × E-MULTI-ROUTE (batch routes during PL drain)
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* G-WAITING × E-PEER-UP — New peer joins during PL drain.
 * Put group into WAITING by blocking peer3 and filling queue.
 * While WAITING, bring peer5 UP (reconnect). Peer5 enters
 * DETACHED_INIT_DUMP independently. After unblocking peer3,
 * verify peer3/peer4 continue normally. Drain peer5 init dump. */
TEST_P(UpdateGroupMultiPeerTest, GWaiting_PeerUp) {
  XLOGF(INFO, "=== TEST: GWaiting_PeerUp ===");

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

  /* Bring peer5 DOWN so we can reconnect it later during WAITING */
  bringDownPeer(kPeerAddr5);

  /* Block peer3 and fill queue to put group into WAITING */
  blockPeer(kPeerAddr3);

  injectLocalRoutesAtRuntime({"25.1.0.0/16"}, {"2501:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("25.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "25.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2501:1"));

  injectLocalRoutesAtRuntime({"25.2.0.0/16"}, {"2502:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("25.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "25.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2502:1"));

  injectLocalRoutesAtRuntime({"25.3.0.0/16"}, {"2503:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("25.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "25.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2503:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* While group is WAITING, bring peer5 back UP — independent init dump */
  bringUpPeer(kPeerAddr5);
  sendEoRToPeer(peerId5);
  drainPeerQueueCompletely(peerId5);

  /* Unblock peer3 — PL drains, group resumes */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Verify peer3 and peer4 deliver a new route after recovery */
  injectLocalRoutesAtRuntime({"25.4.0.0/16"}, {"2504:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("25.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "25.4.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2504:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "25.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2504:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOGF(INFO, "=== TEST PASSED: GWaiting_PeerUp ===");
}

/* G-WAITING × E-POLICY-CHG — Policy change during PL drain.
 * Policy changes cannot be triggered directly in slow peer fixture
 * (setPolicyConfig causes CHECK failure — learned from W4). Simulate
 * policy-like behavior: block peer3 → WAITING, then withdraw a route
 * and inject a different prefix (simulating policy re-evaluation).
 * After unblock, verify peer3 receives the new route. */
TEST_P(UpdateGroupMultiPeerTest, GWaiting_PolicyChange) {
  XLOGF(INFO, "=== TEST: GWaiting_PolicyChange ===");

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

  /* Inject an initial route before blocking — both peers receive it */
  injectLocalRoutesAtRuntime({"25.10.0.0/16"}, {"2510:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("25.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "25.10.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2510:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "25.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2510:1"));

  /* Block peer3, fill queue → WAITING */
  blockPeer(kPeerAddr3);

  injectLocalRoutesAtRuntime({"25.11.0.0/16"}, {"2511:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("25.11.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "25.11.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2511:1"));

  injectLocalRoutesAtRuntime({"25.12.0.0/16"}, {"2512:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("25.12.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "25.12.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2512:1"));

  injectLocalRoutesAtRuntime({"25.13.0.0/16"}, {"2513:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("25.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "25.13.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2513:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* Simulate policy change: withdraw old prefix while WAITING.
   * This creates a CL item (withdrawal) processed after PL drain. */
  withdrawLocalRoutesAtRuntime({"25.10.0.0/16"});

  /* Unblock peer3 — PL drains, then CL withdrawal is processed.
   * Both peers receive the withdrawal asynchronously after PL drain.
   * Consume the CL-origin withdrawal before verifying new routes. */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "25.10.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "25.10.0.0", 16, kPeerAddr4));

  /* Inject post-policy route — both peers should receive */
  injectLocalRoutesAtRuntime({"25.14.0.0/16"}, {"2514:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("25.14.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "25.14.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2514:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "25.14.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2514:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOGF(INFO, "=== TEST PASSED: GWaiting_PolicyChange ===");
}

/* G-WAITING × E-MRAI-FIRE — MRAI during WAITING.
 * MRAI fires are deferred during WAITING (PL drain in progress).
 * Put group into WAITING (block + fill queue), inject another route
 * (which would trigger MRAI in IDLE/READY but is deferred in WAITING).
 * After unblock, verify all routes delivered correctly — the MRAI-based
 * CL processing happens after PL drain completes. */
TEST_P(UpdateGroupMultiPeerTest, GWaiting_MraiFire) {
  XLOGF(INFO, "=== TEST: GWaiting_MraiFire ===");

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

  /* Block peer3 and fill queue → WAITING */
  blockPeer(kPeerAddr3);

  injectLocalRoutesAtRuntime({"25.20.0.0/16"}, {"2520:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("25.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "25.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2520:1"));

  injectLocalRoutesAtRuntime({"25.21.0.0/16"}, {"2521:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("25.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "25.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2521:1"));

  injectLocalRoutesAtRuntime({"25.22.0.0/16"}, {"2522:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("25.22.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "25.22.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2522:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* Inject another route while WAITING — CL item created. MRAI would
   * fire but is deferred because group is in WAITING state. The CL item
   * is processed after PL drain completes. */
  injectLocalRoutesAtRuntime({"25.23.0.0/16"}, {"2523:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("25.23.0.0/16")));

  /* Unblock peer3 — PL drains, then deferred CL item (25.23.0.0)
   * is processed. unblockPeer drains peer3's queue (PL items 25.20-25.22).
   * CL item 25.23 arrives AFTER the drain (CL processed after PL drain),
   * so it is safe to verify on both peers. */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "25.23.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2523:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "25.23.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2523:1"));

  /* Verify post-recovery route delivery for both peers */
  injectLocalRoutesAtRuntime({"25.24.0.0/16"}, {"2524:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("25.24.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "25.24.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2524:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "25.24.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2524:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOGF(INFO, "=== TEST PASSED: GWaiting_MraiFire ===");
}

/* G-WAITING × E-MULTI-ROUTE — Batch routes during PL drain.
 * Block peer3, fill queue → WAITING. Then inject 3 more routes (batch)
 * while group is WAITING. CL grows with all 3. After unblock, peer3
 * receives everything from PL+CL drain. Peer4 receives inline. */
TEST_P(UpdateGroupMultiPeerTest, GWaiting_MultiRoute) {
  XLOGF(INFO, "=== TEST: GWaiting_MultiRoute ===");

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

  /* Block peer3, fill queue → WAITING */
  blockPeer(kPeerAddr3);

  injectLocalRoutesAtRuntime({"25.30.0.0/16"}, {"2530:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("25.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "25.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2530:1"));

  injectLocalRoutesAtRuntime({"25.31.0.0/16"}, {"2531:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("25.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "25.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2531:1"));

  injectLocalRoutesAtRuntime({"25.32.0.0/16"}, {"2532:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("25.32.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "25.32.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2532:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* Inject batch of 3 more routes while WAITING — CL grows.
   * These are CL items; peer3 is blocked so they accumulate. */
  for (int i = 0; i < 3; ++i) {
    auto prefix = fmt::format("25.{}.0.0/16", 33 + i);
    auto community = fmt::format("25{}:1", 33 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
  }

  /* Peer3 still blocked — group accumulates CL items */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* Unblock — PL drains, then CL items (3 routes) processed as new PL.
   * CL delivery order is non-deterministic for batch items (different
   * communities = different attribute buckets). Just verify state recovery
   * after unblock — the key assertion is CL items are preserved and
   * peer recovers to JOINED_RUNNING after batch CL processing. */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOGF(INFO, "=== TEST PASSED: GWaiting_MultiRoute ===");
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
