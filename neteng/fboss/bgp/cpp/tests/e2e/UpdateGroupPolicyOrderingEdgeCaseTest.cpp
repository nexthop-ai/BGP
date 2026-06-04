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

/* E2E tests: Policy change ordering and edge cases.
 * Prefix range: 31.x.0.0/16
 *
 * Policy change ordering — PL drain (old policy) then new policy
 * Policy change immediately after detachment — ordering with PL clone
 * Policy change with add-path — per-pathId re-evaluation
 * Old policy denied, new policy allows — entry created from scratch
 * Per-peer egress policy change — peer leaves group
 *
 * Policy changes cannot be triggered via setPolicyConfig with update groups
 * enabled (CHECK failure). Simulated via withdraw + re-inject with different
 * attributes/prefixes.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Policy change ordering — PL drain (old) THEN processRibDumpReq (new).
 * Simulate: inject routes so PL has items, then withdraw+re-inject (policy
 * change) while PL is draining. The drain should complete with old-policy
 * routes, then the new-policy routes arrive as CL items processed afterward.
 * Use queue (5,4,0) so we can observe PL drain without blocking.
 */
TEST_P(UpdateGroupMultiPeerTest, PolicyChangeOrdering_PlDrainThenNew) {
  XLOG(INFO, "=== TEST: PolicyChangeOrdering_PlDrainThenNew ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

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

  /* Inject 3 routes with old-policy attributes (LP=150) */
  for (int i = 1; i <= 3; ++i) {
    auto prefix = fmt::format("31.{}.0.0/16", i);
    auto community = fmt::format("31{:02d}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Simulate policy change: withdraw old route, inject new with LP=200 */
  withdrawLocalRoutesAtRuntime({"31.1.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "31.1.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "31.1.0.0", 16, kPeerAddr4));

  /* New-policy route with different prefix to avoid CL suppression */
  injectLocalRoutesAtRuntime({"31.10.0.0/16"}, {"3110:1"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.10.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3110:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3110:1"));

  /* Old-policy routes (31.2, 31.3) still in RIB, new-policy route (31.10) added
   */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED ===");
}

/*
 * Policy change immediately after detachment — ordering with PL clone.
 * Detach peer3 via freq, then immediately simulate policy change (withdraw +
 * re-inject). The CL should capture both the old withdrawal and new
 * announcement for the detached peer. Peer4 (in-sync) receives both inline.
 */
TEST_P(UpdateGroupMultiPeerTest, PolicyChangeImmediatelyAfterDetach) {
  XLOG(INFO, "=== TEST: PolicyChangeImmediatelyAfterDetach ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* Fast peer (peer4) gets large queue — never blocks */
  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr4);

  /* Slow peer (peer3) gets small queue — fills naturally */
  setDefaultQueueSizes(3, 2, 0);
  bringUpPeer(kPeerAddr3);

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Inject shared route before detachment */
  injectLocalRoutesAtRuntime({"31.20.0.0/16"}, {"3120:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.20.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3120:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3120:1"));

  /* Freq-detach peer3: threshold=1, inject 3 routes to fill peer3's (3,2,0)
   * queue past hwm. Only drain peer4 — don't read from peer3 so it fills. */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("31.{}.0.0/16", 21 + i);
    auto community = fmt::format("312{}:1", 1 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("31.{}.0.0", 21 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Immediately simulate policy change: withdraw shared + inject new */
  withdrawLocalRoutesAtRuntime({"31.20.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "31.20.0.0", 16, kPeerAddr4));

  injectLocalRoutesAtRuntime({"31.30.0.0/16"}, {"3130:1"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3130:1"));

  /* Peer4 got both withdrawal and new announcement; peer3 CL accumulates */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED ===");
}

/*
 * Policy change with add-path — per-pathId re-evaluation.
 * E2E framework has no explicit pathId support. Simulate with 2 distinct
 * prefixes as separate "paths". Withdraw one path, re-inject with different
 * attributes. The other path should be unaffected. Verify independent
 * delivery for each "path" (prefix).
 */
TEST_P(UpdateGroupMultiPeerTest, PolicyChangeAddPath_PerPathId) {
  XLOG(INFO, "=== TEST: PolicyChangeAddPath_PerPathId ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

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

  /* Path A and Path B — two distinct prefixes as independent paths */
  injectLocalRoutesAtRuntime({"31.30.0.0/16"}, {"3130:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.30.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3130:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3130:1"));

  injectLocalRoutesAtRuntime({"31.31.0.0/16"}, {"3131:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.31.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3131:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3131:1"));

  /* Policy change on Path A only: withdraw + re-inject with different attrs */
  withdrawLocalRoutesAtRuntime({"31.30.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "31.30.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "31.30.0.0", 16, kPeerAddr4));

  injectLocalRoutesAtRuntime({"31.32.0.0/16"}, {"3132:1"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.32.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.32.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3132:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.32.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3132:1"));

  /* Path B (31.31) should be completely unaffected */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED ===");
}

/*
 * Old policy denied (no entry), new policy allows — entry from scratch.
 * Simulate: no route injected initially (old policy denied it). Then inject
 * the route (simulates new policy allowing it). Both peers receive a fresh
 * announcement with no prior entry to clone or update.
 */
TEST_P(UpdateGroupMultiPeerTest, OldDenied_NewAllows_FreshEntry) {
  XLOG(INFO, "=== TEST: OldDenied_NewAllows_FreshEntry ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

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

  /* Old policy: no routes injected (denied). Verify group is IDLE/stable */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* New policy allows: inject fresh route — no prior entry exists */
  injectLocalRoutesAtRuntime({"31.40.0.0/16"}, {"3140:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.40.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3140:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3140:1"));

  /* Inject a second route to confirm continued operation */
  injectLocalRoutesAtRuntime({"31.41.0.0/16"}, {"3141:1"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.41.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3141:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3141:1"));

  /* Both peers received fresh entries — no clone needed */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED ===");
}

/*
 * Per-peer egress policy change — peer leaves group, joins/creates
 * different group. Simulated: bring up 2 peers, inject routes (both receive),
 * bring peer3 DOWN (simulates leaving due to policy change), verify peer4
 * continues operating alone. Inject more routes — only peer4 receives.
 * This tests that one peer leaving doesn't disrupt the remaining peer.
 */
TEST_P(UpdateGroupMultiPeerTest, PerPeerPolicyChange_PeerLeavesGroup) {
  XLOG(INFO, "=== TEST: PerPeerPolicyChange_PeerLeavesGroup ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

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

  /* Inject shared route — both peers receive */
  injectLocalRoutesAtRuntime({"31.50.0.0/16"}, {"3150:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.50.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.50.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3150:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.50.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3150:1"));

  /* Peer3 leaves group (simulates per-peer policy change) */
  bringDownPeer(kPeerAddr3);

  /* Peer4 continues operating as sole group member */
  injectLocalRoutesAtRuntime({"31.51.0.0/16"}, {"3151:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.51.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.51.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3151:1"));

  injectLocalRoutesAtRuntime({"31.52.0.0/16"}, {"3152:1"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("31.52.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.52.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3152:1"));

  /* Peer4 is the sole member, still running correctly */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  verifySlowPeerInvariants(kPeerAddr4);

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
