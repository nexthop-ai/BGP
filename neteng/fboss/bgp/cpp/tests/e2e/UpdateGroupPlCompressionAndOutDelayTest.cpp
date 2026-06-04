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

/* E2E tests: PL compression independence and out-delay interactions.
 * Prefix range: 34.1-34.50/16.
 * Group PL vs detached PL evolve independently
 * PL with mixed IPv4 and IPv6 prefixes — both AFIs packed correctly
 * Withdrawal bypasses out-delay — immediate for all consumers
 * Out-delay disabled when update group enabled — verify
 * Peer blocks during out-delay expiry — routes queued then blocked
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* After detaching peer3, the group PL and the detached PL evolve
 * independently. Updates to the group PL (new routes for peer4) do NOT
 * affect peer3's detached CL, and vice versa. Verify by injecting routes
 * post-detach and confirming only peer4 receives them via PL while peer3's
 * CL accumulates independently. Then inject more routes and verify the
 * group and detached state remain separate. */
TEST_P(UpdateGroupMultiPeerTest, GroupPlVsDetachedPlIndependent) {
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr4);
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

  /* Inject a shared route before detachment */
  injectLocalRoutesAtRuntime({"34.1.0.0/16"}, {"3401:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3401:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3401:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  injectLocalRoutesAtRuntime({"34.2.0.0/16"}, {"3402:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3402:1"));
  injectLocalRoutesAtRuntime({"34.3.0.0/16"}, {"3403:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3403:1"));
  injectLocalRoutesAtRuntime({"34.4.0.0/16"}, {"3404:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3404:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Post-detach: inject 2 more routes — only peer4 receives via group PL */
  injectLocalRoutesAtRuntime({"34.5.0.0/16"}, {"3405:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.5.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.5.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3405:1"));

  injectLocalRoutesAtRuntime({"34.6.0.0/16"}, {"3406:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.6.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.6.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3406:1"));

  /* Withdraw the shared route — only peer4 gets withdrawal */
  withdrawLocalRoutesAtRuntime({"34.1.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "34.1.0.0", 16, kPeerAddr4));

  /* Verify group and detached state are independent */
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);
}

/* Inject both IPv4 and IPv6 routes before detachment, then detach peer3
 * and inject more mixed-AFI routes post-detach. Both AFIs are packed
 * correctly in the group PL and delivered to peer4. */
TEST_P(UpdateGroupMultiPeerTest, MixedIpv4Ipv6PlPacking) {
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr4);
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

  /* Inject v4 and v6 shared routes — drain one at a time */
  injectLocalRoutesAtRuntime({"34.10.0.0/16"}, {"3410:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.10.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3410:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3410:1"));

  injectLocalRoutesAtRuntime({"2001:db8:3411::/48"}, {"3411:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("2001:db8:3411::/48")));
  EXPECT_TRUE(verifyRouteAdd(
      "v6",
      "2001:db8:3411::",
      48,
      kPeerAddr3,
      kNextHopV6_3.str(),
      "4200000001",
      "3411:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v6",
      "2001:db8:3411::",
      48,
      kPeerAddr4,
      kNextHopV6_4.str(),
      "4200000001",
      "3411:1"));

  /* Freq-detach peer3: peer3 has queue (3,2,0), fills naturally */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Need > hwm=2 fill routes to trigger queue blocking with (3,2,0) */
  for (int i = 0; i < 3; ++i) {
    auto prefix = fmt::format("34.{}.0.0/16", 12 + i);
    auto community = fmt::format("{}:1", 3412 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("34.{}.0.0", 12 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Post-detach: inject mixed v4+v6 — both AFIs delivered to peer4 */
  injectLocalRoutesAtRuntime({"34.20.0.0/16"}, {"3420:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3420:1"));

  injectLocalRoutesAtRuntime({"2001:db8:3421::/48"}, {"3421:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("2001:db8:3421::/48")));
  EXPECT_TRUE(verifyRouteAdd(
      "v6",
      "2001:db8:3421::",
      48,
      kPeerAddr4,
      kNextHopV6_4.str(),
      "4200000001",
      "3421:1"));

  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);
  verifySlowPeerInvariants(kPeerAddr4);
}

/* Withdrawal bypasses out-delay — immediate delivery. Inject a shared route,
 * then withdraw it. Both peers receive the withdrawal immediately (no
 * deferred processing). Verifies withdrawals are never delayed regardless
 * of out-delay configuration. */
TEST_P(UpdateGroupMultiPeerTest, WithdrawalBypassesOutDelay) {
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

  /* Inject 2 routes — drain one at a time */
  injectLocalRoutesAtRuntime({"34.25.0.0/16"}, {"3425:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.25.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.25.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3425:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.25.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3425:1"));

  injectLocalRoutesAtRuntime({"34.26.0.0/16"}, {"3426:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.26.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.26.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3426:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.26.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3426:1"));

  /* Withdraw first route — both peers receive immediately */
  withdrawLocalRoutesAtRuntime({"34.25.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "34.25.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "34.25.0.0", 16, kPeerAddr4));

  /* Withdraw second route — also immediate */
  withdrawLocalRoutesAtRuntime({"34.26.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "34.26.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "34.26.0.0", 16, kPeerAddr4));

  /* Both peers still running after withdrawals */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
}

/* Out-delay is disabled when update groups are enabled (per D98009838).
 * Inject routes and verify both peers receive them without any deferred
 * processing. The test confirms that with update groups active, out-delay
 * does not cause any route delivery delays. */
TEST_P(UpdateGroupMultiPeerTest, OutDelayDisabledWithUpdateGroups) {
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

  /* Rapid burst of 4 routes — all should arrive without delay */
  for (int i = 0; i < 4; ++i) {
    auto prefix = fmt::format("34.{}.0.0/16", 30 + i);
    auto community = fmt::format("{}:1", 3430 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("34.{}.0.0", 30 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("34.{}.0.0", 30 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Withdraw one and re-inject with different attributes — no delay */
  withdrawLocalRoutesAtRuntime({"34.30.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "34.30.0.0", 16, kPeerAddr3));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "34.30.0.0", 16, kPeerAddr4));

  injectLocalRoutesAtRuntime({"34.34.0.0/16"}, {"3434:1"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.34.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.34.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3434:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.34.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3434:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
}

/* Peer blocks during route processing — routes are queued and then the peer
 * transitions to JOINED_BLOCKED. After unblocking, the queued routes drain
 * and the peer recovers. This simulates the scenario where out-delay timer
 * fires and pushes routes while a peer is becoming slow. */
TEST_P(UpdateGroupMultiPeerTest, PeerBlocksDuringRouteProcessing) {
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(10, 8, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr4);
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

  /* Inject routes to fill peer3's small queue (3,2,0) naturally */

  injectLocalRoutesAtRuntime({"34.40.0.0/16"}, {"3440:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3440:1"));

  injectLocalRoutesAtRuntime({"34.41.0.0/16"}, {"3441:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3441:1"));

  injectLocalRoutesAtRuntime({"34.42.0.0/16"}, {"3442:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.42.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.42.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3442:1"));

  /* Peer3 should be blocked now — queue (3,2,0) filled past hwm=2 */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));

  /* Unblock — unblockPeer drains queued routes internally (R2 pattern).
   * Do NOT verifyRouteAdd for the drained routes — already consumed. */
  unblockPeer(kPeerAddr3);

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Verify recovery — inject one more route, both peers receive it */
  injectLocalRoutesAtRuntime({"34.43.0.0/16"}, {"3443:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.43.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.43.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3443:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.43.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3443:1"));
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
