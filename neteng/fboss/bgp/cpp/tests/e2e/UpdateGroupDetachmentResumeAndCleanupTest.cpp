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
 * E2E tests: Detachment resume, cleanup, and edge cases.
 *
 * Prefix range: 80.70-80.110/16
 *
 * Tests:
 *   Group resumes immediately after only blocked peer detached
 *   Detach during IDLE state -- group stays IDLE
 *   Detach then peer DOWN before recovery -- cleanup
 *   Detach with empty attrToPrefixMap entries (withdraw before detach)
 *   Detach with serialization -- UpdateDescriptor in PL clone
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Detach peer, verify group immediately resumes if it was the
 * only blocked peer. After freq-detach, group should no longer be stalled
 * on the blocked peer and delivers to peer4 immediately.
 */
TEST_P(UpdateGroupMultiPeerTest, GroupResumesAfterOnlyBlockedPeerDetached) {
  XLOG(INFO, "=== TEST: GroupResumesAfterOnlyBlockedPeerDetached ===");

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

  /* Freq-detach peer3 (the only blocked peer) */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("80.{}.0.0/16", 70 + i);
    auto community = fmt::format("80{}:1", 70 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("80.{}.0.0", 70 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Group should resume immediately -- peer4 gets new route without stall */
  injectLocalRoutesAtRuntime({"80.75.0.0/16"}, {"8075:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("80.75.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "80.75.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "8075:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_FALSE(isPeerQueueBlocked(peerId4));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOG(INFO, "=== TEST PASSED: GroupResumesAfterOnlyBlockedPeerDetached ===");
}

/*
 * Detach peer while group is in IDLE state -- group stays IDLE.
 * After detachment, group continues normal operation for remaining peers.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachDuringIdleGroupStaysIdle) {
  XLOG(INFO, "=== TEST: DetachDuringIdleGroupStaysIdle ===");

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

  /* Group is IDLE (no routes injected yet). Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("80.{}.0.0/16", 78 + i);
    auto community = fmt::format("80{}:1", 78 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("80.{}.0.0", 78 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Group should be operational -- inject route, peer4 receives it */
  injectLocalRoutesAtRuntime({"80.83.0.0/16"}, {"8083:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("80.83.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "80.83.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "8083:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  XLOG(INFO, "=== TEST PASSED: DetachDuringIdleGroupStaysIdle ===");
}

/*
 * Detach peer, then same peer goes DOWN before any recovery.
 * Verify no crash, cleanup is correct, peer4 continues normally.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachThenPeerDownBeforeRecovery) {
  XLOG(INFO, "=== TEST: DetachThenPeerDownBeforeRecovery ===");

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
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("80.{}.0.0/16", 85 + i);
    auto community = fmt::format("80{}:1", 85 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("80.{}.0.0", 85 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Peer goes DOWN before any recovery attempt */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Verify no crash -- peer4 continues receiving routes */
  injectLocalRoutesAtRuntime({"80.90.0.0/16"}, {"8090:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("80.90.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "80.90.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "8090:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: DetachThenPeerDownBeforeRecovery ===");
}

/*
 * Detachment with attrToPrefixMap containing entries with 0 prefixes.
 * Inject a route, withdraw it (creating an empty attr entry), then detach.
 * PL clone should handle empty entries gracefully.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachWithEmptyAttrPrefixEntries) {
  XLOG(INFO, "=== TEST: DetachWithEmptyAttrPrefixEntries ===");

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

  /* Inject a route then withdraw it -- may leave empty attr entry in map */
  injectLocalRoutesAtRuntime({"80.92.0.0/16"}, {"8092:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("80.92.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "80.92.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "8092:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "80.92.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "8092:1"));

  withdrawLocalRoutesAtRuntime({"80.92.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "80.92.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "80.92.0.0", 16, kPeerAddr4));

  /* Now freq-detach peer3 with empty PL state */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("80.{}.0.0/16", 94 + i);
    auto community = fmt::format("80{}:1", 94 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("80.{}.0.0", 94 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Post-detach: verify peer4 continues normally */
  injectLocalRoutesAtRuntime({"80.99.0.0/16"}, {"8099:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("80.99.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "80.99.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "8099:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOG(INFO, "=== TEST PASSED: DetachWithEmptyAttrPrefixEntries ===");
}

/*
 * Detachment with serialization enabled.
 * Parameterized test runs in both modes. Verifies UpdateDescriptor in PL
 * clone works correctly by injecting routes before and after detachment.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachWithSerializationVerification) {
  XLOG(INFO, "=== TEST: DetachWithSerializationVerification ===");

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

  /* Inject pre-detach routes (creates PL entries with UpdateDescriptor) */
  for (int i = 0; i < 2; i++) {
    auto prefix = fmt::format("80.{}.0.0/16", 101 + i);
    auto community = fmt::format("{}:1", 10100 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("80.{}.0.0", 101 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("80.{}.0.0", 101 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("80.{}.0.0/16", 105 + i);
    auto community = fmt::format("{}:1", 10500 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("80.{}.0.0", 105 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Post-detach: PL clone should preserve UpdateDescriptor correctly */
  injectLocalRoutesAtRuntime({"80.110.0.0/16"}, {"11000:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("80.110.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "80.110.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "11000:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1);
  verifySlowPeerInvariants(kPeerAddr3);

  XLOG(INFO, "=== TEST PASSED: DetachWithSerializationVerification ===");
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
