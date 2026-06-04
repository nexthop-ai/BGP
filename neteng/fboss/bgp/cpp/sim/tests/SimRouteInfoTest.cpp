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
 * Tests for SimRouteInfo — the RouteBase adapter for the BGP simulator.
 * Verifies accessor delegation to BgpPath, flag behavior, thrift export,
 * and debug string output.
 */

#include <gtest/gtest.h>

#include "neteng/fboss/bgp/cpp/routelib/RouteSelector.h"
#include "neteng/fboss/bgp/cpp/sim/SimRouteInfo.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

namespace facebook::bgp {

class SimRouteInfoTest : public ::testing::Test {
 protected:
  static constexpr uint32_t kDefaultRouterId = 0x0A010001; // 10.1.0.1
  static constexpr uint32_t kDefaultAsCount = 2;

  /*
   * Create a SimRouteInfo with configurable attributes.
   */
  std::shared_ptr<SimRouteInfo> makeSimRouteInfo(
      uint32_t localPref = kLocalPref,
      uint32_t med = kMed,
      uint16_t weight = kWeight,
      uint32_t asCount = kDefaultAsCount,
      uint64_t routerId = kDefaultRouterId,
      RouteOrigin origin = RouteOrigin::EXTERNAL,
      uint32_t originatorId = kOriginatorId) {
    auto fields = buildBgpPathFields(
        asCount,
        /*community_count=*/1,
        /*ext_community_count=*/0,
        /*cluster_list_count=*/0);
    auto bgpPath = std::make_shared<BgpPath>(*fields);
    bgpPath->setLocalPref(localPref);
    bgpPath->setMed(med);
    bgpPath->setWeight(weight);
    bgpPath->setOriginatorId(originatorId);
    bgpPath->publish();

    return std::make_shared<SimRouteInfo>(
        kV4Prefix1,
        std::move(bgpPath),
        kPeerAddr1.str(),
        routerId,
        kPeerAddr1,
        origin,
        /*medMissingAsWorst=*/false);
  }
};

/*
 * Test that accessor methods correctly delegate to the underlying BgpPath.
 */
TEST_F(SimRouteInfoTest, AccessorDelegation) {
  auto route = makeSimRouteInfo(
      /*localPref=*/200, /*med=*/50, /*weight=*/150, /*asCount=*/3);

  EXPECT_EQ(200, route->getBgpLocalPreference());
  EXPECT_EQ(50, route->getBgpMedValue());
  EXPECT_EQ(150, route->getBgpWeightValue());
  EXPECT_EQ(3, route->getBgpAsPathLen());
  EXPECT_EQ(kV4Prefix1.second, route->getBgpPrefixLength());

  /*
   * Route origin flags — verify RouteBase interface correctly reflects
   * constructor parameters(used by route selection filters)
   */
  EXPECT_TRUE(route->getIsRouteExternal());
  EXPECT_FALSE(route->getIsRouteConfedExternal());

  auto localRoute = makeSimRouteInfo(
      kLocalPref,
      kMed,
      kWeight,
      kDefaultAsCount,
      kDefaultRouterId,
      RouteOrigin::LOCAL);
  EXPECT_FALSE(localRoute->getIsRouteExternal());
  EXPECT_EQ(RouteOrigin::LOCAL, localRoute->origin);
}

/*
 * The preferred flag is owned by best-path selection: RouteSelector marks the
 * winning path via setRoutePreferred() and clears the losers via
 * clearRoutePreferred(). Verify that running selection over two competing paths
 * sets the flag on the winner, clears it on the loser, and that the flag then
 * propagates to the thrift export (is_best_path).
 */
TEST_F(SimRouteInfoTest, PreferredFlagDrivenByRouteSelection) {
  auto highPref = makeSimRouteInfo(/*localPref=*/200);
  auto lowPref = makeSimRouteInfo(/*localPref=*/100);
  EXPECT_FALSE(highPref->getIsRoutePreferred());
  EXPECT_FALSE(lowPref->getIsRoutePreferred());

  // Best-path selection rule: prefer the highest local preference.
  nettools::edge::RouteSelector selector(
      nettools::edge::getRouteFilterConfigs(
          {{nettools::edge::RouteMetric::BGP_LOCAL_PREFERENCE,
            /*highest=*/true}}));
  std::vector<std::shared_ptr<nettools::edge::RouteBase>> routes{
      highPref, lowPref};
  EXPECT_TRUE(selector.markPreferredRoutes(routes));

  // Winner is marked preferred, loser is cleared.
  EXPECT_TRUE(highPref->getIsRoutePreferred());
  EXPECT_FALSE(lowPref->getIsRoutePreferred());

  // The preferred flag flows into the thrift export.
  EXPECT_TRUE(*highPref->toTBgpPath().is_best_path());
  EXPECT_FALSE(*lowPref->toTBgpPath().is_best_path());
}

/*
 * The deleted flag is consumed by the DELETED_ROUTE filter that
 * getRouteFilterConfigs() always prepends: a route flagged via
 * setRouteDeleted() must be dropped during selection before any tie-breaking
 * runs, leaving only the live route.
 */
TEST_F(SimRouteInfoTest, DeletedRouteFilteredOutBySelection) {
  auto liveRoute = makeSimRouteInfo();
  auto deletedRoute = makeSimRouteInfo();
  deletedRoute->setRouteDeleted();
  EXPECT_TRUE(deletedRoute->getIsRouteDeleted());

  nettools::edge::RouteSelector selector(
      nettools::edge::getRouteFilterConfigs(
          {{nettools::edge::RouteMetric::BGP_LOCAL_PREFERENCE,
            /*highest=*/true}}));
  std::vector<std::shared_ptr<nettools::edge::RouteBase>> routes{
      liveRoute, deletedRoute};

  const std::vector<std::shared_ptr<nettools::edge::RouteBase>> expected{
      liveRoute};
  EXPECT_EQ(expected, selector.selectRoutes(routes));
}

/*
 * Test toTBgpPath thrift conversion — verify key fields are populated.
 */
TEST_F(SimRouteInfoTest, ToTBgpPathConversion) {
  auto route = makeSimRouteInfo(/*localPref=*/200, /*med=*/50, /*weight=*/150);
  route->setRoutePreferred();

  auto tPath = route->toTBgpPath();

  EXPECT_EQ(200, *tPath.local_pref());
  EXPECT_EQ(50, *tPath.med());
  EXPECT_EQ(150, *tPath.weight());
  EXPECT_TRUE(*tPath.is_best_path());

  // buildBgpPathFields(asCount=2) creates one AS_SEQUENCE segment [0, 1]
  ASSERT_EQ(1u, tPath.as_path()->size());
  const auto& seg = tPath.as_path()->at(0);
  EXPECT_EQ(
      facebook::neteng::fboss::bgp_attr::TAsPathSegType::AS_SEQUENCE,
      *seg.seg_type());
  const std::vector<int64_t> expectedAsns{0, 1};
  EXPECT_EQ(expectedAsns, *seg.asns_4_byte());
}

/*
 * Test toDebugString contains key identifying information.
 * Checks for data values without coupling to exact format.
 */
TEST_F(SimRouteInfoTest, DebugString) {
  auto route = makeSimRouteInfo();
  auto debugStr = route->toDebugString();

  EXPECT_FALSE(debugStr.empty());
  EXPECT_NE(std::string::npos, debugStr.find(kPeerAddr1.str()));
  EXPECT_NE(std::string::npos, debugStr.find("8.0.0.0"));
  EXPECT_NE(std::string::npos, debugStr.find(std::to_string(kLocalPref)));
  EXPECT_NE(std::string::npos, debugStr.find(std::to_string(kMed)));
  EXPECT_NE(std::string::npos, debugStr.find(std::to_string(kWeight)));
}

/*
 * Test getOriginAsnAndPeerAsn extracts correct ASNs from AS path.
 */
TEST_F(SimRouteInfoTest, OriginAsnAndPeerAsn) {
  auto route = makeSimRouteInfo(kLocalPref, kMed, kWeight, /*asCount=*/3);
  auto [originAsn, peerAsn] = route->getOriginAsnAndPeerAsn();
  // buildBgpPathFields creates AS sequence [0, 1, 2]
  EXPECT_EQ(0u, peerAsn);
  EXPECT_EQ(2u, originAsn);
}

/*
 * getBgpRouterId() without originator ID falls back to routerId for both
 * upper and lower 32 bits: (routerId << 32) + routerId.
 */
TEST_F(SimRouteInfoTest, GetBgpRouterIdWithoutOriginatorId) {
  auto route = makeSimRouteInfo(
      kLocalPref,
      kMed,
      kWeight,
      kDefaultAsCount,
      kDefaultRouterId,
      RouteOrigin::EXTERNAL,
      /*originatorId=*/0);
  uint64_t expected =
      (static_cast<uint64_t>(kDefaultRouterId) << 32) + kDefaultRouterId;
  EXPECT_EQ(expected, route->getBgpRouterId());
}

/*
 * getBgpRouterId() with originator ID places it in the upper 32 bits
 * and keeps routerId in the lower 32 bits: (originatorId << 32) + routerId.
 */
TEST_F(SimRouteInfoTest, GetBgpRouterIdWithOriginatorId) {
  auto route = makeSimRouteInfo();
  uint64_t expected =
      (static_cast<uint64_t>(kOriginatorId) << 32) + kDefaultRouterId;
  EXPECT_EQ(expected, route->getBgpRouterId());
}

/*
 * Verify that two routes with the same routerId but different originator IDs
 * produce different getBgpRouterId() values — this is the tie-breaking
 * mechanism for VIP injector scenarios.
 */
TEST_F(SimRouteInfoTest, GetBgpRouterIdDifferentOriginatorsTieBreak) {
  constexpr uint32_t kOriginatorA = 0x01020304;
  constexpr uint32_t kOriginatorB = 0x05060708;
  auto routeA = makeSimRouteInfo(
      kLocalPref,
      kMed,
      kWeight,
      kDefaultAsCount,
      kDefaultRouterId,
      RouteOrigin::INTERNAL,
      /*originatorId=*/kOriginatorA);
  auto routeB = makeSimRouteInfo(
      kLocalPref,
      kMed,
      kWeight,
      kDefaultAsCount,
      kDefaultRouterId,
      RouteOrigin::INTERNAL,
      /*originatorId=*/kOriginatorB);
  EXPECT_NE(routeA->getBgpRouterId(), routeB->getBgpRouterId());
}

} // namespace facebook::bgp
