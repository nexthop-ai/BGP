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

/* E2E tests: PL compression withdrawal, drain order, and empty PL.
 * Prefix range: 33.20-33.64/16.
 * Withdrawal removes prefix from PL bucket (no stale entry)
 * PL drain order — withdrawals before announcements
 * Empty PL clone — detachedPackingList has no entries
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/* Inject a route, detach peer3, then withdraw that route. The withdrawal
 * removes the prefix from the PL bucket. Peer4 gets the withdrawal. CL
 * records Case 4 clone then withdrawal for peer3. */
TEST_P(UpdateGroupMultiPeerTest, WithdrawalRemovesPrefixFromPlBucket) {
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

  /* Inject shared route before detachment */
  injectLocalRoutesAtRuntime({"33.20.0.0/16"}, {"3320:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.20.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3320:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3320:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"33.21.0.0/16"}, {"3321:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3321:1"));
  injectLocalRoutesAtRuntime({"33.22.0.0/16"}, {"3322:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.22.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.22.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3322:1"));
  injectLocalRoutesAtRuntime({"33.23.0.0/16"}, {"3323:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.23.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.23.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3323:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Withdraw the shared route — peer4 gets withdrawal */
  withdrawLocalRoutesAtRuntime({"33.20.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "33.20.0.0", 16, kPeerAddr4));

  /* Inject a new route to confirm group continues normally */
  injectLocalRoutesAtRuntime({"33.24.0.0/16"}, {"3324:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.24.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.24.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3324:1"));

  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);
  verifySlowPeerInvariants(kPeerAddr4);
}

/* Inject 2 shared routes, detach peer3, then withdraw one and add a new
 * one. CL records both withdrawal and announcement. Peer4 receives both
 * correctly. Confirms mixed drain order works. */
TEST_P(UpdateGroupMultiPeerTest, PlDrainWithdrawalsAndAnnouncements) {
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
  injectLocalRoutesAtRuntime({"33.50.0.0/16"}, {"3350:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.50.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.50.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3350:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.50.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3350:1"));

  injectLocalRoutesAtRuntime({"33.51.0.0/16"}, {"3351:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.51.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.51.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3351:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.51.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3351:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"33.52.0.0/16"}, {"3352:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.52.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.52.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3352:1"));
  injectLocalRoutesAtRuntime({"33.53.0.0/16"}, {"3353:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.53.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.53.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3353:1"));
  injectLocalRoutesAtRuntime({"33.54.0.0/16"}, {"3354:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.54.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.54.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3354:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Withdraw one shared route */
  withdrawLocalRoutesAtRuntime({"33.50.0.0/16"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "33.50.0.0", 16, kPeerAddr4));

  /* Add a new route — peer4 gets announcement */
  injectLocalRoutesAtRuntime({"33.55.0.0/16"}, {"3355:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.55.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.55.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3355:1"));

  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 1);
  verifySlowPeerInvariants(kPeerAddr4);
}

/* Empty PL clone — detach a peer when no routes exist (only init dump +
 * EoRs). detachedPackingList has no route entries. Post-detach route
 * additions go through CL normally (Case 2 — new prefix, ribVersion=0).
 * Peer4 receives everything. */
TEST_P(UpdateGroupMultiPeerTest, EmptyPlCloneNoRouteEntries) {
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

  /* No shared routes — detach peer3 with empty PL */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  /* Fill routes with different communities to trigger blocking */
  injectLocalRoutesAtRuntime({"33.60.0.0/16"}, {"3360:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.60.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.60.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3360:1"));
  injectLocalRoutesAtRuntime({"33.61.0.0/16"}, {"3361:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.61.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.61.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3361:1"));
  injectLocalRoutesAtRuntime({"33.62.0.0/16"}, {"3362:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.62.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.62.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3362:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Post-detach: new routes are Case 2 (ribVersion=0, no clone) */
  injectLocalRoutesAtRuntime({"33.63.0.0/16"}, {"3363:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.63.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.63.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3363:1"));

  injectLocalRoutesAtRuntime({"33.64.0.0/16"}, {"3364:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("33.64.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "33.64.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3364:1"));

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
