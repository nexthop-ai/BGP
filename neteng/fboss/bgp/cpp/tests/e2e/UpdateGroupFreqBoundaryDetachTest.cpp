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

/* E2E tests: Frequency detection boundary conditions.
 * Prefix range: 30.30-30.45/16.
 *
 * Exactly N blocks in W seconds (boundary) — detach triggers
 * N blocks but window expired — no detach (window eviction)
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Exactly N blocks in W seconds (boundary) — detach triggers.
 * Set freq threshold to 2 blocks in 60s. Do exactly 2 block cycles.
 * Peer3 should detach on the 2nd cycle (boundary condition). */
TEST_P(UpdateGroupMultiPeerTest, FreqBoundary_ExactlyN_Detaches) {
  XLOG(INFO, "=== TEST: FreqBoundary_ExactlyN_Detaches ===");

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

  /* Freq threshold: 2 blocks in 60s window triggers detachment */
  setSlowPeerThresholds(
      kPeerAddr3, std::chrono::seconds(600), 2, std::chrono::seconds(60));

  /* Block cycle #1: block, fill, wait blocked, unblock, drain */
  blockPeer(kPeerAddr3);

  for (int i = 30; i <= 32; i++) {
    auto prefix = fmt::format("30.{}.0.0/16", i);
    auto community = fmt::format("30{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("30.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Still JOINED_BLOCKED after 1 cycle */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* Unblock peer3 — unblockPeer auto-drains the queue */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* Block cycle #2 — should trigger detachment at boundary (exactly N=2) */
  blockPeer(kPeerAddr3);

  for (int i = 33; i <= 35; i++) {
    auto prefix = fmt::format("30.{}.0.0/16", i);
    auto community = fmt::format("30{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("30.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* 2nd block cycle — freq detachment triggers */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Peer4 continues */
  injectLocalRoutesAtRuntime({"30.36.0.0/16"}, {"3036:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.36.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.36.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3036:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);
  XLOG(INFO, "=== TEST PASSED ===");
}

/* N blocks but window expired between N-1th and Nth — no detach.
 * Set freq threshold to 2 blocks in a 1ms window. Do cycle #1, unblock,
 * then cycle #2. The 1ms window expires between cycles so the count resets
 * and peer stays JOINED_BLOCKED (not detached). */
TEST_P(UpdateGroupMultiPeerTest, FreqBoundary_WindowExpired_NoDetach) {
  XLOG(INFO, "=== TEST: FreqBoundary_WindowExpired_NoDetach ===");

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

  /* Freq threshold: 2 blocks in 1ms window.
   * By the time we finish cycle #1 and start cycle #2,
   * the 1ms window has expired and cycle #1 is evicted. */
  setSlowPeerThresholds(
      kPeerAddr3, std::chrono::seconds(600), 2, std::chrono::milliseconds(1));

  /* Block cycle #1 */
  blockPeer(kPeerAddr3);

  for (int i = 40; i <= 42; i++) {
    auto prefix = fmt::format("30.{}.0.0/16", i);
    auto community = fmt::format("30{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("30.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Unblock peer3 — unblockPeer auto-drains the queue */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  /* The 1ms window has long expired by now. Block cycle #2 should NOT
   * trigger detachment — cycle #1's timestamp was evicted. */
  blockPeer(kPeerAddr3);

  for (int i = 43; i <= 45; i++) {
    auto prefix = fmt::format("30.{}.0.0/16", i);
    auto community = fmt::format("30{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("30.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Peer3 should be JOINED_BLOCKED, NOT detached (window expired) */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* Unblock peer3 — unblockPeer auto-drains the queue */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));

  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);
  XLOG(INFO, "=== TEST PASSED ===");
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
