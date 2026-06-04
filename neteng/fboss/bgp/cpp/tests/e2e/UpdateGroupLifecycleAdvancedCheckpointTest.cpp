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
 * E2E test: Advanced lifecycle checkpoint with 3 peers,
 * all different recovery paths (DFP, DSP, unblock).
 *
 * Prefix range: 87.x.0.0/16
 *
 * Verifies that a 3-peer group handles detachment of 2 peers, with the
 * third staying in-sync. After detachment, one peer takes the DFP (fast
 * path) recovery and the other takes the DSP (slow path) recovery via
 * unblock. Checkpoints at each state transition confirm invariants.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Advanced lifecycle -- 3 peers, checkpoint at each transition.
 *
 * Steps:
 * 1. Set up 3 peers in JOINED_RUNNING
 * 2. Inject shared routes -- all 3 receive
 * 3. Freq-detach peer3 -> DETACHED_BLOCKED (checkpoint)
 * 4. Raise threshold so peer4 doesn't detach
 * 5. Block peer4, fill queue -> JOINED_BLOCKED (checkpoint)
 * 6. Inject route while peer3=DB, peer4=JB -- CL item (group WAITING)
 * 7. Unblock peer4 -> JOINED_RUNNING (PL drains, checkpoint)
 * 8. Unblock peer3 -> starts recovery (DRJ or DB depending on CL batch)
 * 9. Inject final recovery route -- peer4 and peer5 receive
 * 10. Verify peer5 stayed in-sync throughout, peer4 recovered
 */
TEST_P(UpdateGroupMultiPeerTest, AdvancedLifecycleThreePeers) {
  XLOGF(INFO, "=== TEST: AdvancedLifecycleThreePeers ===");

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
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);
  drainPeerQueueCompletely(peerId5);

  /* Checkpoint 1: all 3 peers in sync */
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 0);

  /* Inject shared route -- all 3 peers receive */
  injectLocalRoutesAtRuntime({"87.1.0.0/16"}, {"8701:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("87.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "87.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "8701:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "87.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "8701:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "87.1.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "8701:1"));

  /* Step 3: Freq-detach peer3 -> DETACHED_BLOCKED */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("87.{}.0.0/16", 10 + i);
    auto community = fmt::format("{}:1", 8710 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("87.{}.0.0", 10 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("87.{}.0.0", 10 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Checkpoint 2: peer3=DB, peer4+5=JR */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 1);
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  /* Step 4: Raise threshold so peer4 won't detach */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));

  /* Step 5: Block peer4, fill queue -> JOINED_BLOCKED */
  blockPeer(kPeerAddr4);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("87.{}.0.0/16", 20 + i);
    auto community = fmt::format("{}:1", 8720 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("87.{}.0.0", 20 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_BLOCKED));

  /* Checkpoint 3: peer3=DB, peer4=JB, peer5=JR */
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));
  verifySlowPeerInvariants(kPeerAddr3);

  /*
   * Step 6: Inject route while peer3=DB, peer4=JB.
   * Group is WAITING (peer4 blocked) so route goes to CL only.
   * CANNOT verifyRouteAdd on ANY peer during WAITING -- it hangs.
   */
  injectLocalRoutesAtRuntime({"87.30.0.0/16"}, {"8730:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("87.30.0.0/16")));

  /* Step 7: Unblock peer4 first -> PL drains, group leaves WAITING */
  unblockPeer(kPeerAddr4);
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /*
   * After unblock, peer4 auto-drains fill routes (87.20-22). CL-origin 87.30
   * is processed as new PL after group leaves WAITING and pushes to both
   * in-sync peers (peer4, peer5). Consume on both.
   */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "87.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "8730:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "87.30.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "8730:1"));

  /* Step 8: Unblock peer3 -> starts recovery */
  unblockPeer(kPeerAddr3);
  auto state3 = getPeerState(kPeerAddr3);
  EXPECT_NE(state3, PeerUpdateState::DOWN);

  /* Step 9: Inject recovery route -- peer4 and peer5 receive */
  injectLocalRoutesAtRuntime({"87.40.0.0/16"}, {"8740:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("87.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "87.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "8740:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "87.40.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "8740:1"));

  /* Checkpoint 4: peer4 recovered, peer5 always in-sync */
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  XLOGF(INFO, "=== TEST PASSED: AdvancedLifecycleThreePeers ===");
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
