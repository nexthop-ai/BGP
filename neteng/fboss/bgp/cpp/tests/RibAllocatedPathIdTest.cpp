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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#define AdjRib_TEST_FRIENDS                                                    \
  FRIEND_TEST(                                                                 \
      RibAllocatedPathIdTestFixture, ProcessShadowRibEntryChangeAnnounceTest); \
  FRIEND_TEST(                                                                 \
      RibAllocatedPathIdTestFixture, ProcessShadowRibEntryChangeWithdrawTest); \
  FRIEND_TEST(                                                                 \
      RibAllocatedPathIdTestFixture,                                           \
      ProcessShadowRibEntryChangeAnnounceAddPathTest);                         \
  FRIEND_TEST(                                                                 \
      RibAllocatedPathIdTestFixture,                                           \
      ProcessShadowRibEntryChangeWithdrawAddPathTest);                         \
  FRIEND_TEST(RibAllocatedPathIdTestFixture, ProcessRibAnnouncedEntryTest);    \
  FRIEND_TEST(RibAllocatedPathIdTestFixture, ProcessRibOutWithdrawalTest);

#include <folly/coro/BlockingWait.h>
#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/tests/AdjRibOutUtils.h"

namespace facebook::bgp {

// Parameterize tests to toggle rib-allocated path ID on and off.
class RibAllocatedPathIdTestFixture : public AdjRibOutboundFixture,
                                      public testing::WithParamInterface<bool> {
};

INSTANTIATE_TEST_SUITE_P(
    RibAllocatedPathIdTestFixture,
    RibAllocatedPathIdTestFixture,
    testing::Bool() /* ribAllocatedPathId */);

// TODO: all 4 of these need to be parameterized and run for both rib-allocated
// path ID enabled and disabled

TEST_P(RibAllocatedPathIdTestFixture, ProcessShadowRibEntryChangeAnnounceTest) {
  setupAdjRib(
      kLocalAs1, /* globalAs */
      kLocalAs1, /* localAs */
      kRemoteAs1, /* remoteAs */
      true, /* isRrClient */
      false, /* isConfedPeer */
      false, /* nexthopSelf */
      kV4Nexthop1, /* v4Nexthop */
      kV6Nexthop1, /* v6Nexthop */
      false /* call sessionEstablished */);
  adjRib_->pathIdGenerator_ = std::make_unique<PathIdGenerator>(false);
  adjRib_->egressEoRsSent_ = true;
  adjRib_->isAfiIpv4Negotiated_ = true;
  adjRib_->isAfiIpv6Negotiated_ = true;
  adjRib_->enableRibAllocatedPathId_ = GetParam();
  auto attrs1 =
      std::make_shared<facebook::bgp::BgpPath>(*buildBgpPathFields(1, 4, 4, 4));
  attrs1->publish();
  auto path1 =
      std::make_shared<ShadowRibRouteInfo>(eBgpPeer_, attrs1, kMaxPathIDToSend);
  setShadowRibRouteState(path1, SHADOWRIBROUTE_IN_UPDATE);
  ShadowRibOutAnnouncementEntry srEntry(kV4Prefix1, path1, {});

  adjRib_->processShadowRibEntryChange(srEntry);

  // whether feature is enabled or not, we should have one adjRibEntry with
  // pathID 0 and the expected attrs
  ASSERT_EQ(
      adjRib_->adjRibOutGroup_->getPeerEntriesCountFromLiteTree(
          adjRib_->adjRibOutGroup_->LiteTree_, adjRib_->getPeerOwnerKey()),
      1);
  auto adjRibEntry = adjRib_->adjRibOutGroup_->getFromLiteTree(
      adjRib_->adjRibOutGroup_->LiteTree_,
      kV4Prefix1,
      adjRib_->getPeerOwnerKey());
  ASSERT_NE(adjRibEntry, nullptr);
  EXPECT_EQ(adjRibEntry->getPathId(), kDefaultPathID);
  EXPECT_EQ(adjRibEntry->getPreOut(), attrs1);
}

TEST_P(RibAllocatedPathIdTestFixture, ProcessShadowRibEntryChangeWithdrawTest) {
  setupAdjRib(
      kLocalAs1, /* globalAs */
      kLocalAs1, /* localAs */
      kRemoteAs2, /* remoteAs */
      false, /* isRrClient */
      false, /* isConfedPeer */
      false, /* nexthopSelf */
      kV4Nexthop1, /* v4Nexthop */
      kV6Nexthop1, /* v6Nexthop */
      false /* call sessionEstablished */);
  adjRib_->pathIdGenerator_ = std::make_unique<PathIdGenerator>(false);
  adjRib_->egressEoRsSent_ = true;
  adjRib_->isAfiIpv4Negotiated_ = true;
  adjRib_->isAfiIpv6Negotiated_ = true;
  adjRib_->enableRibAllocatedPathId_ = GetParam();
  auto attrs1 =
      std::make_shared<facebook::bgp::BgpPath>(*buildBgpPathFields(1, 4, 4, 4));
  attrs1->publish();
  auto path1 =
      std::make_shared<ShadowRibRouteInfo>(eBgpPeer_, attrs1, kMaxPathIDToSend);
  setShadowRibRouteState(path1, SHADOWRIBROUTE_IN_WITHDRAW);
  ShadowRibOutAnnouncementEntry srEntry(kV4Prefix1, path1, {});
  // need to add adjRibEntry w/ path ID 0 to make sure it gets withdrawn
  adjRib_->adjRibOutGroup_->addToLiteTree(
      adjRib_->adjRibOutGroup_->LiteTree_,
      kV4Prefix1,
      adjRib_->getPeerOwnerKey(),
      kDefaultPathID);
  ASSERT_EQ(
      adjRib_->adjRibOutGroup_->getPeerEntriesCountFromLiteTree(
          adjRib_->adjRibOutGroup_->LiteTree_, adjRib_->getPeerOwnerKey()),
      1);
  auto adjRibEntry = adjRib_->adjRibOutGroup_->getFromLiteTree(
      adjRib_->adjRibOutGroup_->LiteTree_,
      kV4Prefix1,
      adjRib_->getPeerOwnerKey());
  adjRibEntry->setPreOut(attrs1);
  ASSERT_NE(adjRibEntry, nullptr);
  EXPECT_EQ(adjRibEntry->getPathId(), kDefaultPathID);
  EXPECT_EQ(adjRibEntry->getPreOut(), attrs1);

  adjRib_->processShadowRibEntryChange(srEntry);

  // whether feature is enabled or not, we should see that the corresponding
  // adjRibEntry is withdrawn
  ASSERT_EQ(
      adjRib_->adjRibOutGroup_->getPeerEntriesCountFromLiteTree(
          adjRib_->adjRibOutGroup_->LiteTree_, adjRib_->getPeerOwnerKey()),
      0);
}

TEST_P(
    RibAllocatedPathIdTestFixture,
    ProcessShadowRibEntryChangeAnnounceAddPathTest) {
  setupAdjRib(
      kLocalAs1, /* globalAs */
      kLocalAs1, /* localAs */
      kRemoteAs2, /* remoteAs */
      false, /* isRrClient */
      false, /* isConfedPeer */
      false, /* nexthopSelf */
      kV4Nexthop1, /* v4Nexthop */
      kV6Nexthop1, /* v6Nexthop */
      false /* call sessionEstablished */);
  adjRib_->pathIdGenerator_ = std::make_unique<PathIdGenerator>(false);
  adjRib_->sendAddPath_ = true;
  adjRib_->egressEoRsSent_ = true;
  adjRib_->isAfiIpv4Negotiated_ = true;
  adjRib_->isAfiIpv6Negotiated_ = true;
  adjRib_->enableRibAllocatedPathId_ = GetParam();
  auto attrs1 =
      std::make_shared<facebook::bgp::BgpPath>(*buildBgpPathFields(1, 4, 4, 4));
  attrs1->publish();
  auto path2 =
      std::make_shared<ShadowRibRouteInfo>(eBgpPeer_, attrs1, kMaxPathIDToSend);
  setShadowRibRouteState(path2, SHADOWRIBROUTE_IN_UPDATE);
  ShadowRibRouteInfos mp{{uint32_t(0), path2}};
  ShadowRibOutAnnouncementEntry srEntry2(kV4Prefix1, nullptr, mp);

  adjRib_->processShadowRibEntryChange(
      srEntry2); // with feature disabled, adjRibEntry w/ path ID 0-n are
                 // updated

  // we should have one adjRibEntry for the updated SR multipath. If
  // rib-allocated path ID is enabled, then the pathIdToSend value from the SR
  // entry should get used for the adjRibEntry (kMaxPathIDToSend in this case).
  // Otherwise, the path ID would be whatever the pathIDGenerator_ functionality
  // provides (kDefaultPathID, in this case)
  ASSERT_EQ(
      adjRib_->adjRibOutGroup_->getPeerEntriesCountFromPathTree(
          adjRib_->adjRibOutGroup_->PathTree_, adjRib_->getPeerOwnerKey()),
      1);
  // for non-rib-allocated case, verify the pathIDGenerator chosen path ID
  // directly. Should be 0, but whatever it is we just want to make sure it's
  // use for adjRibEntry
  auto adjRibPathId =
      adjRib_->pathIdGenerator_->getPathId(kV4Prefix1, kV4Nexthop1);
  auto pathId = GetParam() ? kMaxPathIDToSend : adjRibPathId;
  auto adjRibEntry = adjRib_->adjRibOutGroup_->getFromPathTree(
      adjRib_->adjRibOutGroup_->PathTree_,
      kV4Prefix1,
      adjRib_->getPeerOwnerKey(),
      pathId);
  ASSERT_NE(adjRibEntry, nullptr);
  EXPECT_EQ(adjRibEntry->getPathId(), pathId);
  EXPECT_EQ(adjRibEntry->getPreOut(), attrs1);
  // need one invocation for rib-allocated path ID enabled and one for disabled.
}

TEST_P(
    RibAllocatedPathIdTestFixture,
    ProcessShadowRibEntryChangeWithdrawAddPathTest) {
  setupAdjRib(
      kLocalAs1, /* globalAs */
      kLocalAs1, /* localAs */
      kRemoteAs2, /* remoteAs */
      false, /* isRrClient */
      false, /* isConfedPeer */
      false, /* nexthopSelf */
      kV4Nexthop1, /* v4Nexthop */
      kV6Nexthop1, /* v6Nexthop */
      false /* call sessionEstablished */);
  adjRib_->pathIdGenerator_ = std::make_unique<PathIdGenerator>(false);
  adjRib_->sendAddPath_ = true;
  adjRib_->egressEoRsSent_ = true;
  adjRib_->isAfiIpv4Negotiated_ = true;
  adjRib_->isAfiIpv6Negotiated_ = true;
  adjRib_->enableRibAllocatedPathId_ = GetParam();
  auto attrs1 =
      std::make_shared<facebook::bgp::BgpPath>(*buildBgpPathFields(1, 4, 4, 4));
  attrs1->publish();
  auto path2 = std::make_shared<ShadowRibRouteInfo>(
      localPeerV4_, attrs1, kMaxPathIDToSend);
  setShadowRibRouteState(path2, SHADOWRIBROUTE_IN_WITHDRAW);
  ShadowRibRouteInfos mp{{uint32_t(0), path2}};
  ShadowRibOutAnnouncementEntry srEntry2(kV4Prefix1, nullptr, mp);
  // for non-rib-allocated case, verify the pathIDGenerator chosen path ID
  // directly. Should be 0, but whatever it is we just want to make sure it's
  // use for adjRibEntry
  auto adjRibPathId =
      adjRib_->pathIdGenerator_->getPathId(kV4Prefix1, kV4Nexthop1);
  auto pathId = GetParam() ? kMaxPathIDToSend : adjRibPathId;

  adjRib_->adjRibOutGroup_->addToPathTree(
      adjRib_->adjRibOutGroup_->PathTree_,
      kV4Prefix1,
      adjRib_->getPeerOwnerKey(),
      pathId);
  ASSERT_EQ(
      adjRib_->adjRibOutGroup_->getPeerEntriesCountFromPathTree(
          adjRib_->adjRibOutGroup_->PathTree_, adjRib_->getPeerOwnerKey()),
      1);
  auto adjRibEntry = adjRib_->adjRibOutGroup_->getFromPathTree(
      adjRib_->adjRibOutGroup_->PathTree_,
      kV4Prefix1,
      adjRib_->getPeerOwnerKey(),
      pathId);
  adjRibEntry->setPreOut(attrs1);
  ASSERT_NE(adjRibEntry, nullptr);
  EXPECT_EQ(adjRibEntry->getPathId(), pathId);
  EXPECT_EQ(adjRibEntry->getPreOut(), attrs1);

  adjRib_->processShadowRibEntryChange(
      srEntry2); // with feature disabled, adjRibEntry w/ path ID 0-n are
                 // updated

  // whether feature is enabled or not, we should see that the corresponding
  // adjRibEntry is withdrawn
  ASSERT_EQ(
      adjRib_->adjRibOutGroup_->getPeerEntriesCountFromLiteTree(
          adjRib_->adjRibOutGroup_->LiteTree_, adjRib_->getPeerOwnerKey()),
      0);
}

TEST_P(RibAllocatedPathIdTestFixture, ProcessRibAnnouncedEntryTest) {
  setupAdjRib(
      kLocalAs1, /* globalAs */
      kLocalAs1, /* localAs */
      kRemoteAs1, /* remoteAs */
      true, /* isRrClient */
      false, /* isConfedPeer */
      false, /* nexthopSelf */
      kV4Nexthop1, /* v4Nexthop */
      kV6Nexthop1, /* v6Nexthop */
      false /* call sessionEstablished */);
  adjRib_->pathIdGenerator_ = std::make_unique<PathIdGenerator>(false);
  adjRib_->egressEoRsSent_ = true;
  adjRib_->isAfiIpv4Negotiated_ = true;
  adjRib_->isAfiIpv6Negotiated_ = true;
  adjRib_->enableRibAllocatedPathId_ = GetParam();
  auto attrs1 =
      std::make_shared<facebook::bgp::BgpPath>(*buildBgpPathFields(1, 4, 4, 4));
  attrs1->publish();
  auto dummyId = 12;
  RibOutAnnouncementEntry ann(kV4Prefix1, dummyId, eBgpPeer_, attrs1);

  adjRib_->processRibAnnouncedEntry(ann);

  // if feature is enabled, we get the ID that was on RibOutAnnouncementEntry.
  // Otherwise, we get pathIdGenerator_'s value for getPathId
  ASSERT_EQ(
      adjRib_->adjRibOutGroup_->getPeerEntriesCountFromLiteTree(
          adjRib_->adjRibOutGroup_->LiteTree_, adjRib_->getPeerOwnerKey()),
      1);
  auto adjRibEntry = adjRib_->adjRibOutGroup_->getFromLiteTree(
      adjRib_->adjRibOutGroup_->LiteTree_,
      kV4Prefix1,
      adjRib_->getPeerOwnerKey());
  ASSERT_NE(adjRibEntry, nullptr);
  EXPECT_EQ(
      adjRibEntry->getPathId(),
      GetParam()
          ? dummyId
          : adjRib_->pathIdGenerator_->getPathId(kV4Prefix1, kV4Nexthop1));
  EXPECT_EQ(adjRibEntry->getPreOut(), attrs1);
}

TEST_P(RibAllocatedPathIdTestFixture, ProcessRibOutWithdrawalTest) {
  setupAdjRib(
      kLocalAs1, /* globalAs */
      kLocalAs1, /* localAs */
      kRemoteAs2, /* remoteAs */
      false, /* isRrClient */
      false, /* isConfedPeer */
      false, /* nexthopSelf */
      kV4Nexthop1, /* v4Nexthop */
      kV6Nexthop1, /* v6Nexthop */
      false /* call sessionEstablished */);
  adjRib_->pathIdGenerator_ = std::make_unique<PathIdGenerator>(false);
  adjRib_->egressEoRsSent_ = true;
  adjRib_->isAfiIpv4Negotiated_ = true;
  adjRib_->isAfiIpv6Negotiated_ = true;
  adjRib_->enableRibAllocatedPathId_ = GetParam();
  auto attrs1 =
      std::make_shared<facebook::bgp::BgpPath>(*buildBgpPathFields(1, 4, 4, 4));
  attrs1->publish();
  auto pathId = GetParam()
      ? 12
      : adjRib_->pathIdGenerator_->getPathId(kV4Prefix1, kV4Nexthop1);
  RibOutWithdrawalEntry withEntry(kV4Prefix1, pathId, kV4Nexthop1);
  RibOutWithdrawal with({withEntry}, {});

  // need to add adjRibEntry w/ pathID based on feature to make sure it gets
  // withdrawn
  adjRib_->adjRibOutGroup_->addToLiteTree(
      adjRib_->adjRibOutGroup_->LiteTree_,
      kV4Prefix1,
      adjRib_->getPeerOwnerKey(),
      pathId);
  ASSERT_EQ(
      adjRib_->adjRibOutGroup_->getPeerEntriesCountFromLiteTree(
          adjRib_->adjRibOutGroup_->LiteTree_, adjRib_->getPeerOwnerKey()),
      1);
  auto adjRibEntry = adjRib_->adjRibOutGroup_->getFromLiteTree(
      adjRib_->adjRibOutGroup_->LiteTree_,
      kV4Prefix1,
      adjRib_->getPeerOwnerKey());
  adjRibEntry->setPreOut(attrs1);
  ASSERT_NE(adjRibEntry, nullptr);
  EXPECT_EQ(adjRibEntry->getPathId(), pathId);
  EXPECT_EQ(adjRibEntry->getPreOut(), attrs1);

  adjRib_->processRibOutWithdrawal(with);

  // whether feature is enabled or not, we should see that the corresponding
  // adjRibEntry is withdrawn
  ASSERT_EQ(
      adjRib_->adjRibOutGroup_->getPeerEntriesCountFromLiteTree(
          adjRib_->adjRibOutGroup_->LiteTree_, adjRib_->getPeerOwnerKey()),
      0);
}

} // namespace facebook::bgp
