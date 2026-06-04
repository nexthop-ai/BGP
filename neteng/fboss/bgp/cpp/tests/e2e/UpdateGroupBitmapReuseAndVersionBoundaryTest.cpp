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

/* E2E tests: Bitmap reuse/consistency and version boundary cases.
 * Prefix: 32.x/16. Bitmap bit reuse after peer down — new peer gets
 * same bit, no stale state. syncBitmap AND establishedBitmap consistency
 * — sync is subset of established. All bitmaps cleared on peer down — no
 * stale bits. divergenceRibVersion = 0 — detach before any CL consumed.
 * divergenceRibVersion == lastSeenRibVersion — DFP check boundary.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* After peer3 goes DOWN, bring it back UP. The reconnected peer
 * reuses the same bit position. Verify no stale detachment state from
 * the previous session leaks into the new session. The reconnected peer
 * enters DETACHED_INIT_DUMP (existing group), but the old detachment
 * tracking is cleaned up — isPeerDetached returns false. */
TEST_P(UpdateGroupMultiPeerTest, BitmapBitReuseAfterPeerDown) {
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* Large queue for fast peer */
  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr4);
  /* Small queue for slow peer */
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

  /* Inject a shared route — both peers receive it */
  injectLocalRoutesAtRuntime({"32.1.0.0/16"}, {"3201:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3201:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3201:1"));

  /* Freq-detach peer3 — small queue fills naturally, no blockPeer needed */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(60000),
      1,
      std::chrono::milliseconds(60000));

  injectLocalRoutesAtRuntime({"32.2.0.0/16"}, {"3202:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3202:1"));
  injectLocalRoutesAtRuntime({"32.3.0.0/16"}, {"3203:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3203:1"));
  injectLocalRoutesAtRuntime({"32.4.0.0/16"}, {"3204:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3204:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Bring peer3 DOWN — clears detachment tracking */
  bringDownPeer(kPeerAddr3);

  /* Bring peer3 back UP — reuses same bit position.
   * After reconnect, peer may be in DETACHED_INIT_DUMP (init dump pending),
   * DETACHED_RUNNING (draining init dump PL), DETACHED_READY_TO_JOIN
   * (init dump complete, awaiting group acceptance), or JOINED_RUNNING
   * (already accepted back) depending on timing. All are valid.
   * Use larger queue for reconnection so init dump fits. */
  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  ASSERT_TRUE(waitForPeerStateAny(
      kPeerAddr3,
      {PeerUpdateState::JOINED_RUNNING,
       PeerUpdateState::DETACHED_RUNNING,
       PeerUpdateState::DETACHED_INIT_DUMP,
       PeerUpdateState::DETACHED_READY_TO_JOIN}));

  /* peer4 continues working normally after peer3 reconnect */
  injectLocalRoutesAtRuntime({"32.5.0.0/16"}, {"3205:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.5.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.5.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3205:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  verifySlowPeerInvariants(kPeerAddr4);
}

/* syncBitmap is always a subset of establishedBitmap. After setup,
 * both peers are established AND in sync. After detachment, the detached
 * peer is still established but NOT in sync. Verify isPeerInSync is false
 * while the peer is still a group member (getGroupMemberCount unchanged). */
TEST_P(UpdateGroupMultiPeerTest, SyncBitmapSubsetOfEstablished) {
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

  /* Both established AND in sync */
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_EQ(getGroupMemberCount(kPeerAddr3), 2);

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(60000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"32.10.0.0/16"}, {"3210:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3210:1"));
  injectLocalRoutesAtRuntime({"32.11.0.0/16"}, {"3211:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.11.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.11.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3211:1"));
  injectLocalRoutesAtRuntime({"32.12.0.0/16"}, {"3212:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.12.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.12.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3212:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* After detachment: peer3 is NOT in sync but still a group member */
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_EQ(getGroupMemberCount(kPeerAddr3), 2);

  /* peer4 remains established AND in sync */
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Group still delivers to peer4 */
  injectLocalRoutesAtRuntime({"32.13.0.0/16"}, {"3213:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.13.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3213:1"));
  verifySlowPeerInvariants(kPeerAddr4);
}

/* When peer3 goes DOWN, all bitmaps are cleared for that peer.
 * No stale bits remain. isPeerInSync, isPeerDetached both return false
 * for a DOWN peer. Group member count decreases. */
TEST_P(UpdateGroupMultiPeerTest, AllBitmapsClearedOnPeerDown) {
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

  /* Confirm both peers in sync before DOWN */
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_EQ(getGroupMemberCount(kPeerAddr3), 2);

  /* Inject a route so there's group activity */
  injectLocalRoutesAtRuntime({"32.20.0.0/16"}, {"3220:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.20.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3220:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3220:1"));

  /* Bring peer3 DOWN */
  bringDownPeer(kPeerAddr3);

  /* All bitmaps cleared — no stale state */
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* peer4 still operational, member count reduced */
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Verify group continues normally with just peer4 */
  injectLocalRoutesAtRuntime({"32.21.0.0/16"}, {"3221:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3221:1"));
  verifySlowPeerInvariants(kPeerAddr4);
}

/* Detach peer before any CL entries are consumed (divergenceRibVersion
 * = 0). Setup peers, reach JOINED_RUNNING, then immediately detach without
 * injecting any routes first. CL consumer starts at version 0. Post-detach
 * routes are tracked in CL from the very beginning. */
TEST_P(UpdateGroupMultiPeerTest, DivergenceRibVersionZero) {
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

  /* NO routes injected — divergenceRibVersion will be 0 */

  /* Freq-detach peer3 immediately */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(60000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"32.30.0.0/16"}, {"3230:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3230:1"));
  injectLocalRoutesAtRuntime({"32.31.0.0/16"}, {"3231:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3231:1"));
  injectLocalRoutesAtRuntime({"32.32.0.0/16"}, {"3232:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.32.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.32.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3232:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* CL at version 0 — all post-detach routes tracked from beginning */
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);

  /* Inject post-detach routes — CL accumulates from version 0 */
  injectLocalRoutesAtRuntime({"32.33.0.0/16"}, {"3233:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.33.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.33.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3233:1"));

  injectLocalRoutesAtRuntime({"32.34.0.0/16"}, {"3234:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.34.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.34.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3234:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  verifySlowPeerInvariants(kPeerAddr4);
}

/* divergenceRibVersion == lastSeenRibVersion — DFP check boundary.
 * Inject routes so both peers consume them (advancing lastSeenRibVersion),
 * then detach immediately (divergenceRibVersion == lastSeenRibVersion).
 * Post-detach route has ribVersion > divergenceRibVersion, so Case 3
 * applies (no clone needed). Verify CL tracking and delivery work. */
TEST_P(UpdateGroupMultiPeerTest, DivergenceEqualsLastSeenVersion) {
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

  /* Inject 2 routes — both peers consume, advancing lastSeenRibVersion */
  injectLocalRoutesAtRuntime({"32.40.0.0/16"}, {"3240:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.40.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3240:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3240:1"));

  injectLocalRoutesAtRuntime({"32.41.0.0/16"}, {"3241:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.41.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3241:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3241:1"));

  /* Detach peer3 immediately — divergenceRibVersion == lastSeenRibVersion */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(60000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"32.42.0.0/16"}, {"3242:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.42.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.42.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3242:1"));
  injectLocalRoutesAtRuntime({"32.43.0.0/16"}, {"3243:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.43.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.43.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3243:1"));
  injectLocalRoutesAtRuntime({"32.44.0.0/16"}, {"3244:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.44.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.44.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3244:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);

  /* Post-detach: new route has ribVersion > divergenceRibVersion (Case 3).
   * No lazy clone needed — CL tracks it as a new entry. */
  injectLocalRoutesAtRuntime({"32.45.0.0/16"}, {"3245:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.45.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.45.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3245:1"));

  /* Update a pre-detach route (32.40) — ribVersion <= divergenceRibVersion,
   * so Case 4 triggers a lazy clone for peer3's diverged view */
  withdrawLocalRoutesAtRuntime({"32.40.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "32.40.0.0", 16, kPeerAddr4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
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
