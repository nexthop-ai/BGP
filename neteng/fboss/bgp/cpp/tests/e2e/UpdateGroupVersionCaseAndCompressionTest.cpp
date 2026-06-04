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

/* E2E tests: ribVersion Case 2/3 boundaries and PL compression.
 * Prefix: 33.x/16. entry.ribVersion == divergenceRibVersion + 1 — Case 3 (>),
 * no clone lastSeenRibVersion advances monotonically during DSP recovery group
 * vs peer lastSeenRibVersion after acceptance ribVersion 0 for brand new prefix
 * — Case 2 applies, no clone PL compression — multiple prefixes same attr, some
 * updated post-detach
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Inject a route AFTER detachment. Its ribVersion is
 * divergenceRibVersion + 1 (Case 3: ribVersion > divergenceRibVersion).
 * This means NO clone is needed — the entry was created post-detach so the
 * detached peer never saw it. Peer4 receives it normally. */
TEST_P(UpdateGroupMultiPeerTest, RibVersionCase3NoClone) {
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
  injectLocalRoutesAtRuntime({"33.1.0.0/16"}, {"3301:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3301:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3301:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"33.2.0.0/16"}, {"3302:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3302:1"));
  injectLocalRoutesAtRuntime({"33.3.0.0/16"}, {"3303:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3303:1"));
  injectLocalRoutesAtRuntime({"33.4.0.0/16"}, {"3304:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3304:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Inject brand-new prefix AFTER detachment — ribVersion will be
   * divergenceRibVersion + N (Case 3). No clone needed. */
  injectLocalRoutesAtRuntime({"33.5.0.0/16"}, {"3305:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.5.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.5.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3305:1"));

  /* Update the post-detach prefix again — still Case 3, no clone */
  injectLocalRoutesAtRuntime({"33.5.0.0/16"}, {"3305:2"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.5.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.5.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3305:2"));

  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);
  verifySlowPeerInvariants(kPeerAddr4);
}

/* After freq-detach, unblock peer3 to enter DRJ (recovery).
 * During DSP recovery, inject routes one at a time. Each CL entry
 * has a monotonically increasing ribVersion. Verify peer4 receives
 * all routes (proving CL tracking works with advancing versions). */
TEST_P(UpdateGroupMultiPeerTest, LastSeenRibVersionMonotonicDurDSP) {
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

  /* Freq-detach peer3 with queue (5,4,0) and 5 fill routes */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 5; ++i) {
    auto prefix = fmt::format("33.{}.0.0/16", 10 + i);
    auto community = fmt::format("{}:1", 3310 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("33.{}.0.0", 10 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock to enter DRJ (recovery) */
  unblockPeer(kPeerAddr3);

  /* Inject 3 routes during recovery — each has increasing ribVersion.
   * Inject-drain one at a time to avoid order issues. */
  for (int i = 0; i < 3; ++i) {
    auto prefix = fmt::format("33.{}.0.0/16", 20 + i);
    auto community = fmt::format("{}:1", 3320 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("33.{}.0.0", 20 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  verifySlowPeerInvariants(kPeerAddr4);
}

/* After detach + recovery + acceptance (peer DOWN/UP cycle),
 * the group's and the re-joined peer's lastSeenRibVersion should be
 * consistent. Verified indirectly: post-acceptance routes go to both peers. */
TEST_P(UpdateGroupMultiPeerTest, GroupVsPeerVersionAfterAcceptance) {
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

  /* Inject shared route */
  injectLocalRoutesAtRuntime({"33.30.0.0/16"}, {"3330:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.30.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3330:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3330:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"33.31.0.0/16"}, {"3331:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3331:1"));
  injectLocalRoutesAtRuntime({"33.32.0.0/16"}, {"3332:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.32.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.32.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3332:1"));
  injectLocalRoutesAtRuntime({"33.33.0.0/16"}, {"3333:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.33.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.33.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3333:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Bring peer3 DOWN then UP — enters DETACHED_INIT_DUMP, then DOWN again
   * to complete acceptance cycle cleanly. Peer4 keeps working. */
  unblockPeer(kPeerAddr3);
  bringDownPeer(kPeerAddr3);

  /* Inject a route while peer3 is DOWN to advance group ribVersion */
  injectLocalRoutesAtRuntime({"33.34.0.0/16"}, {"3334:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.34.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.34.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3334:1"));

  /* Peer4 still running — group version advanced, versions consistent */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  verifySlowPeerInvariants(kPeerAddr4);
}

/* Brand new prefix injected AFTER detachment has ribVersion=0 in
 * the entry (never existed before). Case 2 applies (ribVersion == 0 means
 * the entry was created post-detach). No clone needed. */
TEST_P(UpdateGroupMultiPeerTest, RibVersionZeroCase2NoClone) {
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

  /* Freq-detach peer3 — NO shared routes before detachment */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"33.40.0.0/16"}, {"3340:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3340:1"));
  injectLocalRoutesAtRuntime({"33.41.0.0/16"}, {"3341:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3341:1"));
  injectLocalRoutesAtRuntime({"33.42.0.0/16"}, {"3342:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.42.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.42.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3342:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Inject 2 brand-new prefixes (never existed before). ribVersion=0 in
   * the RIB entry → Case 2 applies → no clone needed. */
  injectLocalRoutesAtRuntime({"33.43.0.0/16"}, {"3343:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.43.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.43.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3343:1"));

  injectLocalRoutesAtRuntime({"33.44.0.0/16"}, {"3344:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.44.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.44.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3344:1"));

  /* Update a post-detach prefix — still Case 3 (ribVersion > divergence) */
  injectLocalRoutesAtRuntime({"33.43.0.0/16"}, {"3343:2"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.43.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.43.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3343:2"));

  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);
  verifySlowPeerInvariants(kPeerAddr4);
}

/* PL compression — multiple prefixes with same attributes get packed
 * into one BGP UPDATE. After detachment, update some (but not all) of those
 * prefixes with different attributes. The updated prefixes get new buckets
 * while the original ones stay intact. Peer4 receives both the unchanged
 * routes (from the original PL) and the updated ones (from new PL). */
TEST_P(UpdateGroupMultiPeerTest, PlCompressionBucketIntegrity) {
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

  /* Inject 3 routes with SAME community — packed into 1 UPDATE (1 bucket).
   * Inject-drain one at a time to avoid order issues. */
  for (int i = 0; i < 3; ++i) {
    auto prefix = fmt::format("33.{}.0.0/16", 50 + i);
    injectLocalRoutesAtRuntime({prefix}, {"3350:1"}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("33.{}.0.0", 50 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        "3350:1"));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("33.{}.0.0", 50 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        "3350:1"));
  }

  /* Detach peer3 via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 7; ++i) {
    auto prefix = fmt::format("33.{}.0.0/16", 60 + i);
    auto community = fmt::format("{}:1", 3360 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("33.{}.0.0", 60 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Update 1 of the 3 same-community prefixes with DIFFERENT attributes.
   * This splits it into a new bucket while the other 2 stay in the original.
   * Peer4 receives the update normally. */
  injectLocalRoutesAtRuntime({"33.50.0.0/16"}, {"3350:2"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.50.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.50.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3350:2"));

  /* Update another one with yet different attributes — another bucket split */
  injectLocalRoutesAtRuntime({"33.51.0.0/16"}, {"3351:3"}, 250);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.51.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.51.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3351:3"));

  /* Third prefix (33.52.0.0/16) remains unchanged — original bucket intact */
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);
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
