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

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "fb303/ServiceData.h"
#include "neteng/fboss/bgp/cpp/rib/RibCounters.h"
#include "neteng/fboss/bgp/cpp/stats/Stats.h"

namespace facebook::bgp {

namespace {
int64_t counter(const std::string& key) {
  auto tcData = fb303::ThreadCachedServiceData::get();
  tcData->publishStats();
  return tcData->getCounter(key);
}
} // namespace

// Each mutator must update the in-memory field AND mirror the matching fb303
// ODS counter, so existing dashboards / HealthValidator observe identical
// values.
TEST(RibCountersTest, PrefixCountTracksFieldAndFb303) {
  RibStats::initCounters();
  RibCounters c;
  EXPECT_EQ(0, c.totalPrefixes());
  EXPECT_EQ(0, counter(RibStats::kRibPrefixCount));

  c.onPrefixAdded(/*isV4=*/true, 24);
  c.onPrefixAdded(/*isV4=*/false, 64);
  EXPECT_EQ(2, c.totalPrefixes());
  EXPECT_EQ(2, counter(RibStats::kRibPrefixCount));

  c.onPrefixRemoved(/*isV4=*/true, 24);
  EXPECT_EQ(1, c.totalPrefixes());
  EXPECT_EQ(1, counter(RibStats::kRibPrefixCount));
}

// Total and per-prefix-length counts are split per address family.
TEST(RibCountersTest, PerAfiPrefixAndPerLengthCounts) {
  RibStats::initCounters();
  RibCounters c;

  c.onPrefixAdded(/*isV4=*/true, 24);
  c.onPrefixAdded(/*isV4=*/true, 24);
  c.onPrefixAdded(/*isV4=*/true, 32);
  c.onPrefixAdded(/*isV4=*/false, 64);

  EXPECT_EQ(3, c.totalPrefixes(/*isV4=*/true));
  EXPECT_EQ(1, c.totalPrefixes(/*isV4=*/false));
  EXPECT_EQ(4, c.totalPrefixes());

  EXPECT_EQ(2, c.prefixLenCounts(/*isV4=*/true)[24]);
  EXPECT_EQ(1, c.prefixLenCounts(/*isV4=*/true)[32]);
  EXPECT_EQ(0, c.prefixLenCounts(/*isV4=*/true)[64]);
  EXPECT_EQ(1, c.prefixLenCounts(/*isV4=*/false)[64]);
  // v4 /24 must not leak into the v6 counts.
  EXPECT_EQ(0, c.prefixLenCounts(/*isV4=*/false)[24]);

  c.onPrefixRemoved(/*isV4=*/true, 24);
  EXPECT_EQ(1, c.prefixLenCounts(/*isV4=*/true)[24]);
  EXPECT_EQ(2, c.totalPrefixes(/*isV4=*/true));
}

TEST(RibCountersTest, OriginatedRoutesTracksFieldAndFb303) {
  RibStats::initCounters();
  RibCounters c;

  c.setOriginatedRoutes(7);
  EXPECT_EQ(7, c.originatedRoutes());
  EXPECT_EQ(7, counter(RibStats::kTotalOriginatedRoutes));

  c.setOriginatedRoutes(3);
  EXPECT_EQ(3, c.originatedRoutes());
  EXPECT_EQ(3, counter(RibStats::kTotalOriginatedRoutes));
}

TEST(RibCountersTest, UnresolvableNexthopsTracksFieldAndFb303) {
  RibStats::initCounters();
  RibCounters c;

  c.onUnresolvableNexthopAdded();
  c.onUnresolvableNexthopAdded();
  c.onUnresolvableNexthopAdded();
  EXPECT_EQ(3, c.unresolvableNexthops());
  EXPECT_EQ(3, counter(RibStats::kRibUnresolvableNexthopsCount));

  c.onUnresolvableNexthopRemoved();
  EXPECT_EQ(2, c.unresolvableNexthops());
  EXPECT_EQ(2, counter(RibStats::kRibUnresolvableNexthopsCount));
}

// reset() zeroes the in-memory fields (used on the shutdown bulk-clear path).
TEST(RibCountersTest, ResetZeroesInMemoryFields) {
  RibStats::initCounters();
  RibCounters c;
  c.onPrefixAdded(/*isV4=*/true, 24);
  c.onPrefixAdded(/*isV4=*/false, 64);
  c.setOriginatedRoutes(5);
  c.onUnresolvableNexthopAdded();

  c.reset();
  EXPECT_EQ(0, c.totalPrefixes());
  EXPECT_EQ(0, c.totalPrefixes(/*isV4=*/true));
  EXPECT_EQ(0, c.prefixLenCounts(/*isV4=*/false)[64]);
  EXPECT_EQ(0, c.originatedRoutes());
  EXPECT_EQ(0, c.unresolvableNexthops());
}

} // namespace facebook::bgp
