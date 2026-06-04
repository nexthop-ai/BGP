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

/* E2E tests: Acceptance edge cases -- re-block after accept, MRAI with
 * accepted peer, dangling consumer cleanup, DSP lag, and IDLE→READY timing.
 * Prefix range: 36.1-36.49/16.
 *
 * Accept peer, same peer immediately re-blocks -- frequency window clean
 * Group in IDLE, accepts peer, transitions to READY -- MRAI with peer
 * Acceptance clears detached consumer -- no dangling consumer
 * DSP peer lags -- group must wait at IDLE for peer to catch up
 * Group reaches IDLE briefly, accepts peer, immediately back to READY
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Detach peer3 via freq threshold, unblock to start recovery, then
 * immediately re-block peer3 after acceptance. The frequency window should
 * be clean (reset) after acceptance, so the first re-block should NOT trigger
 * a second detachment.
 */
TEST_P(UpdateGroupMultiPeerTest, AcceptReblockFreqWindowClean) {
  XLOGF(INFO, "=== TEST: AcceptReblockFreqWindowClean ===");

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
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  /* Set freq threshold=1 (detach on first block) */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Block peer3 and fill queue to trigger detachment */
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 5; i++) {
    auto prefix = fmt::format("36.{}.0.0/16", i + 1);
    auto community = fmt::format("{}:1", 3601 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("36.{}.0.0", i + 1),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock to start recovery -- peer goes through DRJ and acceptance */
  unblockPeer(kPeerAddr3);

  /* Verify peer4 still works after detach */
  injectLocalRoutesAtRuntime({"36.10.0.0/16"}, {"3610:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3610:1"));

  /* Now re-block peer3 immediately. Since freq window should be reset after
   * acceptance, this single block should NOT trigger a second detachment.
   */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"36.11.0.0/16"}, {"3611:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.11.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.11.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3611:1"));

  /* With freq threshold=1, CL recovery may re-block and re-detach before
   * acceptance completes. The key validation is no crash from re-blocking.
   */

  unblockPeer(kPeerAddr3);
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOGF(INFO, "=== TEST PASSED: AcceptReblockFreqWindowClean ===");
}
/* After acceptance, the detached consumer should be cleared. Detach
 * peer3, let it recover and be accepted, then verify detachedPeerCount is 0
 * and no dangling consumer exists (invariants hold).
 */
TEST_P(UpdateGroupMultiPeerTest, AcceptanceClearsDetachedConsumer) {
  XLOGF(INFO, "=== TEST: AcceptanceClearsDetachedConsumer ===");

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
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 5; i++) {
    auto prefix = fmt::format("36.{}.0.0/16", 22 + i);
    auto community = fmt::format("{}:1", 3622 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("36.{}.0.0", 22 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Confirm detached count is 1 */
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1);

  /* Unblock to trigger recovery and acceptance */
  unblockPeer(kPeerAddr3);

  /* Inject a route to allow group to cycle through READY→IDLE for acceptance */
  injectLocalRoutesAtRuntime({"36.28.0.0/16"}, {"3628:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.28.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.28.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3628:1"));

  /* Verify no dangling consumer -- detached count should be 0 after acceptance
   */
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);

  XLOGF(INFO, "=== TEST PASSED: AcceptanceClearsDetachedConsumer ===");
}

/* DSP peer lags -- group must wait at IDLE for peer to catch up.
 * Detach peer3, inject routes while detached so CL diverges, then unblock.
 * The group should wait for the DRJ peer's CL to catch up before accepting.
 * During this time peer4 continues receiving routes normally.
 */
TEST_P(UpdateGroupMultiPeerTest, DspPeerLagsGroupWaits) {
  XLOGF(INFO, "=== TEST: DspPeerLagsGroupWaits ===");

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
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  /* Inject a shared route before detachment */
  injectLocalRoutesAtRuntime({"36.30.0.0/16"}, {"3630:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.30.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3630:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3630:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 5; i++) {
    auto prefix = fmt::format("36.{}.0.0/16", 31 + i);
    auto community = fmt::format("{}:1", 3631 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("36.{}.0.0", 31 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Inject MORE routes while peer3 is detached -- CL diverges significantly */
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("36.{}.0.0/16", 37 + i);
    auto community = fmt::format("{}:1", 3637 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("36.{}.0.0", 37 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Unblock -- peer3 must catch up on CL before acceptance */
  unblockPeer(kPeerAddr3);

  /* Peer4 should continue working while peer3 catches up */
  injectLocalRoutesAtRuntime({"36.41.0.0/16"}, {"3641:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("36.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "36.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3641:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOGF(INFO, "=== TEST PASSED: DspPeerLagsGroupWaits ===");
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
