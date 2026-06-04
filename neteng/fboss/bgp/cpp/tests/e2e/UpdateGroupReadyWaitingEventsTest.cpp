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
 * E2E tests: G-READY and G-WAITING state x Event coverage.
 *
 * Prefix range: 76.x.0.0/16
 *
 * Tests:
 *   G-READY x E-ROUTE-WD - Withdrawal during READY, batched
 *   G-READY x E-UNBLOCK - N/A (no blocked peers in READY)
 *   G-READY x E-SLOW-FREQ - Frequency detach during READY
 *   G-READY x E-PEER-UP - New peer during READY init dump
 *   G-READY x E-CL-END - N/A (CL has pending items)
 *   G-READY x E-ROUTE-REFRESH - Route refresh during READY
 *   G-READY x E-MULTI-ROUTE - Batch during READY, CL grows
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {
/*
 * G-READY x E-UNBLOCK - N/A.
 * No blocked peers in READY state. unblockPeer is a no-op.
 * Verify no crash and continued route delivery.
 */
TEST_P(UpdateGroupMultiPeerTest, GReady_UnblockNoop) {
  XLOG(INFO, "=== TEST: GReady_UnblockNoop ===");

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

  /* unblockPeer on non-blocked peer is a no-op */
  EXPECT_FALSE(isPeerQueueBlocked(peerId3));
  unblockPeer(kPeerAddr3);

  /* Verify state unchanged */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Confirm route delivery still works */
  injectLocalRoutesAtRuntime({"76.5.0.0/16"}, {"7605:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("76.5.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "76.5.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "7605:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "76.5.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "7605:1"));

  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  verifySlowPeerInvariants(kPeerAddr3);

  XLOG(INFO, "=== TEST PASSED: GReady_UnblockNoop ===");
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
