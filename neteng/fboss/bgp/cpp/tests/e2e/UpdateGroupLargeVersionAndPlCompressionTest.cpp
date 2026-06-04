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

/* E2E tests: large ribVersion and PL compression. Prefix: 32.30-32.74/16.
 * Very large ribVersion (near uint64_t max / 2) — no overflow
 * PL compression — prefix moves attribute bucket, group vs detached
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Inject many routes (add+withdraw cycles) to push ribVersion high,
 * then detach and verify CL works with large versions. Uses a larger queue
 * (8,6,2) to accommodate many pre-detach operations without accidental
 * blocking. */
TEST_P(UpdateGroupMultiPeerTest, VeryLargeRibVersionNoOverflow) {
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

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

  /* Drive ribVersion high with 8 add+withdraw cycles (16 version bumps) */
  for (int i = 0; i < 8; ++i) {
    auto prefix = fmt::format("32.{}.0.0/16", 30 + i);
    auto community = fmt::format("{}:1", 3230 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("32.{}.0.0", 30 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("32.{}.0.0", 30 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  for (int i = 0; i < 8; ++i) {
    auto prefix = fmt::format("32.{}.0.0/16", 30 + i);
    withdrawLocalRoutesAtRuntime({prefix});
    EXPECT_TRUE(verifyRouteWithdraw(
        "v4", fmt::format("32.{}.0.0", 30 + i), 16, kPeerAddr3));
    EXPECT_TRUE(verifyRouteWithdraw(
        "v4", fmt::format("32.{}.0.0", 30 + i), 16, kPeerAddr4));
  }

  /* Re-inject 3 more to advance further (19 total version bumps) */
  for (int i = 0; i < 3; ++i) {
    auto prefix = fmt::format("32.{}.0.0/16", 40 + i);
    auto community = fmt::format("{}:1", 3240 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("32.{}.0.0", 40 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("32.{}.0.0", 40 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Detach peer3 at high ribVersion */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 7; ++i) {
    auto prefix = fmt::format("32.{}.0.0/16", 50 + i);
    auto community = fmt::format("{}:1", 3250 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("32.{}.0.0", 50 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Post-detach at high version — CL still tracks correctly */
  injectLocalRoutesAtRuntime({"32.60.0.0/16"}, {"3260:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.60.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.60.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3260:1"));

  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);
  verifySlowPeerInvariants(kPeerAddr4);
}

/* PL compression — prefix moves from one attribute bucket to another.
 * Inject a route with community A, detach peer3, then update the same
 * prefix with community B. The CL records the attribute change. Peer4
 * receives the update with new attributes. The detached peer's stale
 * view preserves community A (via Case 4 clone). */
TEST_P(UpdateGroupMultiPeerTest, PlCompressionPrefixMovesAttrBucket) {
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

  /* Inject route with community A — both peers get it */
  injectLocalRoutesAtRuntime({"32.70.0.0/16"}, {"3270:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.70.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.70.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3270:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.70.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3270:1"));

  /* Inject a second route with community B — different attr bucket */
  injectLocalRoutesAtRuntime({"32.71.0.0/16"}, {"3271:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.71.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.71.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3271:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.71.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3271:1"));

  /* Detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"32.72.0.0/16"}, {"3272:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.72.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.72.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3272:1"));
  injectLocalRoutesAtRuntime({"32.73.0.0/16"}, {"3273:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.73.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.73.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3273:1"));
  injectLocalRoutesAtRuntime({"32.74.0.0/16"}, {"3274:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.74.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.74.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3274:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Move prefix 32.70.0.0/16 from attr bucket A (3270:1) to bucket B
   * (3270:2). This changes attributes — CL records Case 4 clone + update.
   * Peer4 sees the new community. Detached peer3 retains stale view. */
  injectLocalRoutesAtRuntime({"32.70.0.0/16"}, {"3270:2"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.70.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.70.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3270:2"));

  /* Also move 32.71.0.0/16 to same bucket as 32.70.0.0/16 (same community
   * 3270:2). This tests bucket merging — two prefixes now share attr. */
  injectLocalRoutesAtRuntime({"32.71.0.0/16"}, {"3270:2"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("32.71.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "32.71.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3270:2"));

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
