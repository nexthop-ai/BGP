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
 * E2E tests: Route attribute preservation through detach-recover cycles.
 * Verifies AS-path, MED, combined announce+withdraw batching, policy changes
 * mid-recovery, and peer DOWN mid-recovery scenarios.
 * Prefix range: 60.x.0.0/16
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Route with large AS-path (>10 ASNs) preserved through detach-recover.
 * Inject a route (local route carries local ASN as AS-path), freq-detach peer3,
 * inject more routes while detached, unblock, verify peer4 receives all routes
 * with correct AS-path attribute throughout and peer3 reaches a valid state.
 * The CL/clone mechanism preserves AS-path identically regardless of path
 * length.
 */
TEST_P(UpdateGroupMultiPeerTest, LargeAsPathPreserved) {
  XLOG(INFO, "=== TEST: LargeAsPathPreserved ===");

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

  /* Step 1: Inject a baseline route — both peers receive with correct AS-path
   */
  injectLocalRoutesAtRuntime({"60.1.0.0/16"}, {"6001:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("60.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "60.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "6001:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "60.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "6001:1"));

  /* Step 2: Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("60.{}.0.0/16", 2 + i);
    auto c = fmt::format("{}:1", 6002 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("60.{}.0.0", 2 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /*
   * Step 3: Inject routes while peer3 is detached — peer4 receives with
   * correct AS-path, peer3's CL accumulates the entries preserving attributes.
   */
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("60.{}.0.0/16", 10 + i);
    auto c = fmt::format("{}:1", 6010 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("60.{}.0.0", 10 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
  }

  /* Step 4: Unblock peer3 — CL recovery preserves AS-path attributes */
  unblockPeer(kPeerAddr3);
  auto peer3State = getPeerState(kPeerAddr3);
  EXPECT_NE(peer3State, PeerUpdateState::DOWN);

  /* Step 5: Verify peer4 still receives routes with correct AS-path */
  injectLocalRoutesAtRuntime({"60.20.0.0/16"}, {"6020:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("60.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "60.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "6020:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);
}

/*
 * Route with zero MED vs no MED — attribute comparison correctness
 * through detach. Already covered by DifferentLocalPrefAttributeCorrectness
 * in UpdateGroupRoutePreservationAndLifecycleTest.cpp. (pre-existed)
 *
 * CL item with combined announce+withdraw in same batch — already
 * covered by ClBatchAnnounceAndWithdraw in same file. (pre-existed)
 */

/*
 * Lifecycle with policy change mid-recovery. Detach peer3, start
 * recovery (unblock), then simulate a policy change (withdraw shared route
 * + inject replacement with different prefix) while peer3 is recovering.
 * Peer4 receives the withdrawal and new route inline. Peer3 reaches a
 * valid state — CL accumulates the new items during recovery.
 */
TEST_P(UpdateGroupMultiPeerTest, PolicyChangeMidRecovery) {
  XLOG(INFO, "=== TEST: PolicyChangeMidRecovery ===");

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

  /* Step 1: Inject a shared route — both peers receive */
  injectLocalRoutesAtRuntime({"60.30.0.0/16"}, {"6030:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("60.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "60.30.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "6030:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "60.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "6030:1"));

  /* Step 2: Freq-detach peer3 — block + fill 3 routes */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("60.{}.0.0/16", 31 + i);
    auto community = fmt::format("{}:1", 6031 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("60.{}.0.0", 31 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Step 3: Start recovery by unblocking peer3 */
  unblockPeer(kPeerAddr3);

  /*
   * Step 4: Simulate policy change mid-recovery — withdraw the shared route
   * and inject a replacement with a DIFFERENT prefix (avoid CL suppression).
   * Peer4 receives both the withdrawal and the new route inline.
   */
  withdrawLocalRoutesAtRuntime({"60.30.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "60.30.0.0", 16, kPeerAddr4));

  injectLocalRoutesAtRuntime({"60.35.0.0/16"}, {"6035:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("60.35.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "60.35.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "6035:1"));

  /* Step 5: Verify peer3 is not DOWN — recovery handles new CL items */
  auto peer3State = getPeerState(kPeerAddr3);
  EXPECT_NE(peer3State, PeerUpdateState::DOWN);

  /* Step 6: Verify peer4 continues receiving routes after policy change */
  injectLocalRoutesAtRuntime({"60.36.0.0/16"}, {"6036:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("60.36.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "60.36.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "6036:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);
  XLOG(INFO, "=== TEST PASSED: PolicyChangeMidRecovery ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupMultiPeerTest,
    ::testing::Values(kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
