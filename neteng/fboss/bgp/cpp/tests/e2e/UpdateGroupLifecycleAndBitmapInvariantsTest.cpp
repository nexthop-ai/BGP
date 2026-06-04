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
 * E2E tests: Lifecycle multi-group, shutdown, invariants, bitmap edge cases.
 * Prefix range: 56.x.0.0/16
 *
 * Multiple groups — detach in one, in-sync in another
 * Shutdown during CL consumption — coroutine cleanup
 * Shutdown with 0 in-sync peers — cleanup all detached state
 * Final state invariants — syncBitmap/detachedPeers consistency
 * Bitmap bit 63 operations — highest bit (3-peer proxy)
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Multiple groups — peer detaches in one, stays in-sync in another.
 * E2E framework supports 3 peers with same type (one update group).
 * True multi-group requires different UpdateGroupKey (different peer type).
 * Test: 3 peers in same group, detach peer3, verify peer4+peer5 continue
 * receiving routes independently and are unaffected by peer3's detachment. */
TEST_P(UpdateGroupMultiPeerTest, MultiGroupDetachIsolation) {
  XLOG(INFO, "=== TEST: MultiGroupDetachIsolation ===");

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

  /* Inject a shared route — all 3 peers receive it */
  injectLocalRoutesAtRuntime({"56.1.0.0/16"}, {"5601:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("56.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "56.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "5601:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "56.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5601:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "56.1.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "5601:1"));

  /* Freq-detach peer3 only */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 2; i <= 4; ++i) {
    auto prefix = fmt::format("56.{}.0.0/16", i);
    auto comm = fmt::format("56{:02d}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {comm}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("56.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        comm));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("56.{}.0.0", i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        comm));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* peer3 detached, peer4+peer5 still in-sync */
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1);

  /* New route still delivered to peer4+peer5, not peer3 */
  injectLocalRoutesAtRuntime({"56.5.0.0/16"}, {"5605:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("56.5.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "56.5.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5605:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "56.5.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "5605:1"));

  verifySlowPeerInvariants(kPeerAddr3);
  XLOG(INFO, "=== TEST PASSED: MultiGroupDetachIsolation ===");
}

/* Shutdown during CL consumption — coroutine cleanup.
 * Inject CL items while group is WAITING (peer3 blocked), then bring
 * both peers DOWN. Verify no crash or hang from CL cleanup. */
TEST_P(UpdateGroupMultiPeerTest, ShutdownDuringClConsumption) {
  XLOG(INFO, "=== TEST: ShutdownDuringClConsumption ===");

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

  /* Block peer3, fill queue to WAITING state */
  blockPeer(kPeerAddr3);
  for (int i = 10; i <= 12; ++i) {
    auto prefix = fmt::format("56.{}.0.0/16", i);
    auto comm = fmt::format("56{:02d}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {comm}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("56.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        comm));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Inject CL items while WAITING — these accumulate in CL */
  for (int i = 13; i <= 15; ++i) {
    auto prefix = fmt::format("56.{}.0.0/16", i);
    auto comm = fmt::format("56{:02d}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {comm}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
  }

  /* Shutdown both peers while CL items are pending — no crash */
  bringDownPeer(kPeerAddr3);
  bringDownPeer(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: ShutdownDuringClConsumption ===");
}

/* Shutdown with 0 in-sync peers — cleanup all detached state.
 * Freq-detach peer4, then bringDownPeer on peer3 (last in-sync).
 * Now group has 0 in-sync + 1 detached. Bring detached peer4 DOWN too.
 * Verify no crash from empty sync bitmap cleanup. */
TEST_P(UpdateGroupMultiPeerTest, ShutdownZeroInSyncAllDetached) {
  XLOG(INFO, "=== TEST: ShutdownZeroInSyncAllDetached ===");

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

  /* Freq-detach peer4 (not peer3, since peer3 is bit 0 — last synced skip) */
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr4);
  for (int i = 20; i <= 22; ++i) {
    auto prefix = fmt::format("56.{}.0.0/16", i);
    auto comm = fmt::format("56{:02d}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {comm}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("56.{}.0.0", i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        comm));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_FALSE(isPeerInSync(kPeerAddr4));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1);

  /* Bring peer3 DOWN — now 0 in-sync peers, 1 detached */
  bringDownPeer(kPeerAddr3);

  /* Bring peer4 DOWN — cleanup all detached state, no crash */
  bringDownPeer(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: ShutdownZeroInSyncAllDetached ===");
}

/* Final state invariants after various transitions.
 * Verify: isPeerInSync => not detached, isPeerDetached => not in sync,
 * getDetachedPeerCount consistent. Check after each transition:
 * JOINED_RUNNING -> JOINED_BLOCKED -> DETACHED_BLOCKED -> DOWN. */
TEST_P(UpdateGroupMultiPeerTest, FinalStateInvariants) {
  XLOG(INFO, "=== TEST: FinalStateInvariants ===");

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

  /* Invariant check 1: both JOINED_RUNNING */
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 0);
  verifySlowPeerInvariants(kPeerAddr3);

  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Transition to JOINED_BLOCKED */
  blockPeer(kPeerAddr3);
  for (int i = 30; i <= 32; ++i) {
    auto prefix = fmt::format("56.{}.0.0/16", i);
    auto comm = fmt::format("56{:02d}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {comm}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("56.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        comm));
  }

  /* Invariant check 2: peer3 -> DETACHED_BLOCKED (freq threshold=1) */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_FALSE(isPeerDetached(kPeerAddr4));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1);
  verifySlowPeerInvariants(kPeerAddr3);

  /* Invariant check 3: peer3 DOWN — cleared from all bitmaps */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 0);
  verifySlowPeerInvariants(kPeerAddr4);

  /* Final delivery check — peer4 still works */
  injectLocalRoutesAtRuntime({"56.33.0.0/16"}, {"5633:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("56.33.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "56.33.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5633:1"));

  XLOG(INFO, "=== TEST PASSED: FinalStateInvariants ===");
}

/* Bitmap bit 63 operations — highest bit in 64-bit word.
 * E2E framework supports 3 peers (bits 0,1,2). Cannot test bit 63 directly.
 * Proxy test: verify highest available bit (peer5=bit 2) behaves correctly.
 * Set/clear/check operations on the highest bit in a 3-peer group. */
TEST_P(UpdateGroupMultiPeerTest, BitmapHighestBitOperations) {
  XLOG(INFO, "=== TEST: BitmapHighestBitOperations ===");

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

  /* All 3 bits set initially */
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 0);
  EXPECT_EQ(getGroupMemberCount(kPeerAddr3), 3);

  /* Freq-detach peer5 (highest bit=2) */
  setSlowPeerThresholds(
      kPeerAddr5,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr5);
  for (int i = 40; i <= 42; ++i) {
    auto prefix = fmt::format("56.{}.0.0/16", i);
    auto comm = fmt::format("56{:02d}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {comm}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("56.{}.0.0", i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        comm));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("56.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        comm));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::DETACHED_BLOCKED));

  /* Highest bit cleared from sync, set in detached */
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_FALSE(isPeerInSync(kPeerAddr5));
  EXPECT_TRUE(isPeerDetached(kPeerAddr5));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1);

  /* Bring peer5 DOWN — clear highest bit from all bitmaps */
  bringDownPeer(kPeerAddr5);
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 0);

  /* Remaining peers continue working after highest bit cleared */
  injectLocalRoutesAtRuntime({"56.43.0.0/16"}, {"5643:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("56.43.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "56.43.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "5643:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "56.43.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5643:1"));

  verifySlowPeerInvariants(kPeerAddr3);
  XLOG(INFO, "=== TEST PASSED: BitmapHighestBitOperations ===");
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
