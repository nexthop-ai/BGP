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

/* E2E tests: PL compression CL override and drain scenarios.
 * Prefix range: 34.1-34.60/16.
 * CL item changes attribute for prefix in detached PL — CL supersedes
 * PL has nullptr attr bucket (withdrawals) — correctly drained
 * PL with 1 prefix per bucket — many small BGP UPDATEs
 * PL drain interleaved with CL consumption — no conflict
 * PL with prefix already withdrawn in CL — CL wins
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Inject a shared route, detach peer3, then update the same prefix with a
 * different community via CL. The CL entry supersedes the detached PL entry
 * for that prefix. Peer4 receives the updated attributes. */
TEST_P(UpdateGroupMultiPeerTest, ClSupersedesPlForSamePrefix) {
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

  /* Inject shared route with community 3401:1 */
  injectLocalRoutesAtRuntime({"34.1.0.0/16"}, {"3401:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3401:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3401:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"34.2.0.0/16"}, {"3402:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.2.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.2.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3402:1"));
  injectLocalRoutesAtRuntime({"34.3.0.0/16"}, {"3403:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.3.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.3.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3403:1"));
  injectLocalRoutesAtRuntime({"34.4.0.0/16"}, {"3404:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.4.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.4.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3404:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* CL supersedes: update the SAME prefix 34.1.0.0/16 with new community */
  injectLocalRoutesAtRuntime({"34.1.0.0/16"}, {"3401:2"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3401:2"));

  /* Verify a fresh route also works post-CL-override */
  injectLocalRoutesAtRuntime({"34.5.0.0/16"}, {"3405:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.5.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.5.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3405:1"));

  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);
  verifySlowPeerInvariants(kPeerAddr4);
}

/* Inject a shared route, detach peer3, then withdraw that route. The PL
 * bucket for that prefix becomes a withdrawal (nullptr attr). CL records
 * the withdrawal. Peer4 receives the withdrawal. Then inject a new route
 * to confirm group continues after nullptr-attr PL entry. */
TEST_P(UpdateGroupMultiPeerTest, PlNullptrAttrBucketWithdrawalDrained) {
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

  /* Inject 2 shared routes before detachment */
  injectLocalRoutesAtRuntime({"34.10.0.0/16"}, {"3410:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.10.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3410:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3410:1"));

  injectLocalRoutesAtRuntime({"34.11.0.0/16"}, {"3411:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.11.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.11.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3411:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.11.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3411:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"34.12.0.0/16"}, {"3412:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.12.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.12.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3412:1"));
  injectLocalRoutesAtRuntime({"34.13.0.0/16"}, {"3413:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.13.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3413:1"));
  injectLocalRoutesAtRuntime({"34.14.0.0/16"}, {"3414:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.14.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.14.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3414:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Withdraw both shared routes — creates nullptr attr PL entries */
  withdrawLocalRoutesAtRuntime({"34.10.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "34.10.0.0", 16, kPeerAddr4));

  withdrawLocalRoutesAtRuntime({"34.11.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "34.11.0.0", 16, kPeerAddr4));

  /* Confirm group continues normally after nullptr-attr PL entries */
  injectLocalRoutesAtRuntime({"34.15.0.0/16"}, {"3415:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.15.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.15.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3415:1"));

  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);
  verifySlowPeerInvariants(kPeerAddr4);
}

/* Inject 4 routes each with DIFFERENT community (4 PL buckets, 1 prefix
 * each). After detachment, update one prefix. CL triggers Case 4 clone for
 * that one. Verifies many-bucket PL works. */
TEST_P(UpdateGroupMultiPeerTest, OnePrefixPerBucketManySmallUpdates) {
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

  /* Inject 4 routes each with different community = 4 PL buckets */
  for (int i = 0; i < 4; ++i) {
    auto prefix = fmt::format("34.{}.0.0/16", 20 + i);
    auto community = fmt::format("{}:1", 3420 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("34.{}.0.0", 20 + i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("34.{}.0.0", 20 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Freq-detach peer3 with different fill routes */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  /* Need > hwm=6 fill routes to trigger queue blocking */
  for (int i = 0; i < 7; ++i) {
    auto prefix = fmt::format("34.{}.0.0/16", 30 + i);
    auto community = fmt::format("{}:1", 3430 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("34.{}.0.0", 30 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Update one prefix from distinct-bucket set */
  injectLocalRoutesAtRuntime({"34.21.0.0/16"}, {"3421:2"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3421:2"));

  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);
  verifySlowPeerInvariants(kPeerAddr4);
}

/* Inject 2 shared routes, detach peer3, then alternate between updating a
 * shared prefix (CL) and injecting a new prefix (also CL). Verifies PL
 * drain and CL consumption interleave without conflict. */
TEST_P(UpdateGroupMultiPeerTest, PlDrainInterleavedWithClConsumption) {
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

  /* Inject 2 shared routes */
  injectLocalRoutesAtRuntime({"34.40.0.0/16"}, {"3440:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.40.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3440:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3440:1"));

  injectLocalRoutesAtRuntime({"34.41.0.0/16"}, {"3441:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.41.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3441:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3441:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"34.42.0.0/16"}, {"3442:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.42.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.42.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3442:1"));
  injectLocalRoutesAtRuntime({"34.43.0.0/16"}, {"3443:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.43.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.43.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3443:1"));
  injectLocalRoutesAtRuntime({"34.44.0.0/16"}, {"3444:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.44.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.44.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3444:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Interleave: update shared prefix, then add new, then update another */
  injectLocalRoutesAtRuntime({"34.40.0.0/16"}, {"3440:2"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3440:2"));

  injectLocalRoutesAtRuntime({"34.45.0.0/16"}, {"3445:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.45.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.45.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3445:1"));

  injectLocalRoutesAtRuntime({"34.41.0.0/16"}, {"3441:2"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3441:2"));

  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);
  verifySlowPeerInvariants(kPeerAddr4);
}

/* Inject a shared route, detach peer3, withdraw that route via CL, then
 * inject it again with different attributes. The withdrawal in CL removes
 * the PL entry. The re-injection is a fresh CL add (Case 2, ribVersion >
 * divergence). Peer4 sees withdrawal then new announcement. */
TEST_P(UpdateGroupMultiPeerTest, PlPrefixWithdrawnInClThenReannounced) {
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

  /* Inject shared route */
  injectLocalRoutesAtRuntime({"34.50.0.0/16"}, {"3450:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.50.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.50.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3450:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.50.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3450:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"34.51.0.0/16"}, {"3451:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.51.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.51.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3451:1"));
  injectLocalRoutesAtRuntime({"34.52.0.0/16"}, {"3452:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.52.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.52.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3452:1"));
  injectLocalRoutesAtRuntime({"34.53.0.0/16"}, {"3453:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.53.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.53.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3453:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Withdraw the shared route — CL wins over PL */
  withdrawLocalRoutesAtRuntime({"34.50.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "34.50.0.0", 16, kPeerAddr4));

  /* Re-announce same prefix with different attributes */
  injectLocalRoutesAtRuntime({"34.50.0.0/16"}, {"3450:2"}, 200);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.50.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.50.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3450:2"));

  /* One more fresh route to confirm continued operation */
  injectLocalRoutesAtRuntime({"34.54.0.0/16"}, {"3454:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("34.54.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "34.54.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3454:1"));

  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);
  verifySlowPeerInvariants(kPeerAddr4);
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
