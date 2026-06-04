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

/* E2E tests: Bitmap mask arithmetic edge cases. Prefix: 31.x/16.
 * Bitmap bits set then cleared on peer down — verify mask arithmetic
 * divergedPeerBitmap with multiple bits — iteration correctness
 * blockedBitmap cleared on detachment — peer no longer blocked
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* In a 3-peer group, detach peer3 and peer4 (2 diverged bits set).
 * Then bring peer3 DOWN — its detached consumer is unregistered, clearing
 * one bit. Verify getDetachedPeerCount decreases from 2 to 1. Then bring
 * peer4 DOWN — count should go to 0. This exercises the mask clear logic. */
TEST_P(UpdateGroupMultiPeerTest, BitmapClearedOnPeerDown) {
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

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"31.1.0.0/16"}, {"3101:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3101:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.1.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3101:1"));
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
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.2.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
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
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.3.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3103:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr5), 1);

  /* Now freq-detach peer4 */
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  unblockPeer(kPeerAddr3);
  blockPeer(kPeerAddr4);
  injectLocalRoutesAtRuntime({"31.4.0.0/16"}, {"3104:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.4.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3104:1"));
  injectLocalRoutesAtRuntime({"31.5.0.0/16"}, {"3105:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.5.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.5.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3105:1"));
  injectLocalRoutesAtRuntime({"31.6.0.0/16"}, {"3106:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.6.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.6.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3106:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* 2 bits set in divergedPeerBitmap */
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr5), 2);

  /* Bring peer3 DOWN — clears one bit */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr5), 1);

  /* Bring peer4 DOWN — clears last bit */
  unblockPeer(kPeerAddr4);
  bringDownPeer(kPeerAddr4);
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DOWN));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr5), 0);

  /* peer5 continues normally */
  injectLocalRoutesAtRuntime({"31.7.0.0/16"}, {"3107:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.7.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.7.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3107:1"));
  verifySlowPeerInvariants(kPeerAddr5);
}

/* 3-peer group, detach both peer3 and peer4. Verify
 * divergedPeerBitmap has 2 bits set. Inject post-detach routes and verify
 * only peer5 (in-sync) receives them. Iteration over multiple bits. */
TEST_P(UpdateGroupMultiPeerTest, DivergedBitmapMultipleBitsIteration) {
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

  /* Inject shared route before detachments */
  injectLocalRoutesAtRuntime({"31.10.0.0/16"}, {"3110:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.10.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3110:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3110:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.10.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3110:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
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
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.11.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
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
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.12.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3112:1"));
  injectLocalRoutesAtRuntime({"31.13.0.0/16"}, {"3113:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.13.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3113:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.13.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3113:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr5), 1);

  /* Freq-detach peer4 */
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  unblockPeer(kPeerAddr3);
  blockPeer(kPeerAddr4);
  injectLocalRoutesAtRuntime({"31.14.0.0/16"}, {"3114:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.14.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.14.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3114:1"));
  injectLocalRoutesAtRuntime({"31.15.0.0/16"}, {"3115:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.15.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.15.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3115:1"));
  injectLocalRoutesAtRuntime({"31.16.0.0/16"}, {"3116:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.16.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.16.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3116:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* 2 bits set — both peer3 and peer4 diverged */
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr5), 2);
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerDetached(kPeerAddr4));
  EXPECT_FALSE(isPeerDetached(kPeerAddr5));

  /* Post-detach routes only go to peer5 */
  for (int i = 0; i < 3; ++i) {
    auto prefix = fmt::format("31.{}.0.0/16", 20 + i);
    auto community = fmt::format("{}:1", 3120 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", 20 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));
  }
  verifySlowPeerInvariants(kPeerAddr5);
}

/* After freq-detach, the blockedBitmap is cleared for the detached
 * peer. The group immediately resumes for remaining in-sync peers without
 * waiting for the (now-detached) blocked peer. */
TEST_P(UpdateGroupMultiPeerTest, BlockedBitmapClearedOnDetachment) {
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

  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"31.30.0.0/16"}, {"3130:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3130:1"));
  injectLocalRoutesAtRuntime({"31.31.0.0/16"}, {"3131:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3131:1"));
  injectLocalRoutesAtRuntime({"31.32.0.0/16"}, {"3132:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.32.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.32.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3132:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* After detachment, blocked bitmap cleared — group resumes immediately */
  injectLocalRoutesAtRuntime({"31.33.0.0/16"}, {"3133:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.33.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.33.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3133:1"));

  injectLocalRoutesAtRuntime({"31.34.0.0/16"}, {"3134:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.34.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.34.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3134:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);
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
