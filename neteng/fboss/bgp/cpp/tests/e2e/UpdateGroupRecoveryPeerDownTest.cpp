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
 * E2E test: Lifecycle: peer DOWN mid-recovery, comes back fresh.
 *
 * Freq-detach peer3, unblock to start recovery, then immediately
 * bringDownPeer. Bring peer3 back up -- it enters DETACHED_INIT_DUMP
 * (reconnecting to existing group). Verify peer4 continues working
 * throughout the entire sequence. No crash, no hang.
 *
 * Prefix range: 96.40-96.60/16
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Peer goes DOWN during recovery, comes back fresh.
 *
 * This tests the lifecycle: JR -> JB -> DB -> unblock (DRJ) -> DOWN -> UP
 * (DETACHED_INIT_DUMP) -> drain -> verify peer4 still works.
 *
 * Per learned patterns: reconnecting peer enters DETACHED_INIT_DUMP
 * and may never reach JOINED_RUNNING. Must unblockPeer BEFORE
 * bringUpPeer to clear blocked state from prior session.
 */
TEST_P(UpdateGroupMultiPeerTest, PeerDownMidRecoveryComesFresh) {
  XLOGF(INFO, "=== TEST: PeerDownMidRecoveryComesFresh ===");

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
  injectLocalRoutesAtRuntime({"96.40.0.0/16"}, {"9640:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("96.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "96.40.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "9640:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "96.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "9640:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("96.{}.0.0/16", 41 + i);
    auto community = fmt::format("{}:1", 9641 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("96.{}.0.0", 41 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Peer3 DOWN mid-recovery: unblock then immediately down */
  unblockPeer(kPeerAddr3);
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Peer4 should still be working */
  injectLocalRoutesAtRuntime({"96.50.0.0/16"}, {"9650:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("96.50.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "96.50.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "9650:1"));

  /* Bring peer3 back -- enters DETACHED_INIT_DUMP for existing group */
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));

  /* Drain init dump queue -- drainPeerQueueCompletely works for reconnection */
  drainPeerQueueCompletely(peerId3);

  /* Peer4 still works after peer3 reconnected */
  injectLocalRoutesAtRuntime({"96.55.0.0/16"}, {"9655:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("96.55.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "96.55.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "9655:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_NE(getPeerState(kPeerAddr3), PeerUpdateState::DOWN);

  XLOGF(INFO, "=== PASSED: RecoveryPeerDown ===");
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
