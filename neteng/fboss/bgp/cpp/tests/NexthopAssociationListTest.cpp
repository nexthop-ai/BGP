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

#include "neteng/fboss/bgp/cpp/common/RouteInfo.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/NexthopAssociationList.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

using namespace ::testing;
using namespace facebook::bgp;

class NexthopAssociationListTestFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    routeInfo_ = createRouteInfo(
        kV4Prefix1,
        kPeerAddr2,
        kPeerAddr1,
        kLocalPref,
        {},
        kPeerAsn2,
        kPeerRouterId2);
  }

  // Member variable to store routeInfo
  std::shared_ptr<RouteInfo> routeInfo_;
};

TEST_F(NexthopAssociationListTestFixture, LinkRouteInfo) {
  NexthopAssociationList nexthopList;

  // Initial size should be 0
  EXPECT_EQ(0, nexthopList.size());

  // Add the RouteInfo from the fixture
  bool addResult = nexthopList.link(*routeInfo_);

  // Size should be 1 after adding
  EXPECT_TRUE(addResult);
  EXPECT_EQ(1, nexthopList.size());
  EXPECT_TRUE(routeInfo_->isOnNextHopList());

  // Try to add the same RouteInfo again
  addResult = nexthopList.link(*routeInfo_);
  // Size should still be 1 as duplicate should not be added
  EXPECT_FALSE(addResult);
  EXPECT_EQ(1, nexthopList.size());
  EXPECT_TRUE(routeInfo_->isOnNextHopList());
}

TEST_F(NexthopAssociationListTestFixture, UnlinkRouteInfo) {
  NexthopAssociationList nexthopList;

  // Add the RouteInfo from the fixture
  bool addResult = nexthopList.link(*routeInfo_);

  // Size should be 1 after adding
  EXPECT_TRUE(addResult);
  EXPECT_EQ(1, nexthopList.size());

  // Remove the RouteInfo
  bool removeResult = nexthopList.unlink(*routeInfo_);

  // Size should be 0 after removing
  EXPECT_TRUE(removeResult);
  EXPECT_EQ(0, nexthopList.size());
  EXPECT_FALSE(routeInfo_->isOnNextHopList());

  // Try to remove the same RouteInfo again
  removeResult = nexthopList.unlink(*routeInfo_);
  // Size should still be 0 as it was already removed
  EXPECT_FALSE(removeResult);
  EXPECT_EQ(0, nexthopList.size());
  EXPECT_FALSE(routeInfo_->isOnNextHopList());
}

TEST_F(NexthopAssociationListTestFixture, MultipleRouteInfos) {
  NexthopAssociationList nexthopList;

  // Add the RouteInfo from the fixture
  bool addResult = nexthopList.link(*routeInfo_);
  EXPECT_TRUE(addResult);

  // Create a second RouteInfo with different prefix
  folly::CIDRNetwork prefix2{folly::IPAddress("192.168.2.0"), 24};
  auto routeInfo2 = createRouteInfo(
      prefix2,
      kPeerAddr2,
      kPeerAddr1,
      kLocalPref,
      {},
      kPeerAsn2,
      kPeerRouterId2);
  addResult = nexthopList.link(*routeInfo2);
  EXPECT_TRUE(addResult);

  // Create a third RouteInfo with different prefix
  folly::CIDRNetwork prefix3{folly::IPAddress("192.168.3.0"), 24};
  auto routeInfo3 = createRouteInfo(
      prefix3,
      kPeerAddr2,
      kPeerAddr1,
      kLocalPref,
      {},
      kPeerAsn2,
      kPeerRouterId2);
  addResult = nexthopList.link(*routeInfo3);
  EXPECT_TRUE(addResult);

  // Size should be 3 after adding three RouteInfos
  EXPECT_EQ(3, nexthopList.size());
  EXPECT_TRUE(routeInfo_->isOnNextHopList());
  EXPECT_TRUE(routeInfo2->isOnNextHopList());
  EXPECT_TRUE(routeInfo3->isOnNextHopList());

  // Remove the second RouteInfo
  bool removeResult = nexthopList.unlink(*routeInfo2);
  EXPECT_TRUE(removeResult);

  // Size should be 2 after removing one RouteInfo
  EXPECT_EQ(2, nexthopList.size());
  EXPECT_FALSE(routeInfo2->isOnNextHopList());

  // Remove the first RouteInfo
  removeResult = nexthopList.unlink(*routeInfo_);
  EXPECT_TRUE(removeResult);
  EXPECT_FALSE(routeInfo_->isOnNextHopList());

  // Size should be 1 after removing another RouteInfo
  EXPECT_EQ(1, nexthopList.size());

  // Remove the third RouteInfo
  removeResult = nexthopList.unlink(*routeInfo3);
  EXPECT_TRUE(removeResult);

  // Size should be 0 after removing all RouteInfos
  EXPECT_EQ(0, nexthopList.size());
  EXPECT_FALSE(routeInfo3->isOnNextHopList());
}

TEST_F(NexthopAssociationListTestFixture, AddAfterRemove) {
  NexthopAssociationList nexthopList;

  // Add the RouteInfo from the fixture
  bool addResult = nexthopList.link(*routeInfo_);

  // Size should be 1 after adding
  EXPECT_TRUE(addResult);
  EXPECT_EQ(1, nexthopList.size());
  EXPECT_TRUE(routeInfo_->isOnNextHopList());

  // Remove the RouteInfo
  bool removeResult = nexthopList.unlink(*routeInfo_);

  // Size should be 0 after removing
  EXPECT_TRUE(removeResult);
  EXPECT_EQ(0, nexthopList.size());
  EXPECT_FALSE(routeInfo_->isOnNextHopList());

  // Add the RouteInfo again
  addResult = nexthopList.link(*routeInfo_);

  // Size should be 1 after adding again
  EXPECT_TRUE(addResult);
  EXPECT_EQ(1, nexthopList.size());
  EXPECT_TRUE(routeInfo_->isOnNextHopList());
}
