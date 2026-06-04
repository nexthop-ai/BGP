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
 * E2E tests: Lazy Clone — PathId and Radix Tree Edge Cases
 *
 * Prefix range: 30.30-30.49/16
 * Fixture: UpdateGroupLazyCloneTest
 *
 * Tests:
 *   Clone when radix tree has no subtree — edge case
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {
/*
 * Clone when radix tree has no subtree for prefix — edge case.
 *
 * Tests clone on a /32 host route (leaf node in trie) and a /16 route
 * at a different trie depth. Both get independent Case 4 clones.
 * Exercises the radix tree lookup at the deepest possible prefix
 * length where no subtree exists below.
 */
TEST_P(UpdateGroupLazyCloneTest, CloneRadixTreeNoSubtreeEdgeCase) {
  XLOG(INFO, "=== TEST: CloneRadixTreeNoSubtreeEdgeCase ===");

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

  /* Both peers receive a /16 and a sparse /32 host route */
  injectLocalRoutesAtRuntime({"30.40.0.0/16"}, {"3040:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.40.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3040:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3040:1"));

  injectLocalRoutesAtRuntime({"30.41.255.1/32"}, {"3041:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("30.41.255.1/32")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.41.255.1",
      32,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3041:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.41.255.1",
      32,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3041:1"));

  /* Detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"30.42.0.0/16"}, {"3042:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.42.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.42.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3042:1"));
  injectLocalRoutesAtRuntime({"30.43.0.0/16"}, {"3043:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.43.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.43.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3043:1"));
  injectLocalRoutesAtRuntime({"30.44.0.0/16"}, {"3044:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.44.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.44.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3044:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Update the /32 host route — Case 4 clone on sparse trie leaf */
  injectLocalRoutesAtRuntime({"30.41.255.1/32"}, {"3041:99"}, 250);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("30.41.255.1/32")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.41.255.1",
      32,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3041:99"));
  verifySlowPeerInvariants(kPeerAddr3);

  /* Also update the /16 — different trie depth, independent clone */
  injectLocalRoutesAtRuntime({"30.40.0.0/16"}, {"3040:99"}, 250);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3040:99"));
  verifySlowPeerInvariants(kPeerAddr3);

  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  XLOG(INFO, "=== TEST PASSED: CloneRadixTreeNoSubtreeEdgeCase ===");
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
