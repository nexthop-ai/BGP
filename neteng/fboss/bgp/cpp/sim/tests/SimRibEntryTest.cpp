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
 * Tests for SimRibEntry — per-prefix best-path selection for the BGP simulator.
 * Verifies best-path selection criteria, multipath computation, path
 * withdrawal, thrift export, and debug string output.
 */

#include <gtest/gtest.h>

#include "neteng/fboss/bgp/cpp/BgpServiceUtil.h"
#include "neteng/fboss/bgp/cpp/sim/SimRibEntry.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
namespace facebook::bgp {
namespace {

// Router-ID constants for test peers (10.x.0.x in hex)
constexpr uint64_t kRouterId1 = 0x0A010001; // 10.1.0.1
constexpr uint64_t kRouterId2 = 0x0A020002; // 10.2.0.2
constexpr uint64_t kRouterId3 = 0x0A030003; // 10.3.0.3

// Test peer addresses derived from Utils.h (kPeerAddr1, kPeerAddr2)
const std::string kPeer1Addr = kPeerAddr1.str();
const std::string kPeer2Addr = kPeerAddr2.str();
/*
 * Peer 3 uses a distinct address not available in Utils.h
 * (kPeerAddr3 in Utils.h is "127.3.0.1", different value)
 */
const std::string kPeer3Addr = "3.3.3.3";

// AS-path lengths for best-path selection tests
constexpr uint32_t kAsPathLenShort = 1;
constexpr uint32_t kAsPathLenDefault = 2;
constexpr uint32_t kAsPathLenLong = 3;

// MED values without an equivalent in Utils.h (kMed2 == 50 is reused directly)
constexpr uint32_t kMedNone = 0;
constexpr uint32_t kMedHigh = 200;

// Lower-end weight (kWeight == 100 / kWeight2 == 200 are reused from Utils.h)
constexpr uint16_t kWeightLow = 50;

/*
 * Higher local-pref for replacement tests (distinct from
 * kLocalPref/kLocalPref2)
 */

constexpr uint32_t kLocalPrefHigh = 300;

// Non-existent peer for withdraw-miss tests
const std::string kNonExistentPeerAddr = "9.9.9.9";

} // namespace

class SimRibEntryTest : public ::testing::Test {
 protected:
  /*
   * Create a SimRouteInfo with configurable attributes for testing
   * selection criteria.
   */
  std::shared_ptr<SimRouteInfo> makeRoute(
      const std::string& peerAddrStr,
      uint32_t localPref = kLocalPref,
      uint32_t med = kMedNone,
      uint16_t weight = kWeight,
      uint32_t asCount = kAsPathLenDefault,
      uint64_t routerId = kRouterId1,
      RouteOrigin origin = RouteOrigin::EXTERNAL) {
    auto fields = buildBgpPathFields(
        asCount,
        /*community_count=*/0,
        /*ext_community_count=*/0,
        /*cluster_list_count=*/0);
    auto bgpPath = std::make_shared<BgpPath>(*fields);
    bgpPath->setLocalPref(localPref);
    bgpPath->setMed(med);
    bgpPath->setWeight(weight);
    bgpPath->publish();

    auto peerIp = folly::IPAddress(peerAddrStr);
    return std::make_shared<SimRouteInfo>(
        kV4Prefix1,
        std::move(bgpPath),
        peerAddrStr,
        routerId,
        peerIp,
        origin,
        /*medMissingAsWorst=*/false);
  }

  RoutingTableConfig defaultConfig() {
    return RoutingTableConfig{};
  }

  /*
   * Build selectors from config and run best-path selection.
   * Mirrors how RoutingTable::runBestPathSelection works.
   */
  void runSelectBestPath(
      SimRibEntry& entry,
      const RoutingTableConfig& config = RoutingTableConfig{}) {
    const auto selectors = makeSimSelectors(config);
    entry.selectBestPath(selectors.multipath, selectors.bestpath);
  }
};

/*
 * Single path should be best and only multipath.
 */
TEST_F(SimRibEntryTest, SinglePath) {
  SimRibEntry entry(kV4Prefix1);
  auto route = makeRoute(kPeer1Addr);
  entry.insertPath(kPeer1Addr, route);
  EXPECT_TRUE(entry.isDirty());

  runSelectBestPath(entry);
  EXPECT_FALSE(entry.isDirty());
  EXPECT_EQ(route, entry.getBestPath());
  EXPECT_EQ(1u, entry.getMultipaths().size());
}

/*
 * Higher local-pref wins.
 */
TEST_F(SimRibEntryTest, HigherLocalPrefWins) {
  SimRibEntry entry(kV4Prefix1);
  entry.insertPath(kPeer1Addr, makeRoute(kPeer1Addr, kLocalPref));
  entry.insertPath(kPeer2Addr, makeRoute(kPeer2Addr, kLocalPref2));

  runSelectBestPath(entry);
  ASSERT_NE(nullptr, entry.getBestPath());
  EXPECT_EQ(kPeer2Addr, entry.getBestPath()->peerAddr);
}

/*
 * Shorter AS path wins (equal local-pref).
 */
TEST_F(SimRibEntryTest, ShorterAsPathWins) {
  SimRibEntry entry(kV4Prefix1);
  entry.insertPath(
      kPeer1Addr,
      makeRoute(kPeer1Addr, kLocalPref, kMedNone, kWeight, kAsPathLenLong));
  entry.insertPath(
      kPeer2Addr,
      makeRoute(kPeer2Addr, kLocalPref, kMedNone, kWeight, kAsPathLenShort));

  runSelectBestPath(entry);
  ASSERT_NE(nullptr, entry.getBestPath());
  EXPECT_EQ(kPeer2Addr, entry.getBestPath()->peerAddr);
}

/*
 * Lower origin code wins (IGP=0 < EGP=1).
 * buildBgpPathFields sets origin to EGP (1) by default.
 */
TEST_F(SimRibEntryTest, LowerOriginWins) {
  SimRibEntry entry(kV4Prefix1);

  // Route with EGP origin (default from buildBgpPathFields)
  auto route1 = makeRoute(kPeer1Addr);

  // Route with IGP origin
  auto fields2 = buildBgpPathFields(kAsPathLenDefault, 0, 0, 0);
  auto bgpPath2 = std::make_shared<BgpPath>(*fields2);
  bgpPath2->setLocalPref(kLocalPref);
  bgpPath2->setMed(kMedNone);
  bgpPath2->setWeight(kWeight);
  bgpPath2->setOrigin(nettools::bgplib::BgpAttrOrigin::BGP_ORIGIN_IGP);
  bgpPath2->publish();
  auto peerIp2 = folly::IPAddress(kPeer2Addr);
  auto route2 = std::make_shared<SimRouteInfo>(
      kV4Prefix1,
      std::move(bgpPath2),
      kPeer2Addr,
      kRouterId2,
      peerIp2,
      RouteOrigin::EXTERNAL,
      /*medMissingAsWorst=*/false);

  entry.insertPath(kPeer1Addr, route1);
  entry.insertPath(kPeer2Addr, route2);

  runSelectBestPath(entry);
  ASSERT_NE(nullptr, entry.getBestPath());
  EXPECT_EQ(kPeer2Addr, entry.getBestPath()->peerAddr);
}

/*
 * eBGP preferred over iBGP.
 */
TEST_F(SimRibEntryTest, ExternalPreferredOverInternal) {
  SimRibEntry entry(kV4Prefix1);
  entry.insertPath(
      kPeer1Addr,
      makeRoute(
          kPeer1Addr,
          kLocalPref,
          kMedNone,
          kWeight,
          kAsPathLenDefault,
          kRouterId1,
          RouteOrigin::INTERNAL));
  entry.insertPath(
      kPeer2Addr,
      makeRoute(
          kPeer2Addr,
          kLocalPref,
          kMedNone,
          kWeight,
          kAsPathLenDefault,
          kRouterId2,
          RouteOrigin::EXTERNAL));

  runSelectBestPath(entry);
  ASSERT_NE(nullptr, entry.getBestPath());
  EXPECT_EQ(kPeer2Addr, entry.getBestPath()->peerAddr);
  // Only eBGP route in the multipath set
  EXPECT_EQ(1u, entry.getMultipaths().size());
}

/*
 * Lower router-ID tiebreak.
 */
TEST_F(SimRibEntryTest, LowerRouterIdTiebreak) {
  SimRibEntry entry(kV4Prefix1);
  entry.insertPath(
      kPeer1Addr,
      makeRoute(
          kPeer1Addr,
          kLocalPref,
          kMedNone,
          kWeight,
          kAsPathLenDefault,
          kRouterId2));
  entry.insertPath(
      kPeer2Addr,
      makeRoute(
          kPeer2Addr,
          kLocalPref,
          kMedNone,
          kWeight,
          kAsPathLenDefault,
          kRouterId1));

  runSelectBestPath(entry);
  ASSERT_NE(nullptr, entry.getBestPath());
  EXPECT_EQ(kPeer2Addr, entry.getBestPath()->peerAddr);
}

/*
 * Lower peer-IP tiebreak (when router-IDs are equal).
 */
TEST_F(SimRibEntryTest, LowerPeerIpTiebreak) {
  SimRibEntry entry(kV4Prefix1);
  entry.insertPath(
      kPeer2Addr,
      makeRoute(
          kPeer2Addr,
          kLocalPref,
          kMedNone,
          kWeight,
          kAsPathLenDefault,
          kRouterId1));
  entry.insertPath(
      kPeer1Addr,
      makeRoute(
          kPeer1Addr,
          kLocalPref,
          kMedNone,
          kWeight,
          kAsPathLenDefault,
          kRouterId1));

  runSelectBestPath(entry);
  ASSERT_NE(nullptr, entry.getBestPath());
  EXPECT_EQ(kPeer1Addr, entry.getBestPath()->peerAddr);
}

/*
 * ECMP multipath set: equal paths should all appear in multipaths.
 */
TEST_F(SimRibEntryTest, MultipathEcmp) {
  SimRibEntry entry(kV4Prefix1);
  entry.insertPath(
      kPeer1Addr,
      makeRoute(
          kPeer1Addr,
          kLocalPref,
          kMedNone,
          kWeight,
          kAsPathLenDefault,
          kRouterId1));
  entry.insertPath(
      kPeer2Addr,
      makeRoute(
          kPeer2Addr,
          kLocalPref,
          kMedNone,
          kWeight,
          kAsPathLenDefault,
          kRouterId2));
  entry.insertPath(
      kPeer3Addr,
      makeRoute(
          kPeer3Addr,
          kLocalPref,
          kMedNone,
          kWeight,
          kAsPathLenDefault,
          kRouterId3));

  runSelectBestPath(entry);
  /*
   * All paths should be in the multipath set (equal up through multipath
   * filters)
   */
  EXPECT_EQ(3u, entry.getMultipaths().size());
  ASSERT_NE(nullptr, entry.getBestPath());
}

/*
 * Path withdrawal triggers reselection.
 */
TEST_F(SimRibEntryTest, WithdrawTriggersReselection) {
  SimRibEntry entry(kV4Prefix1);
  entry.insertPath(kPeer1Addr, makeRoute(kPeer1Addr, kLocalPref2));
  entry.insertPath(kPeer2Addr, makeRoute(kPeer2Addr, kLocalPref));

  runSelectBestPath(entry);
  ASSERT_NE(nullptr, entry.getBestPath());
  EXPECT_EQ(kPeer1Addr, entry.getBestPath()->peerAddr);

  entry.withdrawPath(kPeer1Addr);
  EXPECT_TRUE(entry.isDirty());

  runSelectBestPath(entry);
  ASSERT_NE(nullptr, entry.getBestPath());
  EXPECT_EQ(kPeer2Addr, entry.getBestPath()->peerAddr);
}

/*
 * Withdraw all paths → empty entry.
 */
TEST_F(SimRibEntryTest, WithdrawAllPaths) {
  SimRibEntry entry(kV4Prefix1);
  entry.insertPath(kPeer1Addr, makeRoute(kPeer1Addr));
  runSelectBestPath(entry);

  entry.withdrawPath(kPeer1Addr);
  runSelectBestPath(entry);

  EXPECT_EQ(nullptr, entry.getBestPath());
  EXPECT_TRUE(entry.getMultipaths().empty());
  EXPECT_TRUE(entry.isEmpty());
}

/*
 * Test toTRibEntry thrift export — verify prefix, best path attributes,
 * multipath group contents, and best_next_hop are correctly serialized.
 */
TEST_F(SimRibEntryTest, ToTRibEntryExport) {
  SimRibEntry entry(kV4Prefix1);
  entry.insertPath(kPeer1Addr, makeRoute(kPeer1Addr, kLocalPref2));
  entry.insertPath(kPeer2Addr, makeRoute(kPeer2Addr, kLocalPref));
  runSelectBestPath(entry);

  auto tEntry = entry.toTRibEntry();

  // Verify prefix is correctly exported
  EXPECT_EQ(kV4Prefix1.second, *tEntry.prefix()->num_bits());

  // Verify best path exists and is marked as best
  ASSERT_TRUE(tEntry.best_path().has_value());
  EXPECT_TRUE(*tEntry.best_path()->is_best_path());

  // Verify best_next_hop is populated
  EXPECT_FALSE(tEntry.best_next_hop()->prefix_bin()->empty());

  // Verify multipath group structure
  EXPECT_EQ(kMultiPathGroup, *tEntry.best_group());
  ASSERT_EQ(1u, tEntry.paths()->count(kMultiPathGroup));
  const auto& multiPaths = tEntry.paths()->at(kMultiPathGroup);
  EXPECT_EQ(1u, multiPaths.size());

  // Verify exactly one path in the group is marked best
  int bestCount = 0;
  for (const auto& p : multiPaths) {
    if (*p.is_best_path()) {
      ++bestCount;
    }
  }
  EXPECT_EQ(1, bestCount);
}

/*
 * Test toDebugString includes key BGP attributes for each path.
 */
TEST_F(SimRibEntryTest, DebugString) {
  SimRibEntry entry(kV4Prefix1);
  entry.insertPath(kPeer1Addr, makeRoute(kPeer1Addr, kLocalPref2));
  runSelectBestPath(entry);

  auto dbg = entry.toDebugString();
  EXPECT_FALSE(dbg.empty());

  /*
   * Verify the peer address is present; exact formatting is an
   * implementation detail of toDebugString(), not a contract.
   */
  EXPECT_NE(std::string::npos, dbg.find(kPeer1Addr));
}

/*
 * LOCAL_ROUTE filter: local route preferred over non-local with equal attrs.
 */
TEST_F(SimRibEntryTest, LocalRoutePreferred) {
  SimRibEntry entry(kV4Prefix1);
  entry.insertPath(
      kPeer1Addr,
      makeRoute(
          kPeer1Addr,
          kLocalPref,
          kMedNone,
          kWeight,
          kAsPathLenDefault,
          kRouterId1,
          RouteOrigin::INTERNAL));
  entry.insertPath(
      kPeer2Addr,
      makeRoute(
          kPeer2Addr,
          kLocalPref,
          kMedNone,
          kWeight,
          kAsPathLenDefault,
          kRouterId2,
          RouteOrigin::LOCAL));

  runSelectBestPath(entry);
  ASSERT_NE(nullptr, entry.getBestPath());
  EXPECT_EQ(kPeer2Addr, entry.getBestPath()->peerAddr);
  // Only the local route passes the LOCAL_ROUTE filter
  EXPECT_EQ(1u, entry.getMultipaths().size());
}

/*
 * Higher local-pref still wins over local route.
 */
TEST_F(SimRibEntryTest, HigherLocalPrefBeatsLocalRoute) {
  SimRibEntry entry(kV4Prefix1);
  entry.insertPath(
      kPeer1Addr,
      makeRoute(
          kPeer1Addr,
          kLocalPref2,
          kMedNone,
          kWeight,
          kAsPathLenDefault,
          kRouterId1,
          RouteOrigin::INTERNAL));
  entry.insertPath(
      kPeer2Addr,
      makeRoute(
          kPeer2Addr,
          kLocalPref,
          kMedNone,
          kWeight,
          kAsPathLenDefault,
          kRouterId2,
          RouteOrigin::LOCAL));

  runSelectBestPath(entry);
  ASSERT_NE(nullptr, entry.getBestPath());
  EXPECT_EQ(kPeer1Addr, entry.getBestPath()->peerAddr);
}

/*
 * MED comparison: when enabled, lower MED wins.
 */
TEST_F(SimRibEntryTest, MedComparisonEnabled) {
  SimRibEntry entry(kV4Prefix1);
  entry.insertPath(
      kPeer1Addr,
      makeRoute(
          kPeer1Addr,
          kLocalPref,
          kMedHigh,
          kWeight,
          kAsPathLenDefault,
          kRouterId1));
  entry.insertPath(
      kPeer2Addr,
      makeRoute(
          kPeer2Addr,
          kLocalPref,
          kMed2,
          kWeight,
          kAsPathLenDefault,
          kRouterId2));

  auto config = defaultConfig();
  config.enableMedComparison = true;
  runSelectBestPath(entry, config);
  ASSERT_NE(nullptr, entry.getBestPath());
  EXPECT_EQ(kPeer2Addr, entry.getBestPath()->peerAddr);
  EXPECT_EQ(1u, entry.getMultipaths().size());
}

/*
 * MED comparison: when disabled, MED is ignored and both are ECMP.
 */
TEST_F(SimRibEntryTest, MedComparisonDisabled) {
  SimRibEntry entry(kV4Prefix1);
  entry.insertPath(
      kPeer1Addr,
      makeRoute(
          kPeer1Addr,
          kLocalPref,
          kMedHigh,
          kWeight,
          kAsPathLenDefault,
          kRouterId1));
  entry.insertPath(
      kPeer2Addr,
      makeRoute(
          kPeer2Addr,
          kLocalPref,
          kMed2,
          kWeight,
          kAsPathLenDefault,
          kRouterId2));

  auto config = defaultConfig();
  config.enableMedComparison = false;
  runSelectBestPath(entry, config);
  // Without MED comparison, both routes are equal through multipath filters
  EXPECT_EQ(2u, entry.getMultipaths().size());
}

/*
 * Weight comparison: when disabled, weight is ignored.
 */
TEST_F(SimRibEntryTest, WeightComparisonDisabled) {
  SimRibEntry entry(kV4Prefix1);
  entry.insertPath(
      kPeer1Addr,
      makeRoute(
          kPeer1Addr,
          kLocalPref,
          kMedNone,
          kWeightLow,
          kAsPathLenDefault,
          kRouterId1));
  entry.insertPath(
      kPeer2Addr,
      makeRoute(
          kPeer2Addr,
          kLocalPref,
          kMedNone,
          kWeight2,
          kAsPathLenDefault,
          kRouterId2));

  auto config = defaultConfig();
  config.enableWeightComparison = false;
  runSelectBestPath(entry, config);
  // Without weight comparison, both routes are ECMP-eligible
  EXPECT_EQ(2u, entry.getMultipaths().size());
}

/*
 * eiBGP multipath: when enabled, eBGP and iBGP routes are both ECMP.
 */
TEST_F(SimRibEntryTest, EiBgpMultipathEnabled) {
  SimRibEntry entry(kV4Prefix1);
  entry.insertPath(
      kPeer1Addr,
      makeRoute(
          kPeer1Addr,
          kLocalPref,
          kMedNone,
          kWeight,
          kAsPathLenDefault,
          kRouterId1,
          RouteOrigin::EXTERNAL));
  entry.insertPath(
      kPeer2Addr,
      makeRoute(
          kPeer2Addr,
          kLocalPref,
          kMedNone,
          kWeight,
          kAsPathLenDefault,
          kRouterId2,
          RouteOrigin::INTERNAL));

  auto config = defaultConfig();
  config.enableEiBgpMultipath = true;
  runSelectBestPath(entry, config);
  // Both eBGP and iBGP should be in the multipath set
  EXPECT_EQ(2u, entry.getMultipaths().size());
}

/*
 * Calling selectBestPath on an empty entry (no paths) should
 * produce no best path and clear dirty.
 */
TEST_F(SimRibEntryTest, SelectBestPath_EmptyEntry) {
  SimRibEntry entry(kV4Prefix1);
  runSelectBestPath(entry);

  EXPECT_EQ(nullptr, entry.getBestPath());
  EXPECT_TRUE(entry.getMultipaths().empty());
  EXPECT_FALSE(entry.isDirty());
}

/*
 * All paths marked deleted → no best path selected.
 */
TEST_F(SimRibEntryTest, SelectBestPath_AllDeletedPaths) {
  SimRibEntry entry(kV4Prefix1);
  auto route1 = makeRoute(kPeer1Addr);
  auto route2 = makeRoute(kPeer2Addr);
  route1->setRouteDeleted();
  route2->setRouteDeleted();
  entry.insertPath(kPeer1Addr, route1);
  entry.insertPath(kPeer2Addr, route2);

  runSelectBestPath(entry);

  EXPECT_EQ(nullptr, entry.getBestPath());
  EXPECT_TRUE(entry.getMultipaths().empty());
}

/*
 * Higher weight wins over lower weight (equal local-pref, AS path, origin).
 */
TEST_F(SimRibEntryTest, HigherWeightWins) {
  SimRibEntry entry(kV4Prefix1);
  entry.insertPath(
      kPeer1Addr,
      makeRoute(
          kPeer1Addr, kLocalPref, kMedNone, kWeightLow, kAsPathLenDefault));
  entry.insertPath(
      kPeer2Addr,
      makeRoute(kPeer2Addr, kLocalPref, kMedNone, kWeight2, kAsPathLenDefault));

  auto config = defaultConfig();
  config.enableWeightComparison = true;

  runSelectBestPath(entry, config);
  ASSERT_NE(nullptr, entry.getBestPath());
  EXPECT_EQ(kPeer2Addr, entry.getBestPath()->peerAddr);
  EXPECT_EQ(1u, entry.getMultipaths().size());
}

/*
 * Multipath set excludes paths with different local-pref.
 * Only paths matching the highest local-pref should be ECMP candidates.
 */
TEST_F(SimRibEntryTest, MultipathExcludesDifferentLocalPref) {
  SimRibEntry entry(kV4Prefix1);
  entry.insertPath(
      kPeer1Addr,
      makeRoute(
          kPeer1Addr,
          kLocalPref2,
          kMedNone,
          kWeight,
          kAsPathLenDefault,
          kRouterId1));
  entry.insertPath(
      kPeer2Addr,
      makeRoute(
          kPeer2Addr,
          kLocalPref2,
          kMedNone,
          kWeight,
          kAsPathLenDefault,
          kRouterId2));
  entry.insertPath(
      kPeer3Addr,
      makeRoute(
          kPeer3Addr,
          kLocalPref,
          kMedNone,
          kWeight,
          kAsPathLenDefault,
          kRouterId3));

  runSelectBestPath(entry);

  /*
   * Only the two paths with local-pref=kLocalPref2 should be in the multipath
   * set
   */
  EXPECT_EQ(2u, entry.getMultipaths().size());
  ASSERT_NE(nullptr, entry.getBestPath());
}

/*
 * Best path should be marked preferred after multi-path selection.
 */
TEST_F(SimRibEntryTest, BestPathMarkedPreferred) {
  SimRibEntry entry(kV4Prefix1);
  entry.insertPath(
      kPeer1Addr,
      makeRoute(
          kPeer1Addr,
          kLocalPref,
          kMedNone,
          kWeight,
          kAsPathLenDefault,
          kRouterId1));
  entry.insertPath(
      kPeer2Addr,
      makeRoute(
          kPeer2Addr,
          kLocalPref,
          kMedNone,
          kWeight,
          kAsPathLenDefault,
          kRouterId2));

  runSelectBestPath(entry);

  ASSERT_NE(nullptr, entry.getBestPath());
  EXPECT_TRUE(entry.getBestPath()->getIsRoutePreferred());
}

/*
 * Inserting a new route for the same peer replaces the old one.
 */
TEST_F(SimRibEntryTest, ReplacePathForSamePeer) {
  SimRibEntry entry(kV4Prefix1);
  entry.insertPath(kPeer1Addr, makeRoute(kPeer1Addr, kLocalPref));
  runSelectBestPath(entry);
  EXPECT_EQ(kLocalPref, entry.getBestPath()->getBgpLocalPreference());

  // Replace with a route with different local-pref
  entry.insertPath(kPeer1Addr, makeRoute(kPeer1Addr, kLocalPrefHigh));
  runSelectBestPath(entry);
  EXPECT_EQ(kLocalPrefHigh, entry.getBestPath()->getBgpLocalPreference());
  EXPECT_EQ(1u, entry.getAllPaths().size());
}

/*
 * Withdrawing a non-existent path should not set dirty.
 */
TEST_F(SimRibEntryTest, WithdrawNonExistentPath) {
  SimRibEntry entry(kV4Prefix1);
  entry.insertPath(kPeer1Addr, makeRoute(kPeer1Addr));
  runSelectBestPath(entry);
  EXPECT_FALSE(entry.isDirty());

  entry.withdrawPath(kNonExistentPeerAddr);
  EXPECT_FALSE(entry.isDirty());
}

} // namespace facebook::bgp
