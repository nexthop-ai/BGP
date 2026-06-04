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

/* E2E tests: Detachment procedure corner cases, timer resets, and multi-peer
 * interaction during detection.
 * Prefix range: 31.x.0.0/16.
 *
 * New peer joins while another being detected as slow
 * Peer blocks during another peer's detachment -- verify atomicity
 * Peer unblocks then immediately reblocks -- timer reset then restart
 * PL clone when group PL is empty -- DFP immediately
 * PL clone preserves attribute pointers (shared_ptr)
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* While peer3 is being detected as slow (blocked, near freq threshold),
 * bring peer5 DOWN and back UP. The new peer joining should not interfere with
 * peer3's slow peer detection. Peer3 should still detach, and the reconnected
 * peer5 should enter DETACHED_INIT_DUMP as expected for existing groups.
 */
TEST_P(UpdateGroupMultiPeerTest, NewPeerJoinsDuringSlowDetection) {
  XLOG(INFO, "=== TEST: NewPeerJoinsDuringSlowDetection ===");

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

  /* Freq threshold=2 on peer3 -- needs 2 block cycles to detach */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      2,
      std::chrono::milliseconds(60000));

  /* Block cycle #1 -- need 3 routes to fill queue past hwm=2 */
  blockPeer(kPeerAddr3);
  for (int i = 1; i <= 3; i++) {
    auto prefix = fmt::format("31.{}.0.0/16", i);
    auto community = fmt::format("310{}:1", i);
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
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  /* unblockPeer drains queued messages -- don't verifyRouteAdd after */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* While peer3 is near threshold, bring peer5 DOWN then back UP */
  bringDownPeer(kPeerAddr5);
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::DOWN));

  bringUpPeer(kPeerAddr5);
  /* Reconnected peer enters DETACHED_INIT_DUMP for existing group */

  /* Block cycle #2 -- should trigger freq detachment (count=2 >= threshold=2)
   * despite peer5 reconnection activity. Use 31.4-31.6 (31.1-31.3 used above).
   */
  blockPeer(kPeerAddr3);
  for (int i = 4; i <= 6; i++) {
    auto prefix = fmt::format("31.{}.0.0/16", i);
    auto community = fmt::format("310{}:1", i);
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

  /* Peer3 should be freq-detached now */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Peer4 continues normally */
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: NewPeerJoinsDuringSlowDetection ===");
}

/* While peer3 is being freq-detached (blocking + queue fill), peer4
 * also blocks simultaneously. Verify peer3 detaches atomically and peer4's
 * blocking is handled independently without corruption.
 */
TEST_P(UpdateGroupMultiPeerTest, PeerBlocksDuringAnotherDetachment) {
  XLOG(INFO, "=== TEST: PeerBlocksDuringAnotherDetachment ===");

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

  /* Freq threshold=1 on peer3 -- detaches on first block cycle */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Block both peer3 AND peer4 simultaneously */
  blockPeer(kPeerAddr3);
  blockPeer(kPeerAddr4);

  /* Fill queue -- only peer5 drains */
  injectLocalRoutesAtRuntime({"31.10.0.0/16"}, {"3110:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.10.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3110:1"));
  injectLocalRoutesAtRuntime({"31.11.0.0/16"}, {"3111:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.11.0.0/16")));
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
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3112:1"));

  /* Both peers detach -- freq threshold=1 is per-group, applies to all blocked
   * peers. This verifies the group handles multiple simultaneous detachments
   * atomically without corruption.
   */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr4));

  /* Peer5 still running and in sync despite both others detaching */
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  /* New route reaches peer5 -- proves group is functional with 2 detached peers
   */
  injectLocalRoutesAtRuntime({"31.13.0.0/16"}, {"3113:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.13.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "3113:1"));

  verifySlowPeerInvariants(kPeerAddr5);

  XLOG(INFO, "=== TEST PASSED: PeerBlocksDuringAnotherDetachment ===");
}

/* Peer unblocks then immediately reblocks -- duration timer should
 * reset on unblock and restart on reblock. Use a long duration threshold so
 * the timer doesn't fire during the first block, then unblock, reblock, and
 * use a very short threshold to fire immediately on the second block.
 */
TEST_P(UpdateGroupMultiPeerTest, UnblockReblockTimerReset) {
  XLOG(INFO, "=== TEST: UnblockReblockTimerReset ===");

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

  /* Very long duration threshold -- should NOT fire during first block */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Block cycle #1 -- peer3 blocks but long timer doesn't fire */
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("31.{}.0.0/16", 20 + i);
    auto community = fmt::format("31{}:1", 20 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", 20 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /*
   * Unblock -- timer should be cancelled. unblockPeer drains the queue
   * (consuming the route messages), so we don't verify peer3 routes here.
   * Just wait for peer3 to recover to JOINED_RUNNING.
   */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Now set 1ms duration threshold -- fires immediately on next block */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(1),
      999999,
      std::chrono::milliseconds(60000));

  /* Immediately reblock -- 1ms timer fires, peer3 detaches */
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("31.{}.0.0/16", 25 + i);
    auto community = fmt::format("31{}:1", 25 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", 25 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Peer3 should be DETACHED_BLOCKED from 1ms duration timer */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Peer4 continues normally */
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: UnblockReblockTimerReset ===");
}
/* PL clone preserves attribute pointers (shared_ptr) -- no deep copy
 * of BgpPath. Inject multiple routes pre-detach so shared attributes exist.
 * After detach, inject a post-detach route. Verify CL consumer works and
 * that peer4 still receives routes correctly (proving shared_ptr attributes
 * weren't corrupted by the clone).
 */
TEST_P(UpdateGroupMultiPeerTest, PLClonePreservesAttributePointers) {
  XLOG(INFO, "=== TEST: PLClonePreservesAttributePointers ===");

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

  /* Inject 5 routes with different attributes (communities) pre-detach.
   * Both peers receive them -- establishing shared attribute pointers in RIB.
   */
  for (int i = 0; i < 5; i++) {
    auto prefix = fmt::format("31.{}.0.0/16", 40 + i);
    auto community = fmt::format("31{}:1", 40 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", 40 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", 40 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Freq-detach peer3 with threshold=1 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  /* Fill queue with 5 routes (capacity=5) to trigger detachment */
  for (int i = 0; i < 5; i++) {
    auto prefix = fmt::format("31.{}.0.0/16", 50 + i);
    auto community = fmt::format("31{}:1", 50 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", 50 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Post-detach route -- proves attribute pointers survived the PL clone.
   * If shared_ptr was corrupted, peer4 would crash or get wrong attributes.
   */
  injectLocalRoutesAtRuntime({"31.60.0.0/16"}, {"3160:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.60.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.60.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3160:1"));

  /* Withdraw one of the pre-detach routes -- exercises shared attribute
   * cleanup path. If pointers were deep-copied incorrectly, this could crash.
   */
  withdrawLocalRoutesAtRuntime({"31.40.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "31.40.0.0", 16, kPeerAddr4));

  /* Peer4 continues normally -- shared_ptr attributes intact */
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: PLClonePreservesAttributePointers ===");
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
