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
 * E2E tests: JOINED_BLOCKED EoR and DETACHED_INIT_DUMP withdrawal/unblock
 *
 * Prefix range: 18.x.0.0/16 (1-29)
 *
 * Tests:
 *   P-JB × E-EOR — N/A (peer blocked, EoR already processed)
 *   P-DID × E-ROUTE-WD — Withdrawal appended to CL
 *   P-DID × E-UNBLOCK — N/A (not blocked during init dump)
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {
/*
 * P-DID × E-ROUTE-WD
 * Withdrawal while peer is in DETACHED_INIT_DUMP. The withdrawal goes to
 * CL. Peer continues init dump unaffected. Peer4 receives the withdrawal.
 *
 * Pattern: freq-threshold detach → down → unblock + up → DID.
 * Then withdraw a shared route — peer4 receives wd.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, DetachedInitDump_RouteWithdraw) {
  XLOG(INFO, "=== TEST: DetachedInitDump_RouteWithdraw ===");

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

  /* Inject a shared route (both peers get it) */
  injectLocalRoutesAtRuntime({"18.10.0.0/16"}, {"1810:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("18.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "18.10.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1810:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "18.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1810:1"));

  /* Detach peer3 via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"18.11.0.0/16"}, {"1811:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("18.11.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "18.11.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1811:1"));
  injectLocalRoutesAtRuntime({"18.12.0.0/16"}, {"1812:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("18.12.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "18.12.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1812:1"));
  injectLocalRoutesAtRuntime({"18.13.0.0/16"}, {"1813:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("18.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "18.13.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1813:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* down → unblock + up → DETACHED_INIT_DUMP */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));
  unblockPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr3);

  /* Withdraw the shared route while peer3 is in DID — peer4 gets wd */
  withdrawLocalRoutesAtRuntime({"18.10.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "18.10.0.0", 16, kPeerAddr4));

  /* Peer4 still functional */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: DetachedInitDump_RouteWithdraw ===");
}

/*
 * P-DID × E-UNBLOCK
 * N/A scenario: peer in DETACHED_INIT_DUMP just reconnected and started
 * init dump — it should NOT be blocked. unblockPeer is a no-op.
 * Verify peer is not blocked and state is stable.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, DetachedInitDump_UnblockNoop) {
  XLOG(INFO, "=== TEST: DetachedInitDump_UnblockNoop ===");

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
  injectLocalRoutesAtRuntime({"18.20.0.0/16"}, {"1820:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("18.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "18.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1820:1"));
  injectLocalRoutesAtRuntime({"18.21.0.0/16"}, {"1821:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("18.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "18.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1821:1"));
  injectLocalRoutesAtRuntime({"18.22.0.0/16"}, {"1822:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("18.22.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "18.22.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1822:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* down → unblock + up → DETACHED_INIT_DUMP */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));
  unblockPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr3);

  /*
   * N/A: peer just reconnected, not blocked. unblockPeer is idempotent.
   */
  unblockPeer(kPeerAddr3);
  EXPECT_FALSE(isPeerQueueBlocked(peerId3));

  /* Peer4 still functional — inject a route to prove it */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  injectLocalRoutesAtRuntime({"18.23.0.0/16"}, {"1823:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("18.23.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "18.23.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1823:1"));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: DetachedInitDump_UnblockNoop ===");
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
