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
 * E2E tests: Entry Collapse -- Retain and Orphan Cases
 *
 * CollapseNoEntriesMatch: No per-peer entries match -- all retained
 * CollapseSamePrefixDifferentAttrs: Same prefix different attributes --
 * retained CollapseOrphanedPerPeerEntry: Orphaned per-peer entry (group
 * withdrew prefix)
 *
 * Prefix range: 13.87-13.96/16
 * Fixture: UpdateGroupLazyCloneTest
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Collapse -- orphaned per-peer entry for withdrawn prefix.
 *
 * Setup: Shared route. Detach peer3. Withdraw the shared route (Case 4
 * clone creates per-peer entry, then group removes its entry). Peer3 now
 * has a per-peer entry for a prefix the group no longer has.
 * Action: Unblock for acceptance. Collapse finds orphaned entry.
 * Verify: Orphaned entry cleaned up. No crash. Group functional.
 */
TEST_P(UpdateGroupLazyCloneTest, CollapseOrphanedPerPeerEntry) {
  XLOG(INFO, "=== TEST: CollapseOrphanedPerPeerEntry ===");

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

  /* Both peers receive shared route */
  injectLocalRoutesAtRuntime({"13.81.0.0/16"}, {"1381:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.81.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.81.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1381:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.81.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1381:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);

  injectLocalRoutesAtRuntime({"13.82.0.0/16"}, {"1382:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.82.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.82.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1382:1"));
  injectLocalRoutesAtRuntime({"13.83.0.0/16"}, {"1383:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.83.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.83.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1383:1"));
  injectLocalRoutesAtRuntime({"13.84.0.0/16"}, {"1384:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.84.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.84.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1384:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Withdraw shared route -- clone fires, then group removes entry */
  withdrawLocalRoutesAtRuntime({"13.81.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "13.81.0.0", 16, kPeerAddr4));

  verifySlowPeerInvariants(kPeerAddr3);

  /* Unblock -- acceptance cleans up orphaned per-peer entry */
  unblockPeer(kPeerAddr3);

  /* Verify group functional */
  injectLocalRoutesAtRuntime({"13.85.0.0/16"}, {"1385:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.85.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.85.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1385:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  XLOG(INFO, "=== TEST PASSED: CollapseOrphanedPerPeerEntry ===");
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
