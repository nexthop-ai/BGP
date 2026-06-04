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

/* E2E tests: Policy re-evaluation simulation via route operations.
 * Prefix range: 35.x.0.0/16.
 * Policy changes simulated via withdraw + re-inject (setPolicyConfig
 * incompatible with slow peer fixture).
 * No-op policy change, withdrawn prefix re-allow, A->B->A round trip,
 * rapid policy changes, policy change during MRAI pending.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Policy change with no effective change.
 * Withdraw then re-inject same prefix with identical attrs.
 */
TEST_P(UpdateGroupMultiPeerTest, PolicyNoEffectiveChange) {
  XLOGF(INFO, "=== TEST: PolicyNoEffectiveChange ===");

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

  /* Inject a route -- both peers receive it */
  injectLocalRoutesAtRuntime({"35.1.0.0/16"}, {"3501:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3501:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3501:1"));

  /* Simulate "no effective change" policy: withdraw then re-inject same */
  withdrawLocalRoutesAtRuntime({"35.1.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "35.1.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "35.1.0.0", 16, kPeerAddr4));

  /* Re-inject with identical attributes */
  injectLocalRoutesAtRuntime({"35.1.0.0/16"}, {"3501:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3501:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3501:1"));

  /* Both peers should be running and in sync */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOGF(INFO, "=== TEST PASSED: PolicyNoEffectiveChange ===");
}

/* Policy allows prefix withdrawn for group but cloned for peer.
 * Detach peer3, withdraw route (clone preserves), re-inject same prefix.
 */
TEST_P(UpdateGroupMultiPeerTest, PolicyAllowsWithdrawnClonedPrefix) {
  XLOGF(INFO, "=== TEST: PolicyAllowsWithdrawnClonedPrefix ===");

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

  /* Inject a shared route */
  injectLocalRoutesAtRuntime({"35.10.0.0/16"}, {"3510:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.10.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3510:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3510:1"));

  /* Detach peer3 via frequency threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"35.11.0.0/16"}, {"3511:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.11.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.11.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3511:1"));
  injectLocalRoutesAtRuntime({"35.12.0.0/16"}, {"3512:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.12.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.12.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3512:1"));
  injectLocalRoutesAtRuntime({"35.13.0.0/16"}, {"3513:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.13.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3513:1"));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Withdraw the shared route -- lazy clone preserves peer3's view */
  withdrawLocalRoutesAtRuntime({"35.10.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "35.10.0.0", 16, kPeerAddr4));

  /* Re-inject same prefix (simulating policy re-allow) */
  injectLocalRoutesAtRuntime({"35.10.0.0/16"}, {"3510:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3510:1"));

  /* Peer4 still running, peer3 still detached */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOGF(INFO, "=== TEST PASSED: PolicyAllowsWithdrawnClonedPrefix ===");
}

/* Policy A->B->A -- no net change after round-trip. */
TEST_P(UpdateGroupMultiPeerTest, PolicyRoundTripNoNetChange) {
  XLOGF(INFO, "=== TEST: PolicyRoundTripNoNetChange ===");

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

  /* Phase A: inject with community 3520:1 (policy A) */
  injectLocalRoutesAtRuntime({"35.20.0.0/16"}, {"3520:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.20.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3520:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3520:1"));

  /* Withdraw (simulating policy change to B) */
  withdrawLocalRoutesAtRuntime({"35.20.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "35.20.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "35.20.0.0", 16, kPeerAddr4));

  /* Phase B: inject with different community (policy B) */
  injectLocalRoutesAtRuntime({"35.21.0.0/16"}, {"3521:1"}, 160);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.21.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3521:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3521:1"));

  /* Withdraw B, return to A */
  withdrawLocalRoutesAtRuntime({"35.21.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "35.21.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "35.21.0.0", 16, kPeerAddr4));

  /* Phase A again: re-inject with original attrs */
  injectLocalRoutesAtRuntime({"35.20.0.0/16"}, {"3520:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.20.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3520:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3520:1"));

  /* Both peers running with original attrs restored */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOGF(INFO, "=== TEST PASSED: PolicyRoundTripNoNetChange ===");
}

/* Two rapid policy changes -- only latest applied. */
TEST_P(UpdateGroupMultiPeerTest, TwoRapidPolicyChanges) {
  XLOGF(INFO, "=== TEST: TwoRapidPolicyChanges ===");

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

  /* Initial route */
  injectLocalRoutesAtRuntime({"35.30.0.0/16"}, {"3530:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.30.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3530:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3530:1"));

  /* Policy change #1: withdraw old, inject new prefix */
  withdrawLocalRoutesAtRuntime({"35.30.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "35.30.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "35.30.0.0", 16, kPeerAddr4));

  injectLocalRoutesAtRuntime({"35.31.0.0/16"}, {"3531:1"}, 160);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.31.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3531:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3531:1"));

  /* Policy change #2: withdraw #1's route, inject another */
  withdrawLocalRoutesAtRuntime({"35.31.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "35.31.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "35.31.0.0", 16, kPeerAddr4));

  injectLocalRoutesAtRuntime({"35.32.0.0/16"}, {"3532:1"}, 170);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.32.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.32.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3532:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.32.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3532:1"));

  /* Final state: both peers have only the latest route */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOGF(INFO, "=== TEST PASSED: TwoRapidPolicyChanges ===");
}

/* Policy change during MRAI pending. */
TEST_P(UpdateGroupMultiPeerTest, PolicyChangeDuringMrai) {
  XLOGF(INFO, "=== TEST: PolicyChangeDuringMrai ===");

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

  /* Inject 3 routes with different communities (each triggers MRAI cycle) */
  for (int i = 1; i <= 3; i++) {
    auto prefix = fmt::format("35.{}.0.0/16", 40 + i);
    auto community = fmt::format("35{}:1", 40 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("35.{}.0.0", 40 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("35.{}.0.0", 40 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Simulate policy change during MRAI: withdraw one, inject replacement */
  withdrawLocalRoutesAtRuntime({"35.41.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "35.41.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "35.41.0.0", 16, kPeerAddr4));

  /* Inject replacement with different attrs (different prefix to avoid
   * CL suppression -- same learned pattern as withdraw+reinject) */
  injectLocalRoutesAtRuntime({"35.44.0.0/16"}, {"3544:1"}, 170);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("35.44.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.44.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3544:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "35.44.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3544:1"));

  /* Both peers running and in sync after all operations */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOGF(INFO, "=== TEST PASSED: PolicyChangeDuringMrai ===");
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
