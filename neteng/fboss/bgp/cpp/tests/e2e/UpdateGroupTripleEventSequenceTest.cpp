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
 * E2E tests: Triple event sequences -- complex multi-step scenarios combining
 * detach, policy change, unblock, accept, duration, and attribute mutations.
 * Prefix range: 59.x.0.0/16
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Multiple attribute changes during detachment.
 * Detach peer3, then perform 3 withdraw+re-inject cycles on different prefixes
 * (simulating attribute mutations). Only the final state matters for CL
 * recovery. Peer4 receives all changes inline; peer3's CL accumulates them.
 */
TEST_P(UpdateGroupMultiPeerTest, MultipleAttrChangesDuringDetach) {
  XLOG(INFO, "=== TEST: MultipleAttrChangesDuringDetach ===");

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

  /* Inject 3 baseline routes -- both peers receive */
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("59.{}.0.0/16", 50 + i);
    auto c = fmt::format("{}:1", 5950 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("59.{}.0.0", 50 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        c));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("59.{}.0.0", 50 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
  }

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto p = fmt::format("59.{}.0.0/16", 55 + i);
    auto c = fmt::format("{}:1", 5955 + i);
    injectLocalRoutesAtRuntime({p}, {c}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(p)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("59.{}.0.0", 55 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        c));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /*
   * 3 rounds of withdraw+re-inject with DIFFERENT replacement prefixes.
   * Each round: withdraw one baseline prefix, inject a new one.
   * Peer4 receives each event inline; peer3's CL accumulates all of them.
   * Only the final CL state matters for recovery.
   */
  for (int round = 0; round < 3; round++) {
    auto wdPrefix = fmt::format("59.{}.0.0/16", 50 + round);
    withdrawLocalRoutesAtRuntime({wdPrefix});
    EXPECT_TRUE(verifyRouteWithdraw(
        "v4", fmt::format("59.{}.0.0", 50 + round), 16, kPeerAddr4));

    auto newPrefix = fmt::format("59.{}.0.0/16", 60 + round);
    auto newComm = fmt::format("{}:1", 5960 + round);
    injectLocalRoutesAtRuntime({newPrefix}, {newComm}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(newPrefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("59.{}.0.0", 60 + round),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        newComm));
  }

  /* Unblock peer3 -- starts CL recovery with accumulated items */
  unblockPeer(kPeerAddr3);

  /* Drain-while-waiting pattern: peer3 has ~9 CL items to consume but
   * a small (size=3) queue, so the CL consumer backpressures once the
   * queue fills. Actively drain peer3's queue until it reaches JR.
   * 100ms sleep aligns with the CL consume timer (200ms). */
  for (int i = 0; i < 40; ++i) {
    if (getPeerState(kPeerAddr3) == PeerUpdateState::JOINED_RUNNING) {
      break;
    }
    drainPeerQueueCompletely(peerId3, 1, 100);
    peerManager_->getEventBase().runInEventBaseThreadAndWait([]() {});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  /* Final drain clears leftover CL messages so verifyRouteAdd below
   * reads the fresh post-rejoin 59.70 route (not stale CL output). */
  drainPeerQueueCompletely(peerId3, 1, 100);
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING, 50));

  /* Verify BOTH peers receive the post-recovery route (peer3 must see it
   * too — reviewer asked us to also check the previously-detached peer,
   * not just the sync peer). */
  injectLocalRoutesAtRuntime({"59.70.0.0/16"}, {"5970:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("59.70.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "59.70.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "5970:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "59.70.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "5970:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr3), 0);
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupMultiPeerTest,
    ::testing::Values(kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
