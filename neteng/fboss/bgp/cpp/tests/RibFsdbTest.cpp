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

#define RibBase_TEST_FRIENDS friend class RibFsdbFixture;

#include "fboss/lib/CommonUtils.h"
#include "neteng/fboss/bgp/cpp/tests/RibFsdbPolicyTestFixture.h"

using namespace facebook::bgp;
using namespace std::chrono;
using ::testing::_;

namespace facebook {
namespace bgp {

TEST_F(RibFsdbFixture, RibEntryFsdbPublishTest) {
  FLAGS_publish_rib_to_fsdb = true;

  auto subscribedRibMap = fsdbSubscriber_->subscribe(
      fsdbSubscriber_->getRootStatePath().bgp().ribMap());

  const auto kBatchTime = milliseconds(50);
  rib_->setFibBatchTime(kBatchTime);

  auto prefix1 = folly::IPAddress::createNetwork("10.0.0.0/24");
  auto prefix2 = folly::IPAddress::createNetwork("20.0.0.0/24");
  auto prefixBatch1 = PrefixPathIds{{prefix1, kDefaultPathID}};
  auto prefixBatch2 = PrefixPathIds{{prefix2, kDefaultPathID}};

  auto prefix1Str = folly::IPAddress::networkToString(prefix1);
  auto prefix2Str = folly::IPAddress::networkToString(prefix2);

  EXPECT_CALL(*rib_, prepareFibProgramming_()).Times(testing::AtLeast(1));
  EXPECT_CALL(*fib_, updateUnicastRoute_(_, _, _, _, _, _))
      .Times(testing::AtLeast(1));
  EXPECT_CALL(*fib_, program_(_)).Times(testing::AtLeast(1));

  auto fibFuture = fib_->getFibProgramFuture();
  sendInitialPathComputation();
  fibFuture.wait();

  fibFuture = fib_->getFibProgramFuture();
  sendAnnouncement(prefixBatch1, eBgpPeer1_, attr_);
  sendAnnouncement(prefixBatch2, eBgpPeer1_, attr_);
  fibFuture.wait();

  WITH_RETRIES_N(5, {
    auto ribMapLk = subscribedRibMap.rlock();
    ASSERT_EVENTUALLY_TRUE(ribMapLk->has_value());
    ASSERT_EVENTUALLY_EQ((*ribMapLk)->size(), 2);
    EXPECT_EVENTUALLY_TRUE((*ribMapLk)->count(prefix1Str) > 0);
    EXPECT_EVENTUALLY_TRUE((*ribMapLk)->count(prefix2Str) > 0);
  })

  fibFuture = fib_->getFibProgramFuture();
  sendWithdrawal(prefixBatch1, eBgpPeer1_);
  fibFuture.wait();

  WITH_RETRIES_N(5, {
    auto ribMapLk = subscribedRibMap.rlock();
    ASSERT_EVENTUALLY_TRUE(ribMapLk->has_value());
    EXPECT_EVENTUALLY_EQ((*ribMapLk)->size(), 1);
    EXPECT_EVENTUALLY_EQ((*ribMapLk)->count(prefix1Str), 0);
    EXPECT_EVENTUALLY_TRUE((*ribMapLk)->count(prefix2Str) > 0);
  })

  fibFuture = fib_->getFibProgramFuture();
  sendWithdrawal(prefixBatch2, eBgpPeer1_);
  fibFuture.wait();

  WITH_RETRIES_N(5, {
    auto ribMapLk = subscribedRibMap.rlock();
    ASSERT_EVENTUALLY_TRUE(ribMapLk->has_value());
    EXPECT_EVENTUALLY_EQ((*ribMapLk)->size(), 0);
  })
}

} // namespace bgp
} // namespace facebook
