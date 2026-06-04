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
 * E2E tests: Advanced route refresh scenarios
 *
 * Prefix range: 31.x.0.0/16
 *
 * Tests:
 *   Route refresh followed by policy change — policy takes precedence
 *   Route refresh during G-IDLE — standard detach for RR
 *   Route refresh for only peer in group — single-peer detach for RR
 *   Two route refresh requests in rapid succession — idempotent
 *   Route refresh produces full RIB-OUT re-announcement
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Route refresh for only peer in group -- single-peer detach for RR
 * Single peer group. Simulate route refresh with burst of routes.
 * The lone peer receives all routes. No other peer to compare against.
 * Verifies single-peer group handles refresh correctly without crash.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, SinglePeer_RouteRefresh) {
  XLOGF(INFO, "=== TEST: SinglePeer_RouteRefresh ===");

  addPeer(kDefaultPeerSpec3);
  addLocalRoute("31.20.0.0/16", {"3120:1"}, 100);
  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);

  /*
   * Single-peer group with pre-loaded route: init dump produces 1 route
   * announcement + 1 EoR. Only 1 EoR is sent (not 2) because the init
   * dump path sends a single consolidated EoR after the route batch.
   * Drain route first, then EoR, then confirm state.
   */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.20.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3120:1"));
  EXPECT_TRUE(waitForEoR(peerId3));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_EQ(getGroupMemberCount(kPeerAddr3), 1);

  /* Simulate route refresh: burst of 3 routes with different communities */
  for (int i = 21; i <= 23; ++i) {
    auto prefix = fmt::format("31.{}.0.0/16", i);
    auto community = fmt::format("31{:02d}:1", i);
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
  }

  /* Single peer still JOINED_RUNNING after refresh */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_EQ(getGroupMemberCount(kPeerAddr3), 1);
  verifySlowPeerInvariants(kPeerAddr3);

  XLOGF(INFO, "=== TEST PASSED: SinglePeer_RouteRefresh ===");
}

/*
 * Route refresh produces full RIB-OUT re-announcement
 * Inject several routes, drain to both peers. Then simulate a refresh
 * by withdrawing all and re-injecting them with different communities.
 * Both peers receive all withdrawals and all re-announcements,
 * verifying a complete RIB-OUT rebuild.
 */
TEST_P(UpdateGroupMultiPeerTest, FullRibOutReAnnouncement) {
  XLOGF(INFO, "=== TEST: FullRibOutReAnnouncement ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Phase 1: Inject 3 routes and drain to both peers */
  for (int i = 40; i <= 42; ++i) {
    auto prefix = fmt::format("31.{}.0.0/16", i);
    auto community = fmt::format("31{:02d}:1", i);
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

  /* Phase 2: Withdraw all 3 routes and drain withdrawals */
  for (int i = 40; i <= 42; ++i) {
    auto prefix = fmt::format("31.{}.0.0/16", i);
    withdrawLocalRoutesAtRuntime({prefix});
    EXPECT_TRUE(
        verifyRouteWithdraw("v4", fmt::format("31.{}.0.0", i), 16, kPeerAddr3));
    EXPECT_TRUE(
        verifyRouteWithdraw("v4", fmt::format("31.{}.0.0", i), 16, kPeerAddr4));
  }

  /* Phase 3: Re-announce with different communities (new attributes) */
  for (int i = 40; i <= 42; ++i) {
    auto prefix = fmt::format("31.{}.0.0/16", i);
    auto community = fmt::format("31{:02d}:2", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 200);
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

  /* Both peers received full withdraw + re-announcement cycle */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOGF(INFO, "=== TEST PASSED: FullRibOutReAnnouncement ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupSlowPeerDetectionTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupMultiPeerTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
