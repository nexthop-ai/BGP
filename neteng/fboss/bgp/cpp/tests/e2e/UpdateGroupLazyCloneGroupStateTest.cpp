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
 * E2E tests: Lazy Clone x Group State Interactions (Part A)
 * Tests clone behavior with out-delay, READY state, and UNINIT state.
 *
 * Prefix range: 13.50-13.69/16
 * Fixture: UpdateGroupLazyCloneTest
 *
 * Tests: CloneWithOutDelayPending, CloneDuringReadyState,
 * CloneDuringUninitGroupState
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Clone for prefix with out-delay pending -- interact with
 * deferred updates. Both peers receive a shared route, then it's
 * rapidly re-injected (MRAI out-delay). After detaching peer3,
 * update the same prefix again -- clone fires correctly.
 */
TEST_P(UpdateGroupLazyCloneTest, CloneWithOutDelayPending) {
  XLOG(INFO, "=== TEST: CloneWithOutDelayPending ===");

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

  /* Inject shared route -- both peers receive */
  injectLocalRoutesAtRuntime({"13.50.0.0/16"}, {"1350:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.50.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.50.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1350:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.50.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1350:1"));

  /* Rapid re-inject same prefix -- creates out-delay scenario */
  injectLocalRoutesAtRuntime({"13.50.0.0/16"}, {"1350:2"}, 160);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.50.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.50.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1350:2"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.50.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1350:2"));

  /* Detach peer3 via freq threshold=1 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"13.51.0.0/16"}, {"1351:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.51.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.51.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1351:1"));
  injectLocalRoutesAtRuntime({"13.52.0.0/16"}, {"1352:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.52.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.52.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1352:1"));
  injectLocalRoutesAtRuntime({"13.53.0.0/16"}, {"1353:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.53.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.53.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1353:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Clone fires for the prefix that had prior out-delay activity */
  injectLocalRoutesAtRuntime({"13.50.0.0/16"}, {"1350:99"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("13.50.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "13.50.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1350:99"));

  verifySlowPeerInvariants(kPeerAddr3);
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_FALSE(isPeerInSync(kPeerAddr3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  XLOG(INFO, "=== TEST PASSED: CloneWithOutDelayPending ===");
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
