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
 * E2E tests: Lazy Clone Case 4 (clone needed) and Edge Cases
 *
 * Prefix range: 81.60-81.120/16
 * Fixture: UpdateGroupLazyCloneTest
 *
 * Tests:
 *   Case 4 clone preserves correct attribute values
 *   Case 4 clone uses forPeer() owner key
 *   Two diverged peers both need Case 4 clone
 *   Two diverged peers: one needs clone, other doesn't
 *   Three diverged peers with different divergenceRibVersions
 *   Diverged peer goes DOWN during clone -- graceful skip
 *   Clone for max diverged peers (E2E: 2 diverged + 1 in-sync)
 *   Clone fires, per-peer entry exists from prior clone (Case 1)
 *   Withdraw -> Clone -> Re-announce same prefix
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Two diverged peers: one needs clone (Case 4), other doesn't
 * (Case 2 - prefix injected post-detach of peer4).
 */
TEST_P(UpdateGroupLazyCloneTest, TwoPeersOneCloneOneNoClone) {
  XLOGF(INFO, "=== TEST: TwoPeersOneCloneOneNoClone ===");

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

  /* Shared route -- all 3 peers receive it */
  injectLocalRoutesAtRuntime({"81.85.0.0/16"}, {"8185:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("81.85.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "81.85.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "8185:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "81.85.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "8185:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "81.85.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "8185:1"));

  /* Detach peer3 first */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto pfx = fmt::format("81.{}.0.0/16", 86 + i);
    auto comm = fmt::format("81{}:1", 86 + i);
    injectLocalRoutesAtRuntime({pfx}, {comm}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(pfx)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("81.{}.0.0", 86 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        comm));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("81.{}.0.0", 86 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        comm));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Inject a new prefix AFTER peer3 detached but BEFORE peer4 detaches */
  injectLocalRoutesAtRuntime({"81.90.0.0/16"}, {"8190:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("81.90.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "81.90.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "8190:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "81.90.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "8190:1"));

  /* Raise threshold then detach peer4 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      999999,
      std::chrono::milliseconds(60000));
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr4);
  for (int i = 0; i < 3; i++) {
    auto pfx = fmt::format("81.{}.0.0/16", 91 + i);
    auto comm = fmt::format("81{}:1", 91 + i);
    injectLocalRoutesAtRuntime({pfx}, {comm}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(pfx)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("81.{}.0.0", 91 + i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        comm));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));

  /* Update 81.90 -- peer3 needs Case 4 clone (saw it pre-detach),
   * peer4 Case 2/3 (ribVersion > its divergenceRibVersion) */
  injectLocalRoutesAtRuntime({"81.90.0.0/16"}, {"8190:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("81.90.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "81.90.0.0",
      16,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "8190:99"));

  verifySlowPeerInvariants(kPeerAddr3);
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_TRUE(isPeerDetached(kPeerAddr4));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));

  XLOGF(INFO, "=== TEST PASSED: TwoPeersOneCloneOneNoClone ===");
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
