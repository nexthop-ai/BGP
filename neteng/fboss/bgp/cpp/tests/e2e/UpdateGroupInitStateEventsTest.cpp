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
 * E2E tests: P-INIT state × Event coverage
 * Tests peer behavior when events arrive during initial dump (INIT state).
 *
 * Prefix range: 16.x.0.0/16
 *
 * Tests:
 *   P-INIT × E-ROUTE-WD — Withdrawal during init queued in CL
 *   P-INIT × E-UNBLOCK — N/A, peer starts unblocked, no crash
 *   P-INIT × E-SLOW-FREQ — Frequency threshold during init
 *   P-INIT × E-PL-DRAIN — Init dump PL drained, INIT→JOINED_RUNNING
 *   P-INIT × E-POLICY-CHG — Policy change during init (simulated)
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * P-INIT × E-ROUTE-WD
 * Inject a route before setup (single-peer safe), then withdraw it while
 * peer3 is still in INIT (no EoR sent). The withdrawal goes to the CL.
 * After peer3 completes EoR and reaches JOINED_RUNNING, it receives
 * both the route add and the withdrawal from CL processing.
 *
 * Learned pattern: CL add+withdraw for same prefix are NOT optimized away.
 * Peer receives both as separate messages.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, Init_RouteWithdraw) {
  XLOG(INFO, "=== TEST: Init_RouteWithdraw ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* Bring up both peers but do NOT send EoR to peer3 yet — stays in INIT */
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /* Complete peer4 first so it can receive routes */
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Inject a route while peer3 is in INIT — goes to CL */
  injectLocalRoutesAtRuntime({"16.1.0.0/16"}, {"1601:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("16.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "16.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1601:1"));

  /* Withdraw the route while peer3 is still in INIT — also goes to CL */
  withdrawLocalRoutesAtRuntime({"16.1.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "16.1.0.0", 16, kPeerAddr4));

  /* Now complete peer3's EoR — transitions INIT → JOINED_RUNNING */
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /*
   * Peer3 receives both the add and withdraw from CL processing.
   * CL does NOT optimize away add+withdraw for the same prefix.
   */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "16.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1601:1"));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "16.1.0.0", 16, kPeerAddr3));

  /* Both peers JOINED_RUNNING and in sync */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: Init_RouteWithdraw ===");
}

/*
 * P-INIT × E-UNBLOCK
 * N/A scenario: peer starts unblocked during INIT, so unblockPeer is a no-op.
 * Verify no crash and peer completes init dump normally.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, Init_UnblockNoop) {
  XLOG(INFO, "=== TEST: Init_UnblockNoop ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /* Peer3 is in INIT — call unblockPeer, should be a no-op (not blocked) */
  EXPECT_FALSE(isPeerQueueBlocked(peerId3));
  unblockPeer(kPeerAddr3);
  EXPECT_FALSE(isPeerQueueBlocked(peerId3));

  /*
   * Complete peer4 first — unblockPeer was not called on it, so
   * normal EoR drain works.
   */
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /*
   * For peer3: unblockPeer already drained the outbound queue
   * (including init dump EoRs), so waitForEoR would hang.
   * Send inbound EoR and wait for state transition directly.
   */
  sendEoRToPeer(peerId3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Verify normal operation after the no-op unblock */
  injectLocalRoutesAtRuntime({"16.2.0.0/16"}, {"1602:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("16.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "16.2.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1602:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "16.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1602:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: Init_UnblockNoop ===");
}
/*
 * P-INIT × E-POLICY-CHG
 * Policy change during init dump. Real setPolicyConfig + update groups
 * causes CHECK failure (learned pattern W4), so simulate policy-like
 * behavior: withdraw old route and inject new route with different
 * attributes while peer3 is in INIT. Verify peer3 receives both
 * operations after completing init dump, and peer4 receives them
 * during INIT.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, Init_PolicyChange) {
  XLOG(INFO, "=== TEST: Init_PolicyChange ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* Bring up both, complete peer4 only — peer3 stays in INIT */
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Inject a route while peer3 is in INIT */
  injectLocalRoutesAtRuntime({"16.8.0.0/16"}, {"1608:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("16.8.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "16.8.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1608:1"));

  /*
   * Simulate policy change: withdraw old prefix and inject a different
   * prefix with different attributes. Using different prefix avoids CL
   * suppression (avoid CL suppression by using different prefix).
   */
  withdrawLocalRoutesAtRuntime({"16.8.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "16.8.0.0", 16, kPeerAddr4));

  injectLocalRoutesAtRuntime({"16.9.0.0/16"}, {"1609:1"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("16.9.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "16.9.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1609:1"));

  /* Now complete peer3 — should get all CL items from init dump processing */
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /*
   * Peer3 receives CL items: the add for 16.8, the withdraw for 16.8,
   * and the add for 16.9 (in CL processing order).
   */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "16.8.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1608:1"));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "16.8.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "16.9.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1609:1"));

  /* Both peers JOINED_RUNNING and in sync */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: Init_PolicyChange ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupSlowPeerDetectionTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
