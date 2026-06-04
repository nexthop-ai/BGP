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
 * E2E tests: Multi-peer state combination with route events
 * Prefix range: 54.x.0.0/16
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * (P-JR + P-DB) x E-PEER-DOWN(JR)
 * 2-peer group: peer4 freq-detached to DETACHED_BLOCKED, peer3 is the last
 * in-sync (JR) peer. Bring peer3 DOWN -- only the detached peer4 remains.
 * Verify no crash, inject route (goes to CL since no in-sync peers), and
 * confirm peer4 stays in detached state.
 */
TEST_P(UpdateGroupMultiPeerTest, JrPlusDb_PeerDownOnJr) {
  XLOG(INFO, "=== TEST: JrPlusDb_PeerDownOnJr ===");

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

  /* Inject a shared route so both peers have it */
  injectLocalRoutesAtRuntime({"54.30.0.0/16"}, {"5430:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("54.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "54.30.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "5430:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "54.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5430:1"));

  /* Freq-detach peer4: threshold=1, one block cycle -> DETACHED_BLOCKED */
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr4);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("54.{}.0.0/16", 31 + i);
    auto c = fmt::format("{}:1", 5431 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    /* Drain peer3 (in-sync) after each inject */
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("54.{}.0.0", 31 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Now peer3=JR (last in-sync), peer4=DB */
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerDetached(kPeerAddr4));

  /* EVENT: Bring the last in-sync peer (peer3) DOWN */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Only peer4 (DB) remains -- no in-sync peers in group */
  EXPECT_TRUE(isPeerDetached(kPeerAddr4));

  /* Inject a route -- no in-sync peer to receive via PL, goes to CL */
  injectLocalRoutesAtRuntime({"54.40.0.0/16"}, {"5440:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("54.40.0.0/16")));

  /* Verify no crash, peer4 still detached */
  EXPECT_TRUE(isPeerDetached(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  /* Inject another route for stability confirmation */
  injectLocalRoutesAtRuntime({"54.41.0.0/16"}, {"5441:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("54.41.0.0/16")));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== PASSED: JrPlusDb_PeerDownOnJr ===");
}

/*
 * (P-JB + P-DB) x E-UNBLOCK(JB)
 * 3-peer group: peer4 freq-detached -> DB. Then raise thresholds
 * (thresholds are GROUP-wide) so peer3 stays JB when blocked.
 * Unblock peer3, verify peer5 continues, peer4 still detached.
 */
TEST_P(UpdateGroupMultiPeerTest, JbPlusDb_UnblockJb) {
  XLOG(INFO, "=== TEST: JbPlusDb_UnblockJb ===");

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

  /* Step 1: Freq-detach peer4 -> DETACHED_BLOCKED (freq=1, dur=600s) */
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr4);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("54.{}.0.0/16", 70 + i);
    auto c = fmt::format("{}:1", 5470 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("54.{}.0.0", 70 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        c));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("54.{}.0.0", 70 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /*
   * Step 2: Raise thresholds (GROUP-wide) so peer3 stays JB.
   * freq=999999 and dur=600s prevent any further detachments.
   */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("54.{}.0.0/16", 73 + i);
    auto c = fmt::format("{}:1", 5473 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("54.{}.0.0", 73 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  EXPECT_TRUE(isPeerDetached(kPeerAddr4));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  /* Step 3: EVENT -- unblock peer3 */
  unblockPeer(kPeerAddr3);

  /* Step 4: Inject route -- verify via peer5 (guaranteed in-sync) */
  injectLocalRoutesAtRuntime({"54.80.0.0/16"}, {"5480:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("54.80.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "54.80.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "5480:1"));

  /* Verify peer3 not DOWN, peer4 still detached */
  auto peer3State = getPeerState(kPeerAddr3);
  EXPECT_TRUE(
      peer3State == PeerUpdateState::JOINED_RUNNING ||
      peer3State == PeerUpdateState::JOINED_BLOCKED);
  EXPECT_TRUE(isPeerDetached(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== PASSED: JbPlusDb_UnblockJb ===");
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
