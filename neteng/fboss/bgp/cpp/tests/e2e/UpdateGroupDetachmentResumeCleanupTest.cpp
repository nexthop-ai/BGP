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

/* E2E tests: Detachment resume, cleanup, and edge cases. Prefix: 30.x/16. */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Detach peer, verify group immediately resumes if it was the only
 * blocked peer. After detachment the blocked bitmap is cleared for that peer,
 * so the group should transition out of WAITING and deliver to the remaining
 * in-sync peer immediately.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachOnlyBlockedPeer_GroupResumes) {
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* Fast peer4: large queue that won't block */
  setupSlowPeerComponents(10, 8, 0);
  bringUpPeer(kPeerAddr4);

  /* Slow peer3: small queue that fills naturally */
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

  /* Freq threshold=1 so first block cycle triggers detachment */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Inject 3 routes — peer3's small queue fills naturally past hwm */
  injectLocalRoutesAtRuntime({"30.1.0.0/16"}, {"3001:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3001:1"));
  injectLocalRoutesAtRuntime({"30.2.0.0/16"}, {"3002:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3002:1"));
  injectLocalRoutesAtRuntime({"30.3.0.0/16"}, {"3003:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3003:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /*
   * peer3 was the ONLY blocked peer. After detachment the group should
   * resume immediately -- inject a new route and verify peer4 gets it
   * without delay.
   */
  injectLocalRoutesAtRuntime({"30.4.0.0/16"}, {"3004:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3004:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);
  verifySlowPeerInvariants(kPeerAddr4);
}

/*
 * Detach peer, then same peer goes DOWN before any recovery.
 * Verifies full cleanup -- the detached consumer is removed, peer enters
 * DOWN state, and peer4 continues normally.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachThenPeerDown_FullCleanup) {
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* Fast peer4: large queue that won't block */
  setupSlowPeerComponents(10, 8, 0);
  bringUpPeer(kPeerAddr4);

  /* Slow peer3: small queue that fills naturally */
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

  /* Freq threshold=1 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Inject 3 routes — peer3's small queue fills naturally */
  injectLocalRoutesAtRuntime({"30.20.0.0/16"}, {"3020:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3020:1"));
  injectLocalRoutesAtRuntime({"30.21.0.0/16"}, {"3021:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3021:1"));
  injectLocalRoutesAtRuntime({"30.22.0.0/16"}, {"3022:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.22.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.22.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3022:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Bring detached peer3 DOWN */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Verify full cleanup -- peer4 still works normally */
  injectLocalRoutesAtRuntime({"30.23.0.0/16"}, {"3023:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.23.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.23.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3023:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);
}

/*
 * Detachment with attrToPrefixMap containing entries with 0 prefixes.
 * Inject a route, then withdraw it BEFORE detaching. The RIB entry has an
 * empty prefix set for that attribute bucket. Verify detachment handles
 * the empty bucket gracefully -- no crash, peer4 continues normally.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachWithEmptyAttrBucket) {
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* Fast peer4: large queue that won't block */
  setupSlowPeerComponents(10, 8, 0);
  bringUpPeer(kPeerAddr4);

  /* Slow peer3: small queue for detachment, drain during initial phase */
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

  /* Inject a route then withdraw it -- creates an empty attr bucket */
  injectLocalRoutesAtRuntime({"30.30.0.0/16"}, {"3030:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.30.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3030:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3030:1"));

  withdrawLocalRoutesAtRuntime({"30.30.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "30.30.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "30.30.0.0", 16, kPeerAddr4));

  /* Inject a second route that stays (shared state for PL clone) */
  injectLocalRoutesAtRuntime({"30.31.0.0/16"}, {"3031:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.31.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3031:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3031:1"));

  /* Now detach peer3 via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Inject 3 fill routes — peer3's small queue fills naturally */
  for (int i = 0; i < 3; ++i) {
    auto prefix = fmt::format("30.{}.0.0/16", 32 + i);
    auto community = fmt::format("{}:1", 3032 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("30.{}.0.0", 32 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Verify no crash from empty attr bucket during PL clone */
  injectLocalRoutesAtRuntime({"30.40.0.0/16"}, {"3040:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3040:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
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
