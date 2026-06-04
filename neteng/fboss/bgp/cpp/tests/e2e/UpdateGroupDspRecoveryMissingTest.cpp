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
 * E2E tests: DSP (Detached Slow Peer) recovery tests
 *
 * Prefix range: 40.x.0.0/16
 *
 * Tests:
 *   DSP consumes CL, new items arrive mid-consumption
 *   DSP processes CL withdrawal -- removes per-peer entry
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * DSP consumes CL, new items arrive mid-consumption.
 * After unblock, inject 2 routes while DSP is consuming CL.
 * All items consumed, peer4 receives all normally.
 */
TEST_P(UpdateGroupMultiPeerTest, DSP_NewItemsMidConsumption) {
  XLOG(INFO, "=== TEST: DSP_NewItemsMidConsumption ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(5, 4, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  /* Two waitForEoR per peer — one per AFI (v4 + v6), since the update-group
   * key negotiates both AfiIpv4Negotiated and AfiIpv6Negotiated. */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 5; i++) {
    auto prefix = fmt::format("40.{}.0.0/16", 10 + i);
    auto community = fmt::format("{}:1", 4010 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("40.{}.0.0", 10 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  /* DSP (peer3) invariants while detached */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));
  verifySlowPeerInvariants(kPeerAddr3);

  /* Unblock to start recovery */
  unblockPeer(kPeerAddr3);

  /* Inject 2 routes while DSP is consuming -- mid-consumption */
  for (int i = 0; i < 2; i++) {
    auto prefix = fmt::format("40.{}.0.0/16", 16 + i);
    auto community = fmt::format("{}:1", 4016 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("40.{}.0.0", 16 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* DSP recovery: wait for peer3 to consume CL items (pre-detach Queue fill
   * + 2 mid-consumption CL items) and rejoin the group. */
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING, 200));

  /* Drain the mid-consumption CL items from peer3's queue. Peer3 received
   * 40.16/17 via CL while in DETACHED_RUNNING; they are sitting in peer3's
   * queue ahead of any post-recovery PL route. Verify them here so the
   * next verifyRouteAdd for 40.20 hits a clean queue. */
  for (int i = 0; i < 2; i++) {
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("40.{}.0.0", 16 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        fmt::format("{}:1", 4016 + i)));
  }

  /* Post-recovery: DSP peer3 is back in-sync. Inject a fresh route and
   * verify BOTH peers receive it via the group's PL (not via CL). */
  injectLocalRoutesAtRuntime({"40.20.0.0/16"}, {"4020:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("40.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "40.20.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "4020:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "40.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "4020:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 0);

  XLOG(INFO, "=== TEST PASSED: DSP_NewItemsMidConsumption ===");
}

/*
 * DSP processes CL withdrawal -- removes per-peer entry.
 * Inject shared route before detach. After detach, withdraw it.
 * Lazy clone fires (Case 4). Peer4 gets withdrawal. Peer3's CL
 * will process the withdrawal during recovery.
 */
TEST_P(UpdateGroupMultiPeerTest, DSP_ProcessWithdrawal) {
  XLOG(INFO, "=== TEST: DSP_ProcessWithdrawal ===");

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

  /* Inject shared route before detachment */
  injectLocalRoutesAtRuntime({"40.30.0.0/16"}, {"4030:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("40.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "40.30.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "4030:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "40.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "4030:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"40.31.0.0/16"}, {"4031:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("40.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "40.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "4031:1"));
  injectLocalRoutesAtRuntime({"40.32.0.0/16"}, {"4032:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("40.32.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "40.32.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "4032:1"));
  injectLocalRoutesAtRuntime({"40.33.0.0/16"}, {"4033:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("40.33.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "40.33.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "4033:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Withdraw shared route -- triggers Case 4 lazy clone */
  withdrawLocalRoutesAtRuntime({"40.30.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "40.30.0.0", 16, kPeerAddr4));

  /* Peer3 still detached, peer4 got withdrawal */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);

  /* Unblock peer3 -- CL will drain including the Case-4 withdrawal entry
   * for 40.30.0.0/16. After recovery, peer3 should receive the withdrawal
   * via its CL (not via PL since peer3 was detached when withdrawal fired). */
  unblockPeer(kPeerAddr3);
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING, 200));

  /* DSP peer3 must have processed the CL withdrawal during recovery */
  EXPECT_TRUE(verifyRouteWithdraw("v4", "40.30.0.0", 16, kPeerAddr3));

  /* Post-recovery: inject a fresh route; both peers must receive via PL */
  injectLocalRoutesAtRuntime({"40.34.0.0/16"}, {"4034:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("40.34.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "40.34.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "4034:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "40.34.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "4034:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 0);

  XLOG(INFO, "=== TEST PASSED: DSP_ProcessWithdrawal ===");
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
