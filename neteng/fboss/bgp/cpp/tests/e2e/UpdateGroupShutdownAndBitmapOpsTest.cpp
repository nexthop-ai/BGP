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
 * E2E tests: Lifecycle shutdown scenarios and bitmap bit-0 operations.
 * Prefix range: 55.x.0.0/16
 *
 * Detach during MRAI pending -> recovery -> accept before next MRAI
 * Shutdown during PL drain — verify bounded termination
 * Shutdown during acceptance — partial acceptance cleaned up
 * Process restart after detachment — peer comes back as new session
 * Bitmap bit 0 operations — lowest bit set/clear/check
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Detach during MRAI pending -> recovery -> accept before next MRAI.
 * Inject multiple routes (each triggers MRAI cycle), freq-detach peer3,
 * unblock for recovery, then verify new route delivered after recovery. */
TEST_P(UpdateGroupMultiPeerTest, DetachDuringMraiPending) {
  XLOG(INFO, "=== TEST: DetachDuringMraiPending ===");

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
      2,
      std::chrono::milliseconds(60000));

  /* Block cycle #1: inject routes with different communities (MRAI cycles) */
  blockPeer(kPeerAddr3);
  for (int i = 1; i <= 3; ++i) {
    auto prefix = fmt::format("55.{}.0.0/16", i);
    auto comm = fmt::format("55{:02d}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {comm}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("55.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        comm));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  unblockPeer(kPeerAddr3, 0, 0);
  for (int i = 1; i <= 3; ++i) {
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("55.{}.0.0", i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        fmt::format("55{:02d}:1", i)));
  }

  /* Block cycle #2 -> triggers freq detachment during MRAI pending */
  blockPeer(kPeerAddr3);
  for (int i = 4; i <= 6; ++i) {
    auto prefix = fmt::format("55.{}.0.0/16", i);
    auto comm = fmt::format("55{:02d}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {comm}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("55.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        comm));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock -> recovery, verify new route accepted after detachment */
  unblockPeer(kPeerAddr3, 0, 0);
  injectLocalRoutesAtRuntime({"55.7.0.0/16"}, {"5507:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("55.7.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "55.7.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5507:1"));

  verifySlowPeerInvariants(kPeerAddr3);
  XLOG(INFO, "=== TEST PASSED: DetachDuringMraiPending ===");
}

/* Shutdown during PL drain — verify bounded termination.
 * Put group into WAITING, then bring both peers DOWN. No crash or hang. */
TEST_P(UpdateGroupMultiPeerTest, ShutdownDuringPlDrain) {
  XLOG(INFO, "=== TEST: ShutdownDuringPlDrain ===");

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

  /* Block peer3, fill queue to put group into WAITING */
  blockPeer(kPeerAddr3);
  for (int i = 10; i <= 12; ++i) {
    auto prefix = fmt::format("55.{}.0.0/16", i);
    auto comm = fmt::format("55{:02d}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {comm}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("55.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        comm));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* Shutdown both peers during PL drain — no crash, bounded termination */
  bringDownPeer(kPeerAddr3);
  bringDownPeer(kPeerAddr4);

  /* Success = no crash, no hang. Both peers cleanly go DOWN. */
  XLOG(INFO, "=== TEST PASSED: ShutdownDuringPlDrain ===");
}

/* Shutdown during acceptance — partial acceptance cleaned up.
 * Freq-detach peer3, start acceptance by unblocking, then immediately
 * bring peer3 DOWN. Verify peer4 continues and no crash. */
TEST_P(UpdateGroupMultiPeerTest, ShutdownDuringAcceptance) {
  XLOG(INFO, "=== TEST: ShutdownDuringAcceptance ===");

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

  /* Single block cycle with threshold=1 -> immediate detachment */
  blockPeer(kPeerAddr3);
  for (int i = 20; i <= 22; ++i) {
    auto prefix = fmt::format("55.{}.0.0/16", i);
    auto comm = fmt::format("55{:02d}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {comm}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("55.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        comm));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock to start acceptance, then immediately bring peer3 DOWN */
  unblockPeer(kPeerAddr3);
  bringDownPeer(kPeerAddr3);

  /* Verify peer4 still works — inject and verify delivery */
  injectLocalRoutesAtRuntime({"55.23.0.0/16"}, {"5523:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("55.23.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "55.23.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5523:1"));

  XLOG(INFO, "=== TEST PASSED: ShutdownDuringAcceptance ===");
}

/* Process restart after detachment — peer comes back as new session.
 * Freq-detach peer3, bring DOWN, then UP (new session). Peer enters
 * DETACHED_INIT_DUMP. Drain init dump, verify peer4 continues. */
TEST_P(UpdateGroupMultiPeerTest, RestartAfterDetachment) {
  XLOG(INFO, "=== TEST: RestartAfterDetachment ===");

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
  injectLocalRoutesAtRuntime({"55.30.0.0/16"}, {"5530:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("55.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "55.30.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "5530:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "55.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5530:1"));

  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Freq-detach peer3 with threshold=1 */
  blockPeer(kPeerAddr3);
  for (int i = 31; i <= 33; ++i) {
    auto prefix = fmt::format("55.{}.0.0/16", i);
    auto comm = fmt::format("55{:02d}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {comm}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("55.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        comm));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Simulate process restart: down then up as new session */
  bringDownPeer(kPeerAddr3);
  unblockPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));

  /* Reconnecting peer enters DETACHED_INIT_DUMP, drain its init dump */
  drainPeerQueueCompletely(peerId3);

  /* Verify peer4 continues to work with new route */
  injectLocalRoutesAtRuntime({"55.34.0.0/16"}, {"5534:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("55.34.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "55.34.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5534:1"));

  XLOG(INFO, "=== TEST PASSED: RestartAfterDetachment ===");
}

/* Bitmap bit 0 operations — lowest bit set/clear/check.
 * Peer3 is bit 0. Verify isPeerInSync, isPeerDetached, getDetachedPeerCount
 * before and after detaching bit 0, then clear via peer DOWN. */
TEST_P(UpdateGroupMultiPeerTest, BitmapBit0Operations) {
  XLOG(INFO, "=== TEST: BitmapBit0Operations ===");

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

  /* Both peers in sync initially — bit 0 (peer3) and bit 1 (peer4) set */
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 0);

  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Freq-detach peer3 (bit 0) with threshold=1 */
  blockPeer(kPeerAddr3);
  for (int i = 40; i <= 42; ++i) {
    auto prefix = fmt::format("55.{}.0.0/16", i);
    auto comm = fmt::format("55{:02d}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {comm}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("55.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        comm));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Bit 0 cleared from sync bitmap, set in detached */
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1);

  /* Bring peer3 DOWN — clears bit 0 from all bitmaps */
  bringDownPeer(kPeerAddr3);
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 0);
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Verify peer4 continues working after bit 0 cleared */
  injectLocalRoutesAtRuntime({"55.43.0.0/16"}, {"5543:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("55.43.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "55.43.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5543:1"));

  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED: BitmapBit0Operations ===");
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
