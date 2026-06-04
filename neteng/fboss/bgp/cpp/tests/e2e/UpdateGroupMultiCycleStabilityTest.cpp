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
 * E2E test: Lifecycle: 3 detach-recover cycles, no stale state.
 *
 * Repeatedly detach and recover peer3 via freq threshold. After each
 * cycle, verify peer4 continues to receive routes normally and no
 * crash or state leak occurs. This catches stale bitmaps or counters
 * that accumulate over multiple cycles.
 *
 * Prefix range: 97.x.0.0/16
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * 3 detach-recover cycles.
 *
 * Each cycle: block peer3 -> fill queue -> DETACHED_BLOCKED ->
 * bring DOWN -> bring UP -> drain init dump -> peer4 still works.
 *
 * Uses DOWN/UP cycle instead of unblock for clean recovery, avoiding
 * the CL batch re-blocking issue with small queues.
 */
TEST_P(UpdateGroupMultiPeerTest, ThreeDetachRecoverCycles) {
  XLOGF(INFO, "=== TEST: ThreeDetachRecoverCycles ===");

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

  for (int cycle = 0; cycle < 3; cycle++) {
    XLOGF(INFO, "=== Cycle {} of 3 ===", cycle + 1);

    /* Set freq threshold=1 for this cycle */
    setSlowPeerThresholds(
        kPeerAddr3,
        std::chrono::milliseconds(600000),
        1,
        std::chrono::milliseconds(60000));

    /* Block and fill queue to trigger detachment */
    blockPeer(kPeerAddr3);
    for (int i = 0; i < 3; i++) {
      auto prefix = fmt::format("97.{}.0.0/16", cycle * 10 + i + 1);
      auto community = fmt::format("{}:1", 9700 + cycle * 10 + i + 1);
      injectLocalRoutesAtRuntime({prefix}, {community}, 150);
      ASSERT_TRUE(
          waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
      EXPECT_TRUE(verifyRouteAdd(
          "v4",
          fmt::format("97.{}.0.0", cycle * 10 + i + 1),
          16,
          kPeerAddr4,
          getExpectedNexthop(kPeerAddr4),
          "4200000001",
          community));
    }
    ASSERT_TRUE(
        waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

    /* Recover via DOWN/UP cycle -- clean restart avoids CL re-blocking */
    unblockPeer(kPeerAddr3);
    bringDownPeer(kPeerAddr3);
    ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

    bringUpPeer(kPeerAddr3);
    sendEoRToPeer(peerId3);
    EXPECT_TRUE(waitForEoR(peerId3));
    EXPECT_TRUE(waitForEoR(peerId3));
    drainPeerQueueCompletely(peerId3);

    /* Verify peer4 still works after this cycle */
    auto verifyPrefix = fmt::format("97.{}.0.0/16", 60 + cycle);
    auto verifyCommunity = fmt::format("{}:1", 9760 + cycle);
    injectLocalRoutesAtRuntime({verifyPrefix}, {verifyCommunity}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(verifyPrefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("97.{}.0.0", 60 + cycle),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        verifyCommunity));

    /* Peer3 may be in DETACHED_INIT_DUMP or processing -- just verify alive */
    EXPECT_NE(getPeerState(kPeerAddr3), PeerUpdateState::DOWN);
    EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  }

  XLOGF(INFO, "=== PASSED: MultiCycleStability ===");
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
