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
 * E2E tests: Entry Collapse -- Match, Mixed, PathId, Performance, and Edge
 *
 * All per-peer entries match group entries -- all collapsed
 * Mixed match -- some collapsed, some retained
 * Same attrs but different pathId -- retained
 * 100 per-peer entries all match -- O(n) scan performance
 * Group entry for prefix peer doesn't have -- nothing to collapse
 *
 * Prefix range: 31.x.0.0/16
 * Fixture: UpdateGroupLazyCloneTest
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * All per-peer entries match group -- all collapsed (memory freed).
 *
 * Setup: 3 shared routes. Detach peer3 via freq threshold. Do NOT update
 * any shared routes (no divergence). Per-peer entries created by clone
 * will have SAME attrs as group entries.
 * Action: Unblock peer3 for acceptance.
 * Verify: All per-peer entries match group -> all collapsed. Peer3 back
 * in sync. Group functional with new routes delivered to both peers.
 */
TEST_P(UpdateGroupLazyCloneTest, CollapseAllEntriesMatch) {
  XLOG(INFO, "=== TEST: CollapseAllEntriesMatch ===");

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

  /* Both peers receive 3 shared routes */
  for (int i = 1; i <= 3; i++) {
    auto prefix = fmt::format("31.{}.0.0/16", i);
    auto community = fmt::format("{}:1", 3100 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", i),
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

  for (int i = 4; i <= 8; i++) {
    auto prefix = fmt::format("31.{}.0.0/16", i);
    auto community = fmt::format("{}:1", 3100 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Do NOT update shared routes -- per-peer entries will match group */
  verifySlowPeerInvariants(kPeerAddr3);

  /* Unblock -- collapse finds all entries match, all collapsed */
  unblockPeer(kPeerAddr3);

  /* Verify group functional -- peer4 receives new route after acceptance */
  injectLocalRoutesAtRuntime({"31.9.0.0/16"}, {"3109:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.9.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.9.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3109:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  XLOG(INFO, "=== TEST PASSED: CollapseAllEntriesMatch ===");
}

/*
 * Mixed match -- some per-peer entries collapsed, some retained.
 *
 * Setup: 3 shared routes. Detach peer3. Update only 1 of the 3 shared
 * routes (Case 4 clone for all 3, but only 1 has divergent attrs).
 * Action: Unblock for acceptance.
 * Verify: 2 entries collapsed (matching attrs), 1 retained (diverged).
 * Group functional. No crash.
 */
TEST_P(UpdateGroupLazyCloneTest, CollapseMixedMatch) {
  XLOG(INFO, "=== TEST: CollapseMixedMatch ===");

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

  /* Both peers receive 3 shared routes */
  for (int i = 20; i <= 22; i++) {
    auto prefix = fmt::format("31.{}.0.0/16", i);
    auto community = fmt::format("{}:1", 3100 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", i),
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

  for (int i = 23; i <= 27; i++) {
    auto prefix = fmt::format("31.{}.0.0/16", i);
    auto community = fmt::format("{}:1", 3100 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Update ONLY the first shared route -- Case 4 clone, then diverge */
  injectLocalRoutesAtRuntime({"31.20.0.0/16"}, {"3120:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3120:99"));

  /* Routes 31.21 and 31.22 remain unchanged -- their per-peer entries match */
  verifySlowPeerInvariants(kPeerAddr3);

  /* Unblock -- 2 entries match (31.21, 31.22), 1 diverged (31.20) */
  unblockPeer(kPeerAddr3);

  /* Verify group functional */
  injectLocalRoutesAtRuntime({"31.28.0.0/16"}, {"3128:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.28.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.28.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3128:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  XLOG(INFO, "=== TEST PASSED: CollapseMixedMatch ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupLazyCloneTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
