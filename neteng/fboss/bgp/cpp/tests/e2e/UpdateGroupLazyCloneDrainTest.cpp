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
 * E2E tests: Lazy Clone During Drain Operations
 * Tests for lazy clone behavior during withdrawal, PL drain,
 * and group withdrawal processing.
 *
 * Prefix range: 13.x.0.0/16
 * Fixture: UpdateGroupLazyCloneTest
 *
 * Tests implemented:
 *   Clone on withdrawal — entry preserved for detached peer
 *   Clone during group PL drain
 *   Clone during group withdrawal processing
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Clone fires on withdrawal — entry preserved for peer,
 * removed from group.
 *
 * Setup: Both peers share 2 routes. Peer3 is detached.
 * Action: Withdraw one shared route. The other stays.
 * Verify: Peer4 receives the withdrawal. Peer3 is still detached
 * with invariants intact. The group no longer has the withdrawn
 * route, but peer3's per-peer clone preserves it. A second
 * withdrawal of a different shared route also works correctly.
 */
TEST_P(UpdateGroupLazyCloneTest, CloneOnWithdrawalPreservesEntry) {
  XLOG(INFO, "=== TEST: CloneOnWithdrawalPreservesEntry ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Both peers receive 2 shared routes */
  injectLocalRoutesAtRuntime({"13.20.0.0/16"}, {"1320:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.20.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1320:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1320:1"));

  injectLocalRoutesAtRuntime({"13.21.0.0/16"}, {"1321:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.21.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1321:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1321:1"));

  /* Detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"13.22.0.0/16"}, {"1322:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.22.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.22.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1322:1"));
  injectLocalRoutesAtRuntime({"13.23.0.0/16"}, {"1323:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.23.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.23.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1323:1"));
  injectLocalRoutesAtRuntime({"13.24.0.0/16"}, {"1324:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.24.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.24.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1324:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Withdraw first shared route — clone fires, preserves for peer3 */
  withdrawLocalRoutesAtRuntime({"13.20.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "13.20.0.0", 16, kPeerAddr4));

  verifySlowPeerInvariants(kPeerAddr3);
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));

  /* Withdraw second shared route — clone fires again for this prefix */
  withdrawLocalRoutesAtRuntime({"13.21.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "13.21.0.0", 16, kPeerAddr4));

  verifySlowPeerInvariants(kPeerAddr3);
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  XLOG(INFO, "=== TEST PASSED: CloneOnWithdrawalPreservesEntry ===");
}

/*
 * Clone fires during group PL drain.
 *
 * Setup: Both peers receive a shared route. Peer3 is detached.
 * Action: Inject multiple routes rapidly (group processes PL drain),
 * then update one of the pre-detach shared routes. The clone should
 * fire BEFORE the entry is mutated even during active PL drain.
 * Verify: Peer4 receives all routes. Peer3 invariants hold.
 * The group's PL drain and lazy clone work together correctly.
 */
TEST_P(UpdateGroupLazyCloneTest, CloneDuringGroupPLDrain) {
  XLOG(INFO, "=== TEST: CloneDuringGroupPLDrain ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Both peers receive shared routes before detachment */
  injectLocalRoutesAtRuntime({"13.30.0.0/16"}, {"1330:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.30.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1330:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1330:1"));

  injectLocalRoutesAtRuntime({"13.31.0.0/16"}, {"1331:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.31.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1331:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1331:1"));

  /* Detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"13.32.0.0/16"}, {"1332:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.32.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.32.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1332:1"));
  injectLocalRoutesAtRuntime({"13.33.0.0/16"}, {"1333:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.33.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.33.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1333:1"));
  injectLocalRoutesAtRuntime({"13.34.0.0/16"}, {"1334:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.34.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.34.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1334:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /*
   * Inject more routes (triggers PL drain) AND update a pre-detach
   * shared route in the same batch. The group processes PL drain
   * with lazy clone firing for the shared route update.
   */
  injectLocalRoutesAtRuntime({"13.35.0.0/16"}, {"1335:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.35.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.35.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1335:1"));

  /* Update the shared route — clone fires during PL drain processing */
  injectLocalRoutesAtRuntime({"13.30.0.0/16"}, {"1330:88"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1330:88"));

  /* Verify invariants: detached peer3 state preserved */
  verifySlowPeerInvariants(kPeerAddr3);
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Group version advanced past detached peer */
  auto adjRib3 = getAdjRib(kPeerAddr3);
  auto group = getUpdateGroupForPeer(kPeerAddr3);
  ASSERT_NE(adjRib3, nullptr);
  ASSERT_NE(group, nullptr);
  EXPECT_GE(group->getLastSeenRibVersion(), adjRib3->getDetachedRibVersion());

  XLOG(INFO, "=== TEST PASSED: CloneDuringGroupPLDrain ===");
}

/*
 * Clone fires during group withdrawal processing.
 *
 * Setup: Both peers receive multiple shared routes. Peer3 is detached.
 * Action: Withdraw multiple shared routes in sequence. Each withdrawal
 * triggers a lazy clone for peer3 before the group removes the entry.
 * Verify: Peer4 receives all withdrawals. Peer3 invariants hold
 * throughout the withdrawal processing. The clone fires BEFORE
 * each removal, preserving peer3's view of those routes.
 */
TEST_P(UpdateGroupLazyCloneTest, CloneDuringGroupWithdrawalProcessing) {
  XLOG(INFO, "=== TEST: CloneDuringGroupWithdrawalProcessing ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Both peers receive 3 shared routes before detachment */
  injectLocalRoutesAtRuntime({"13.40.0.0/16"}, {"1340:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.40.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1340:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1340:1"));

  injectLocalRoutesAtRuntime({"13.41.0.0/16"}, {"1341:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.41.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1341:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1341:1"));

  injectLocalRoutesAtRuntime({"13.42.0.0/16"}, {"1342:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.42.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.42.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1342:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.42.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1342:1"));

  /* Detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"13.43.0.0/16"}, {"1343:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.43.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.43.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1343:1"));
  injectLocalRoutesAtRuntime({"13.44.0.0/16"}, {"1344:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.44.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.44.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1344:1"));
  injectLocalRoutesAtRuntime({"13.45.0.0/16"}, {"1345:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.45.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.45.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1345:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /*
   * Withdraw all 3 shared routes in sequence. Each withdrawal
   * triggers a lazy clone before the group removes the entry.
   */
  withdrawLocalRoutesAtRuntime({"13.40.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "13.40.0.0", 16, kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);

  withdrawLocalRoutesAtRuntime({"13.41.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "13.41.0.0", 16, kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);

  withdrawLocalRoutesAtRuntime({"13.42.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "13.42.0.0", 16, kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);

  /* All 3 shared routes withdrawn — peer3 still detached, peer4 functional */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Group should still be functional — inject a new route to peer4 */
  injectLocalRoutesAtRuntime({"13.46.0.0/16"}, {"1346:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.46.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.46.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1346:1"));

  XLOG(INFO, "=== TEST PASSED: CloneDuringGroupWithdrawalProcessing ===");
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
