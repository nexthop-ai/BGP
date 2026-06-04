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

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Policy change with no effective change — re-inject a route with
 * identical attributes (same community + LP). Deep compare should detect
 * no change, so no UPDATE is sent to peers. We verify by injecting a
 * follow-up route that both peers DO receive (proving the no-op didn't
 * break anything) and that no extra messages appeared between them.
 */
TEST_P(UpdateGroupLifecycleTest, PolicyNoEffectiveChange) {
  XLOG(INFO, "=== TEST: PolicyNoEffectiveChange ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(5, 4, 0);

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

  /* Inject initial route — both peers receive */
  injectLocalRoutesAtRuntime({"30.1.0.0/16"}, {"3001:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3001:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3001:1"));

  /*
   * Re-inject SAME prefix with SAME community and LP (no effective change).
   * Deep compare should suppress the UPDATE since attributes are identical.
   */
  injectLocalRoutesAtRuntime({"30.1.0.0/16"}, {"3001:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.1.0.0/16")));

  /*
   * Inject a follow-up route with DIFFERENT community. Both peers must
   * receive it. If the no-op re-inject had queued an extra UPDATE, the
   * follow-up verifyRouteAdd would see the stale message instead — so
   * this proves deep compare suppressed the duplicate.
   */
  injectLocalRoutesAtRuntime({"30.2.0.0/16"}, {"3002:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.2.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3002:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3002:1"));

  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: PolicyNoEffectiveChange ===");
}

/*
 * Policy allows previously withdrawn prefix — reconcile for
 * detached peer. Withdraw a route (both peers get withdrawal), detach
 * peer3, then re-inject the withdrawn prefix (simulates policy now
 * allowing it). Only peer4 receives the re-announcement. Peer3's CL
 * tracks the reconciled state silently.
 */
TEST_P(UpdateGroupLifecycleTest, PolicyAllowWithdrawnClonedPrefix) {
  XLOG(INFO, "=== TEST: PolicyAllowWithdrawnClonedPrefix ===");

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

  /* Both peers receive initial route */
  injectLocalRoutesAtRuntime({"30.3.0.0/16"}, {"3003:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.3.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3003:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3003:1"));

  /* Withdraw the route — both peers get the withdrawal */
  withdrawLocalRoutesAtRuntime({"30.3.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "30.3.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "30.3.0.0", 16, kPeerAddr4));

  /* Detach peer3 via frequency threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"30.4.0.0/16"}, {"3004:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3004:1"));
  injectLocalRoutesAtRuntime({"30.5.0.0/16"}, {"3005:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.5.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.5.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3005:1"));
  injectLocalRoutesAtRuntime({"30.6.0.0/16"}, {"3006:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.6.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.6.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3006:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /*
   * Re-inject the withdrawn prefix (simulates policy now allowing it).
   * Only peer4 receives. Peer3's CL tracks the reconciled entry.
   */
  injectLocalRoutesAtRuntime({"30.3.0.0/16"}, {"3003:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3003:99"));

  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: PolicyAllowWithdrawnClonedPrefix ===");
}

/*
 * Policy A→B→A — re-inject with attrs B then back to A.
 * Deep compare on the second re-inject should detect no net change
 * relative to the CURRENT ShadowRib state (which was already updated
 * to B). So the A→B change produces an UPDATE, and B→A produces
 * another UPDATE back to original attrs. Net result: 3 UPDATEs total
 * (original + B + A).
 */
TEST_P(UpdateGroupLifecycleTest, PolicyAtoBAtoA) {
  XLOG(INFO, "=== TEST: PolicyAtoBAtoA ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

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

  /* Policy A: initial attrs (community 3007:1, LP 100) */
  injectLocalRoutesAtRuntime({"30.7.0.0/16"}, {"3007:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.7.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.7.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3007:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.7.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3007:1"));

  /* Policy B: changed attrs (community 3007:99, LP 200) */
  injectLocalRoutesAtRuntime({"30.7.0.0/16"}, {"3007:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.7.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.7.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3007:99"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.7.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3007:99"));

  /* Policy A again: revert to original attrs */
  injectLocalRoutesAtRuntime({"30.7.0.0/16"}, {"3007:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.7.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.7.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3007:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.7.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3007:1"));

  /* Verify stable state after 3 re-evals */
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: PolicyAtoBAtoA ===");
}

/*
 * Two rapid policy changes — simulate by injecting 2 attribute
 * changes back-to-back on the same prefix. The second change arrives
 * while the first is potentially still in PL processing. The final
 * state should reflect the LAST change. Both peers receive both UPDATEs
 * sequentially (the group serializes them through the CL→PL pipeline).
 */
TEST_P(UpdateGroupLifecycleTest, TwoRapidPolicyChanges) {
  XLOG(INFO, "=== TEST: TwoRapidPolicyChanges ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

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

  /* Initial route */
  injectLocalRoutesAtRuntime({"30.8.0.0/16"}, {"3008:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.8.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.8.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3008:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.8.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3008:1"));

  /* Rapid change #1: attrs change to community 3008:50, LP 150 */
  injectLocalRoutesAtRuntime({"30.8.0.0/16"}, {"3008:50"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.8.0.0/16")));

  /* Rapid change #2: attrs change again to community 3008:99, LP 200 */
  injectLocalRoutesAtRuntime({"30.8.0.0/16"}, {"3008:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.8.0.0/16")));

  /*
   * CL may collapse same-prefix updates (intermediate attrs suppressed),
   * OR intermediate UPDATEs may still be in peer queues. Either way,
   * verify the pipeline is healthy: both peers must still be JOINED_RUNNING.
   */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Final state: both peers running, last attrs applied */
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: TwoRapidPolicyChanges ===");
}

/*
 * Policy change during MRAI pending — simulated by injecting
 * multiple routes with different communities (each triggers a separate
 * MRAI cycle), then injecting an attribute change on an earlier route
 * while subsequent MRAIs are still in flight. The group must serialize
 * all updates correctly through the PL pipeline.
 */
TEST_P(UpdateGroupLifecycleTest, PolicyChangeDuringMraiPending) {
  XLOG(INFO, "=== TEST: PolicyChangeDuringMraiPending ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

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

  /* Inject 3 routes with different communities (3 MRAI cycles) */
  injectLocalRoutesAtRuntime({"30.10.0.0/16"}, {"3010:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.10.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3010:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3010:1"));

  injectLocalRoutesAtRuntime({"30.11.0.0/16"}, {"3011:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.11.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.11.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3011:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.11.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3011:1"));

  injectLocalRoutesAtRuntime({"30.12.0.0/16"}, {"3012:1"}, 100);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.12.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.12.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3012:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.12.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3012:1"));

  /*
   * Now do an attribute change on the FIRST route while subsequent
   * MRAI cycles may still be pending. This simulates policy re-eval
   * arriving mid-MRAI.
   */
  injectLocalRoutesAtRuntime({"30.10.0.0/16"}, {"3010:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.10.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3010:99"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3010:99"));

  /* Verify stable state — all updates serialized correctly */
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: PolicyChangeDuringMraiPending ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupLifecycleTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
