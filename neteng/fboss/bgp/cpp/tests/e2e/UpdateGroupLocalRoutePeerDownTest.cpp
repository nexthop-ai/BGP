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
 * Parameterized E2E tests for BGP Update Group - Part 2
 * Tests are parameterized by:
 *   - IP version (v4/v6)
 *   - Serialization mode (enableSerializeGroupPdu true/false)
 *
 * This file contains: Peer lifecycle tests (go down/come back up)
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupTestUtils.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Test fixture inheriting from the shared UpdateGroupTestBase.
 */
class UpdateGroupPeerDownTest : public UpdateGroupTestBase {};

/*
 * =============================================================================
 * TEST CASES - Peer lifecycle tests (go down/come back up)
 * =============================================================================
 */

/*
 * Test: All peers block, go down, come back up
 */
TEST_P(UpdateGroupPeerDownTest, AllPeersBlockGoDownComeUp_Add) {
  SCOPED_TRACE("Protocol: " + params().name);
  XLOGF(
      INFO, "=== TEST: AllPeersBlockGoDownComeUp_Add ({}) ===", params().name);

  setDefaultQueueSizes(3, 2, 0);
  addPeer(getPeerSpec3());
  addPeer(getPeerSpec4());
  setupComponents();

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));

  /* Add 7 routes - both peers should block */
  auto routePrefixes = prefixes(0, 7);
  injectRoutes(routePrefixes, 400);

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  XLOG(INFO, "Both peers blocked");

  /* Bring both peers down while blocked */
  bringDownPeer(kPeerAddr3);
  bringDownPeer(kPeerAddr4);
  XLOG(INFO, "Both peers down while blocked");

  /* Bring both back up */
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  XLOG(INFO, "Both peers back up");

  /* Both should get full dump from shadowRIB */
  EXPECT_TRUE(
      verifyRoutesWithFix(kPeerAddr3, makeRouteSpecs(routePrefixes, 400)));
  EXPECT_TRUE(
      verifyRoutesWithFix(kPeerAddr4, makeRouteSpecs(routePrefixes, 400)));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));

  XLOG(INFO, "Both peers received all routes after recovery");
}

/*
 * Test: Peer blocks, goes down (other peer stays up)
 */
TEST_P(UpdateGroupPeerDownTest, PeerBlockGoesDown_Add) {
  SCOPED_TRACE("Protocol: " + params().name);
  XLOGF(INFO, "=== TEST: PeerBlockGoesDown_Add ({}) ===", params().name);

  setDefaultQueueSizes(3, 2, 0);
  addPeer(getPeerSpec3());
  addPeer(getPeerSpec4());
  setupComponents();

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));

  /* Add 4 routes to cause blocking */
  auto routePrefixes = prefixes(0, 4);
  injectRoutes(routePrefixes, 500);

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  XLOG(INFO, "Both peers blocked");

  /* Bring peer3 down while blocked */
  bringDownPeer(kPeerAddr3);
  XLOG(INFO, "Peer3 down while blocked");

  /* Peer4 should get unblocked and receive routes */
  EXPECT_TRUE(
      verifyRoutesWithFix(kPeerAddr4, makeRouteSpecs(routePrefixes, 500)));
  EXPECT_FALSE(isPeerQueueBlocked(peerId4));
  XLOG(INFO, "Peer4 received all routes after peer3 went down");
}

/*
 * Test: Peer blocks, goes down, comes back
 */
TEST_P(UpdateGroupPeerDownTest, PeerBlockGoesDownComesBack_Add) {
  SCOPED_TRACE("Protocol: " + params().name);
  XLOGF(
      INFO, "=== TEST: PeerBlockGoesDownComesBack_Add ({}) ===", params().name);

  setDefaultQueueSizes(3, 2, 0);
  addPeer(getPeerSpec3());
  addPeer(getPeerSpec4());
  setupComponents();

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));

  /* Add 6 routes BEFORE peer goes down */
  auto routePrefixes = prefixes(0, 6);
  injectRoutes(routePrefixes, 600);

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));

  /* Bring peer3 down while blocked */
  bringDownPeer(kPeerAddr3);
  XLOG(INFO, "Peer3 down while blocked");

  /* Peer4 receives all routes */
  EXPECT_TRUE(
      verifyRoutesWithFix(kPeerAddr4, makeRouteSpecs(routePrefixes, 600)));

  /* Bring peer3 back up */
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  XLOG(INFO, "Peer3 back up");

  /* Peer3 should get full dump from shadowRIB */
  EXPECT_TRUE(
      verifyRoutesWithFix(kPeerAddr3, makeRouteSpecs(routePrefixes, 600)));
  EXPECT_TRUE(waitForEoR(peerId3));
  XLOG(INFO, "Peer3 received all routes after recovery");
}

INSTANTIATE_UPDATE_GROUP_TESTS(UpdateGroupPeerDownTest);

} // namespace bgp
} // namespace facebook
