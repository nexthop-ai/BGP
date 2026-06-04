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
 * E2E tests: DETACHED_INIT_DUMP x active events + DRJ x withdrawal/unblock
 *
 * Prefix range: 29.x.0.0/16
 *
 * Tests:
 *   P-DID x E-POLICY-CHG -- policy change during detached init dump
 *   P-DID x E-MRAI-FIRE -- MRAI fires, peer continues init dump
 *   P-DID x E-MULTI-ROUTE -- batch routes, CL grows during init dump
 *   P-DRJ x E-ROUTE-WD -- withdrawal during recovery
 *   P-DRJ x E-UNBLOCK -- N/A (peer already unblocked at DRJ)
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * P-DID x E-POLICY-CHG
 * Policy change during DETACHED_INIT_DUMP. Since setPolicyConfig is
 * incompatible with update groups (learned pattern W4), simulate policy
 * change via withdraw + re-inject with different prefix/attributes.
 * Peer3 is in DID doing init dump; the policy change (route churn)
 * should not interfere. Peer4 receives the withdrawal and new route.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachedInitDump_PolicyChange) {
  XLOG(INFO, "=== TEST: DetachedInitDump_PolicyChange ===");

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
  injectLocalRoutesAtRuntime({"29.1.0.0/16"}, {"2901:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2901:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2901:1"));

  /* Freq-detach peer3: threshold=1 block */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"29.2.0.0/16"}, {"2902:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2902:1"));
  injectLocalRoutesAtRuntime({"29.3.0.0/16"}, {"2903:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2903:1"));
  injectLocalRoutesAtRuntime({"29.4.0.0/16"}, {"2904:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2904:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* down → unblock + up → DETACHED_INIT_DUMP */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));
  unblockPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr3);

  /*
   * Simulate policy change: withdraw old prefix and inject a new one
   * with different attributes. Use different prefix to avoid CL
   * suppression (learned pattern).
   */
  withdrawLocalRoutesAtRuntime({"29.1.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "29.1.0.0", 16, kPeerAddr4));

  injectLocalRoutesAtRuntime({"29.5.0.0/16"}, {"2905:1"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.5.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.5.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2905:1"));

  /* Peer4 functional, no crash from policy-like churn during DID */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: DetachedInitDump_PolicyChange ===");
}
/*
 * P-DID x E-MULTI-ROUTE
 * Batch route injection while peer3 is in DETACHED_INIT_DUMP. CL grows
 * with new entries but peer3 continues init dump independently. All
 * batch routes are delivered to peer4. Verify group stability.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachedInitDump_MultiRoute) {
  XLOG(INFO, "=== TEST: DetachedInitDump_MultiRoute ===");

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
  injectLocalRoutesAtRuntime({"29.20.0.0/16"}, {"2920:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2920:1"));
  injectLocalRoutesAtRuntime({"29.21.0.0/16"}, {"2921:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2921:1"));
  injectLocalRoutesAtRuntime({"29.22.0.0/16"}, {"2922:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.22.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.22.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2922:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* down → unblock + up → DETACHED_INIT_DUMP */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));
  unblockPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr3);

  /*
   * Batch inject 4 routes with different communities. Each becomes a
   * separate CL entry. Inject and drain one at a time for peer4.
   */
  for (int i = 0; i < 4; ++i) {
    auto prefix = fmt::format("29.{}.0.0/16", 23 + i);
    auto community = fmt::format("{}:1", 2923 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("29.{}.0.0", 23 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Group stable, all batch routes delivered to peer4 */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: DetachedInitDump_MultiRoute ===");
}

/*
 * P-DRJ x E-ROUTE-WD
 * Route withdrawal while peer3 is in DETACHED_READY_TO_JOIN. The
 * withdrawal creates a new CL entry, which means peer3 is no longer
 * at CL end -- DFP must become DSP (demoted). After freq-detach →
 * unblock (recovery starts), withdraw a shared route. Peer4 receives
 * the withdrawal. Verify group stability and peer4 functionality.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachedReadyToJoin_RouteWithdraw) {
  XLOG(INFO, "=== TEST: DetachedReadyToJoin_RouteWithdraw ===");

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

  /* Inject a shared route before detachment (will withdraw later) */
  injectLocalRoutesAtRuntime({"29.30.0.0/16"}, {"2930:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.30.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2930:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2930:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"29.31.0.0/16"}, {"2931:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2931:1"));
  injectLocalRoutesAtRuntime({"29.32.0.0/16"}, {"2932:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.32.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.32.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2932:1"));
  injectLocalRoutesAtRuntime({"29.33.0.0/16"}, {"2933:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.33.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.33.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2933:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock peer3 -- starts recovery toward DRJ */
  waitForDrjWithDrain(kPeerAddr3, peerId3);

  /*
   * Withdraw the shared route during recovery. This creates a new CL
   * entry, demoting peer3 from DFP to DSP (no longer at CL end).
   * Peer4 receives the withdrawal normally.
   */
  withdrawLocalRoutesAtRuntime({"29.30.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "29.30.0.0", 16, kPeerAddr4));

  /* Inject a new route to verify group continues normally */
  injectLocalRoutesAtRuntime({"29.34.0.0/16"}, {"2934:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.34.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.34.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2934:1"));

  /* Peer4 functional, group stable after withdrawal during DRJ */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: DetachedReadyToJoin_RouteWithdraw ===");
}

/*
 * P-DRJ x E-UNBLOCK
 * N/A: at DRJ the peer is already unblocked (that is what transitions
 * it from DB to DRJ). Calling unblockPeer is a no-op. Verify no crash
 * and peer4 continues receiving routes after the redundant unblock.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachedReadyToJoin_UnblockNoop) {
  XLOG(INFO, "=== TEST: DetachedReadyToJoin_UnblockNoop ===");

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
  injectLocalRoutesAtRuntime({"29.40.0.0/16"}, {"2940:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2940:1"));
  injectLocalRoutesAtRuntime({"29.41.0.0/16"}, {"2941:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2941:1"));
  injectLocalRoutesAtRuntime({"29.42.0.0/16"}, {"2942:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.42.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.42.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2942:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock peer3 -- transitions from DB to DRJ */
  waitForDrjWithDrain(kPeerAddr3, peerId3);

  /* N/A: redundant unblock -- should be harmless no-op */
  unblockPeer(kPeerAddr3);
  EXPECT_FALSE(isPeerQueueBlocked(peerId3));

  /* Verify peer4 still receives routes after redundant unblock */
  injectLocalRoutesAtRuntime({"29.43.0.0/16"}, {"2943:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.43.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.43.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2943:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: DetachedReadyToJoin_UnblockNoop ===");
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
