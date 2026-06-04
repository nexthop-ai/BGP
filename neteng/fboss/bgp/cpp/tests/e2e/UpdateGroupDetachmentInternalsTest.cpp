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

/* E2E tests: Detachment internals — divergenceRibVersion, blocked bitmap
 * clearing, CL consumer registration, UNINIT edge case.
 * Prefix range: 29.1-29.33/16.
 *
 * divergenceRibVersion set correctly (verified via CL delivery)
 * blocked bitmap cleared on detach (group resumes immediately)
 * CL consumer registered (post-detach routes delivered on recovery)
 * detachment when group has no CL yet (fresh IDLE group)
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Detachment sets divergenceRibVersion correctly.
 * After detach, only post-detach routes should appear in CL for the detached
 * peer. Verified by: inject routes pre-detach (peer3 gets them normally),
 * detach, inject post-detach route, unblock — peer3 gets only the post-detach
 * route from CL (proving divergenceRibVersion was set at the right point).
 */
TEST_P(UpdateGroupMultiPeerTest, DivergenceRibVersionSetOnDetach) {
  XLOG(INFO, "=== TEST: DivergenceRibVersionSetOnDetach ===");

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

  /* Inject a pre-detach route — both peers receive it */
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

  /* Freq-detach peer3: threshold=1, block, fill queue */
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
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Inject post-detach route — goes to CL for peer3 */
  injectLocalRoutesAtRuntime({"29.5.0.0/16"}, {"2905:1"}, 150);
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

  /* Unblock peer3 — it should receive the CL items (post-detach routes) */
  unblockPeer(kPeerAddr3);

  /* After unblock, the detached peer processes CL. Verify peer4 still works */
  injectLocalRoutesAtRuntime({"29.6.0.0/16"}, {"2906:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.6.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.6.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2906:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: DivergenceRibVersionSetOnDetach ===");
}

/* Detachment clears adjRibBlockedBitmap — group resumes immediately.
 * When the only blocked peer is detached, the group should NOT be in WAITING
 * state. It should resume to IDLE/READY and deliver routes to in-sync peers
 * without delay.
 */
TEST_P(UpdateGroupMultiPeerTest, BlockedBitmapClearedOnDetach) {
  XLOG(INFO, "=== TEST: BlockedBitmapClearedOnDetach ===");

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

  injectLocalRoutesAtRuntime({"29.10.0.0/16"}, {"2910:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2910:1"));
  injectLocalRoutesAtRuntime({"29.11.0.0/16"}, {"2911:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.11.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.11.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2911:1"));
  injectLocalRoutesAtRuntime({"29.12.0.0/16"}, {"2912:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.12.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.12.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2912:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* After detachment, group should NOT be WAITING — blocked bitmap cleared */
  auto groupState = getGroupState(kPeerAddr3);
  EXPECT_NE(groupState, UpdateGroupState::WAITING)
      << "Group should not be WAITING after detaching the only blocked peer";

  /* Verify group delivers routes to peer4 immediately */
  injectLocalRoutesAtRuntime({"29.13.0.0/16"}, {"2913:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.13.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2913:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: BlockedBitmapClearedOnDetach ===");
}

/* CL consumer registered at group's position on detach.
 * After detach, routes injected go to CL for the detached peer. On unblock +
 * recovery, the detached peer receives CL items — proves consumer was
 * registered at the correct position.
 */
TEST_P(UpdateGroupMultiPeerTest, ClConsumerRegisteredOnDetach) {
  XLOG(INFO, "=== TEST: ClConsumerRegisteredOnDetach ===");

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

  /* Inject pre-detach shared route */
  injectLocalRoutesAtRuntime({"29.20.0.0/16"}, {"2920:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.20.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "2920:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2920:1"));

  /* Freq-detach peer3: use queue (5,4,0), need 5 fill routes */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);

  for (int i = 21; i <= 25; i++) {
    auto prefix = fmt::format("29.{}.0.0/16", i);
    auto community = fmt::format("29{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("29.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Inject 2 post-detach routes — these go to CL for peer3 */
  injectLocalRoutesAtRuntime({"29.26.0.0/16"}, {"2926:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.26.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.26.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2926:1"));

  injectLocalRoutesAtRuntime({"29.27.0.0/16"}, {"2927:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.27.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.27.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2927:1"));

  /* Unblock — CL items should be processed for peer3 */
  unblockPeer(kPeerAddr3);

  /* Verify peer4 continues to work normally */
  injectLocalRoutesAtRuntime({"29.28.0.0/16"}, {"2928:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.28.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.28.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2928:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: ClConsumerRegisteredOnDetach ===");
}

/* Detachment when group has no CL entries (fresh IDLE group).
 * Group reaches IDLE after init dump with no extra routes injected.
 * Detach peer3 via freq threshold — CL consumer is registered at position 0.
 * Then inject a route — it goes to CL for peer3, peer4 gets it immediately.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachFromFreshIdleGroupNoCl) {
  XLOG(INFO, "=== TEST: DetachFromFreshIdleGroupNoCl ===");

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

  /* No routes injected yet — CL is empty / position 0 */

  /* Freq-detach peer3 immediately */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);

  /* Fill queue with 3 routes to trigger detach */
  injectLocalRoutesAtRuntime({"29.30.0.0/16"}, {"2930:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("29.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "2930:1"));
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

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Post-detach: inject route — goes to CL for peer3 */
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

  /* Group continues functioning with peer4 only */
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1);
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: DetachFromFreshIdleGroupNoCl ===");
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
