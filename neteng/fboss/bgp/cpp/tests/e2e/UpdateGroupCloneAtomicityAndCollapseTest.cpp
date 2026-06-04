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
 * E2E tests: Clone Atomicity and Empty Collapse
 *
 * CloneAtomicityDuringAcceptance: Clone fires between acceptance steps
 * CollapseEmptyPerPeerEntries: Empty per-peer entry set -- nothing to collapse
 * (DFP)
 *
 * Prefix range: 13.80-13.99/16
 * Fixture: UpdateGroupLazyCloneTest
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Clone fires between acceptance steps (bitmap set, diverged clear).
 *
 * Setup: Detach peer3 via freq threshold. Inject a post-detach route.
 * Action: Unblock peer3 to start acceptance. Immediately inject an update
 * to the shared prefix (exercises clone atomicity during acceptance).
 * Verify: Peer4 receives all updates. Group functional. No crash.
 */
TEST_P(UpdateGroupLazyCloneTest, CloneAtomicityDuringAcceptance) {
  XLOG(INFO, "=== TEST: CloneAtomicityDuringAcceptance ===");

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

  /* Both peers receive a shared route */
  injectLocalRoutesAtRuntime({"13.80.0.0/16"}, {"1380:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.80.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.80.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1380:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.80.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1380:1"));

  /* Freq-detach peer3: threshold=1 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);

  for (int i = 1; i <= 5; i++) {
    auto prefix = fmt::format("13.{}.0.0/16", 80 + i);
    auto community = fmt::format("{}:1", 1380 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("13.{}.0.0", 80 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock to start acceptance */
  unblockPeer(kPeerAddr3);

  /* Immediately inject update to shared prefix -- clone atomicity test */
  injectLocalRoutesAtRuntime({"13.80.0.0/16"}, {"1380:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.80.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.80.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1380:99"));

  /* Verify group functional -- no crash */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  injectLocalRoutesAtRuntime({"13.86.0.0/16"}, {"1386:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.86.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.86.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1386:1"));

  XLOG(INFO, "=== TEST PASSED: CloneAtomicityDuringAcceptance ===");
}

/*
 * Collapse -- empty per-peer entry set (DFP path).
 *
 * Setup: Detach peer3 via freq threshold. Do NOT update any shared
 * routes post-detach (no lazy clone fires, no per-peer entries).
 * Action: Unblock peer3 for acceptance. Collapse scans empty set.
 * Verify: Acceptance completes cleanly. Group delivers to peer4.
 */
TEST_P(UpdateGroupLazyCloneTest, CollapseEmptyPerPeerEntries) {
  XLOG(INFO, "=== TEST: CollapseEmptyPerPeerEntries ===");

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

  /* Freq-detach peer3 -- no shared routes beforehand */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);

  injectLocalRoutesAtRuntime({"13.97.0.0/16"}, {"1397:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.97.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.97.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1397:1"));
  injectLocalRoutesAtRuntime({"13.98.0.0/16"}, {"1398:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.98.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.98.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1398:1"));
  injectLocalRoutesAtRuntime({"13.99.0.0/16"}, {"1399:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.99.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.99.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1399:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock -- acceptance with empty per-peer set */
  unblockPeer(kPeerAddr3);

  /* Verify group functional */
  injectLocalRoutesAtRuntime({"13.80.0.0/16"}, {"1380:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.80.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.80.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1380:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  XLOG(INFO, "=== TEST PASSED: CollapseEmptyPerPeerEntries ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupLazyCloneTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
