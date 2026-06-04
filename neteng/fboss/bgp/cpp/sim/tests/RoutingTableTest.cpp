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
 * Tests for RoutingTable — main local RIB for the BGP simulator.
 */

#include <gtest/gtest.h>

#include <optional>

#include "neteng/emulation/emulator/if/gen-cpp2/emulation_routing_dump_types.h"
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/sim/RoutingTable.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

namespace facebook::bgp {

using ::neteng::emulation::emulator::FBOSSBgpRoutingInfoCollection;

constexpr uint32_t kDefaultAsCount = 2;
constexpr uint32_t kTestRouterId = 0x0A000001;
constexpr uint64_t kTestLocalAs = 65000;
constexpr uint64_t kTestLocalConfedAs = 65100;
constexpr uint64_t kDefaultPeerRouterId = 0x0A010001;
constexpr uint64_t kSecondPeerRouterId = 0x0A020002;
const auto kPeerAddrStr1 = kPeerAddr1.str();
const auto kPeerAddrStr2 = kPeerAddr2.str();
const std::string kTestPolicyName = "test-policy";

class RoutingTableTest : public ::testing::Test {
 protected:
  std::shared_ptr<SimRouteInfo> makeRoute(
      const folly::CIDRNetwork& prefix,
      const std::string& peerAddrStr,
      uint32_t localPref = kLocalPref,
      uint32_t asCount = kDefaultAsCount,
      uint64_t routerId = kDefaultPeerRouterId,
      RouteOrigin origin = RouteOrigin::EXTERNAL) {
    auto fields = buildBgpPathFields(asCount, 0, 0, 0);
    auto bgpPath = std::make_shared<BgpPath>(*fields);
    bgpPath->setLocalPref(localPref);
    bgpPath->setMed(0);
    bgpPath->setWeight(kWeight);
    bgpPath->publish();

    auto peerIp = folly::IPAddress(peerAddrStr);
    return std::make_shared<SimRouteInfo>(
        prefix,
        std::move(bgpPath),
        peerAddrStr,
        routerId,
        peerIp,
        origin,
        /*medMissingAsWorst=*/false);
  }

  std::shared_ptr<BgpPath> makeOriginatedBgpPath(uint32_t localPref = 100) {
    auto fields = buildBgpPathFields(0, 0, 0, 0);
    auto bgpPath = std::make_shared<BgpPath>(*fields);
    bgpPath->setLocalPref(localPref);
    bgpPath->setMed(0);
    bgpPath->setWeight(100);
    bgpPath->publish();
    return bgpPath;
  }

  RoutingTableConfig testConfig() {
    return RoutingTableConfig{
        .routerId = kTestRouterId,
        .localAs4Byte = kTestLocalAs,
        .localConfedAs4Byte = kTestLocalConfedAs,
    };
  }
};

/*
 * Insert and withdraw path lifecycle.
 */
TEST_F(RoutingTableTest, InsertAndWithdraw) {
  RoutingTable rt(testConfig());
  EXPECT_EQ(0u, rt.size());

  rt.insertPath(
      kV4Prefix1, kPeerAddrStr1, makeRoute(kV4Prefix1, kPeerAddrStr1));
  EXPECT_EQ(1u, rt.size());
  EXPECT_NE(nullptr, rt.getEntry(kV4Prefix1));

  rt.withdrawPath(kV4Prefix1, kPeerAddrStr1);
  EXPECT_EQ(0u, rt.size());
  EXPECT_EQ(nullptr, rt.getEntry(kV4Prefix1));
}

/*
 * withdrawAllFromPeer removes all paths from that peer.
 */
TEST_F(RoutingTableTest, WithdrawAllFromPeer) {
  RoutingTable rt(testConfig());
  rt.insertPath(
      kV4Prefix1, kPeerAddrStr1, makeRoute(kV4Prefix1, kPeerAddrStr1));
  rt.insertPath(
      kV4Prefix2, kPeerAddrStr1, makeRoute(kV4Prefix2, kPeerAddrStr1));
  rt.insertPath(
      kV4Prefix1,
      kPeerAddrStr2,
      makeRoute(
          kV4Prefix1,
          kPeerAddrStr2,
          kLocalPref,
          kDefaultAsCount,
          kSecondPeerRouterId));
  EXPECT_EQ(2u, rt.size());

  rt.withdrawAllFromPeer(kPeerAddrStr1);
  /*
   * kV4Prefix2 has no remaining paths -> removed
   * kV4Prefix1 still has peer 2 -> kept
   */
  EXPECT_EQ(1u, rt.size());
  EXPECT_NE(nullptr, rt.getEntry(kV4Prefix1));
  EXPECT_EQ(nullptr, rt.getEntry(kV4Prefix2));
}

/*
 * runBestPathSelection processes dirty entries.
 */
TEST_F(RoutingTableTest, BestPathSelection) {
  RoutingTable rt(testConfig());
  rt.insertPath(
      kV4Prefix1,
      kPeerAddrStr1,
      makeRoute(kV4Prefix1, kPeerAddrStr1, kLocalPref));
  rt.insertPath(
      kV4Prefix1,
      kPeerAddrStr2,
      makeRoute(
          kV4Prefix1,
          kPeerAddrStr2,
          kLocalPref2,
          kDefaultAsCount,
          kSecondPeerRouterId));

  rt.runBestPathSelection();

  auto* entry = rt.getEntry(kV4Prefix1);
  ASSERT_NE(nullptr, entry);
  ASSERT_NE(nullptr, entry->getBestPath());
  EXPECT_EQ(kPeerAddrStr2, entry->getBestPath()->peerAddr);
  EXPECT_FALSE(entry->isDirty());
}

/*
 * Originated routes management.
 */
TEST_F(RoutingTableTest, OriginatedRoutes) {
  RoutingTable rt(testConfig());
  auto bgpPath = makeOriginatedBgpPath(100);
  rt.addOriginatedRoute(kV4Prefix1, bgpPath, kTestPolicyName);

  // Originated route should appear in entries_ for best-path selection
  EXPECT_EQ(1u, rt.size());
  EXPECT_NE(nullptr, rt.getEntry(kV4Prefix1));

  FBOSSBgpRoutingInfoCollection collection;
  rt.populateCollection(collection);

  EXPECT_EQ(1u, collection.originated_routes()->size());
  EXPECT_TRUE(collection.originated_routes()->at(0).policy_name().has_value());
  EXPECT_EQ(
      kTestPolicyName, *collection.originated_routes()->at(0).policy_name());
}

/*
 * Originated route competes with learned routes in best-path selection.
 * When local-pref is equal, the originated route wins via LOCAL_ROUTE filter.
 */
TEST_F(RoutingTableTest, OriginatedRouteWinsViaLocalRoute) {
  RoutingTable rt(testConfig());

  // Add a learned route
  rt.insertPath(
      kV4Prefix1,
      "1.1.1.1",
      makeRoute(kV4Prefix1, "1.1.1.1", /*localPref=*/100));

  // Add an originated route with equal local-pref
  auto bgpPath = makeOriginatedBgpPath(100);
  rt.addOriginatedRoute(kV4Prefix1, bgpPath, "test-policy");

  rt.runBestPathSelection();

  auto* entry = rt.getEntry(kV4Prefix1);
  ASSERT_NE(nullptr, entry);
  ASSERT_NE(nullptr, entry->getBestPath());
  // Originated route (origin == RouteOrigin::LOCAL) wins via LOCAL_ROUTE filter
  EXPECT_EQ(
      std::string(RoutingTable::kLocalPeerAddr),
      entry->getBestPath()->peerAddr);
}

/*
 * Learned route with higher local-pref beats originated route.
 */
TEST_F(RoutingTableTest, LearnedRouteBeatsOriginatedWithHigherLocalPref) {
  RoutingTable rt(testConfig());

  // Add a learned route with higher local-pref
  rt.insertPath(
      kV4Prefix1,
      "1.1.1.1",
      makeRoute(kV4Prefix1, "1.1.1.1", /*localPref=*/200));

  // Add originated route with lower local-pref
  auto bgpPath = makeOriginatedBgpPath(100);
  rt.addOriginatedRoute(kV4Prefix1, bgpPath, "test-policy");

  rt.runBestPathSelection();

  auto* entry = rt.getEntry(kV4Prefix1);
  ASSERT_NE(nullptr, entry);
  ASSERT_NE(nullptr, entry->getBestPath());
  // Learned route with higher local-pref wins
  EXPECT_EQ("1.1.1.1", entry->getBestPath()->peerAddr);
}

/*
 * populateCollection fills expected fields.
 */
TEST_F(RoutingTableTest, PopulateCollection) {
  RoutingTable rt(testConfig());
  rt.insertPath(
      kV4Prefix1, kPeerAddrStr1, makeRoute(kV4Prefix1, kPeerAddrStr1));
  rt.runBestPathSelection();

  FBOSSBgpRoutingInfoCollection collection;
  rt.populateCollection(collection);

  // bgp_local_config
  EXPECT_EQ(kTestRouterId, *collection.bgp_local_config()->my_router_id());
  EXPECT_EQ(kTestLocalAs, *collection.bgp_local_config()->local_as_4_byte());
  EXPECT_EQ(
      kTestLocalConfedAs,
      *collection.bgp_local_config()->local_confed_as_4_byte());

  // rib_entries
  EXPECT_EQ(1u, collection.rib_entries()->size());
}

/*
 * Entry auto-cleanup when last path is withdrawn.
 */
TEST_F(RoutingTableTest, AutoCleanupOnLastWithdraw) {
  RoutingTable rt(testConfig());
  rt.insertPath(
      kV4Prefix1, kPeerAddrStr1, makeRoute(kV4Prefix1, kPeerAddrStr1));
  rt.insertPath(
      kV4Prefix1,
      kPeerAddrStr2,
      makeRoute(
          kV4Prefix1,
          kPeerAddrStr2,
          kLocalPref,
          kDefaultAsCount,
          kSecondPeerRouterId));
  EXPECT_EQ(1u, rt.size());

  rt.withdrawPath(kV4Prefix1, kPeerAddrStr1);
  EXPECT_EQ(1u, rt.size()); // Still has peer 2

  rt.withdrawPath(kV4Prefix1, kPeerAddrStr2);
  EXPECT_EQ(0u, rt.size()); // Auto-cleaned
}

/*
 * Debug string is non-empty for a populated table.
 */
TEST_F(RoutingTableTest, DebugString) {
  RoutingTable rt(testConfig());
  rt.insertPath(
      kV4Prefix1, kPeerAddrStr1, makeRoute(kV4Prefix1, kPeerAddrStr1));
  rt.runBestPathSelection();

  EXPECT_FALSE(rt.toDebugString().empty());
  EXPECT_NE(std::string::npos, rt.toDebugString().find(kPeerAddrStr1));
  EXPECT_EQ(std::string::npos, rt.toDebugString().find(kPeerAddrStr2));
}

/*
 * Re-adding an originated route with the same prefix replaces the previous
 * entry — only one originated route is exported, with the latest attrs/policy.
 */
TEST_F(RoutingTableTest, OriginatedRouteDeduplicate) {
  RoutingTable rt(testConfig());

  auto bgpPathA = makeOriginatedBgpPath(100);
  auto bgpPathB = makeOriginatedBgpPath(200);

  rt.addOriginatedRoute(kV4Prefix1, bgpPathA, "policy-a");
  rt.addOriginatedRoute(kV4Prefix1, bgpPathB, "policy-b");

  // Only one RIB entry for the prefix
  EXPECT_EQ(1u, rt.size());

  FBOSSBgpRoutingInfoCollection collection;
  rt.populateCollection(collection);

  // Only one originated route exported
  ASSERT_EQ(1u, collection.originated_routes()->size());
  EXPECT_EQ("policy-b", *collection.originated_routes()->at(0).policy_name());
}

/*
 * After deduplication, best-path selection uses the latest attrs.
 */
TEST_F(RoutingTableTest, OriginatedRouteDeduplicateBestPath) {
  RoutingTable rt(testConfig());

  // First originated route with low local-pref
  auto bgpPathA = makeOriginatedBgpPath(50);

  // Second originated route with high local-pref (replaces first)
  auto bgpPathB = makeOriginatedBgpPath(300);

  // Add a learned route with local-pref 200
  rt.insertPath(
      kV4Prefix1,
      "1.1.1.1",
      makeRoute(kV4Prefix1, "1.1.1.1", /*localPref=*/200));

  rt.addOriginatedRoute(kV4Prefix1, bgpPathA, "policy-a");
  rt.addOriginatedRoute(kV4Prefix1, bgpPathB, "policy-b");

  rt.runBestPathSelection();

  auto* entry = rt.getEntry(kV4Prefix1);
  ASSERT_NE(nullptr, entry);
  ASSERT_NE(nullptr, entry->getBestPath());
  // Latest originated route (local-pref 300) beats learned route (200)
  EXPECT_EQ(
      std::string(RoutingTable::kLocalPeerAddr),
      entry->getBestPath()->peerAddr);
}

/*
 * Multiple different prefixes are stored independently.
 */
TEST_F(RoutingTableTest, OriginatedRouteMultiplePrefixes) {
  RoutingTable rt(testConfig());

  auto bgpPathA = makeOriginatedBgpPath(100);
  auto bgpPathB = makeOriginatedBgpPath(100);

  rt.addOriginatedRoute(kV4Prefix1, bgpPathA, "policy-a");
  rt.addOriginatedRoute(kV4Prefix2, bgpPathB, "policy-b");

  EXPECT_EQ(2u, rt.size());

  FBOSSBgpRoutingInfoCollection collection;
  rt.populateCollection(collection);

  EXPECT_EQ(2u, collection.originated_routes()->size());
}

/*
 * populateCollection exports correct TBgpPath fields for originated routes.
 * Verifies the stack-allocated SimRouteInfo produces identical thrift output
 * to what a heap-allocated instance would generate.
 */
TEST_F(RoutingTableTest, OriginatedRoutePathFields) {
  RoutingTable rt(testConfig());

  auto bgpPath = makeOriginatedBgpPath(/*localPref=*/kLocalPref2);
  rt.addOriginatedRoute(kV4Prefix1, bgpPath, "test-policy");

  FBOSSBgpRoutingInfoCollection collection;
  rt.populateCollection(collection);

  ASSERT_EQ(1u, collection.originated_routes()->size());
  const auto& tOrig = collection.originated_routes()->at(0);
  const auto& path = *tOrig.path();

  // local_pref matches the value set on the BgpPath
  EXPECT_EQ(kLocalPref2, *path.local_pref());

  // router_id matches the RoutingTable config
  EXPECT_EQ(testConfig().routerId, *path.router_id());

  // peer_id is the zero IP sentinel for originated routes
  ASSERT_TRUE(path.peer_id().has_value());

  // med and weight match what makeOriginatedBgpPath sets
  EXPECT_EQ(0, *path.med());
  EXPECT_EQ(kWeight, *path.weight());

  // Originated routes are not marked as best path (fresh temporary)
  EXPECT_FALSE(*path.is_best_path());

  // Empty AS path (originated with 0 segments)
  EXPECT_TRUE(path.as_path()->empty());
}

/*
 * Helper to create routes with configurable MED for MED-related tests.
 */
class MedRoutingTableTest : public RoutingTableTest {
 protected:
  std::shared_ptr<SimRouteInfo> makeRouteWithMed(
      const folly::CIDRNetwork& prefix,
      const std::string& peerAddrStr,
      std::optional<uint32_t> med,
      bool medMissingAsWorst,
      uint32_t localPref = 100,
      uint32_t asCount = 2,
      uint64_t routerId = 0x0A010001,
      RouteOrigin origin = RouteOrigin::EXTERNAL) {
    auto fields = buildBgpPathFields(asCount, 0, 0, 0);
    auto bgpPath = std::make_shared<BgpPath>(*fields);
    bgpPath->setLocalPref(localPref);
    if (med.has_value()) {
      bgpPath->setMed(*med);
    } else {
      bgpPath->unSetMed();
    }
    bgpPath->setWeight(100);
    bgpPath->publish();

    auto peerIp = folly::IPAddress(peerAddrStr);
    return std::make_shared<SimRouteInfo>(
        prefix,
        std::move(bgpPath),
        peerAddrStr,
        routerId,
        peerIp,
        origin,
        medMissingAsWorst);
  }
};

/*
 * Parameterized MED best-path selection cases. Two routes are inserted for the
 * same prefix: "1.1.1.1" with MED=500 and "2.2.2.2" with no MED set. The config
 * flags and per-route router-IDs determine which route wins.
 */
struct MedSelectionTestCase {
  // Human-readable name used as the TEST_P instantiation suffix.
  std::string description;
  bool enableMedComparison;
  bool enableMedMissingAsWorst;
  /*
   * Router-IDs control the final tiebreaker (lower wins) when MED does not
   * decide the winner.
   */
  uint64_t routeWithMedRouterId;
  uint64_t routeWithoutMedRouterId;
  std::string expectedWinner;
};

class MedSelectionParamTest
    : public MedRoutingTableTest,
      public ::testing::WithParamInterface<MedSelectionTestCase> {};

TEST_P(MedSelectionParamTest, SelectsExpectedBestPath) {
  const auto& tc = GetParam();
  auto config = testConfig();
  config.enableMedComparison = tc.enableMedComparison;
  config.enableMedMissingAsWorst = tc.enableMedMissingAsWorst;
  RoutingTable rt(config);

  // Route with MED=500
  rt.insertPath(
      kV4Prefix1,
      "1.1.1.1",
      makeRouteWithMed(
          kV4Prefix1,
          "1.1.1.1",
          /*med=*/500,
          /*medMissingAsWorst=*/tc.enableMedMissingAsWorst,
          /*localPref=*/100,
          /*asCount=*/2,
          /*routerId=*/tc.routeWithMedRouterId));

  // Route without MED set
  rt.insertPath(
      kV4Prefix1,
      "2.2.2.2",
      makeRouteWithMed(
          kV4Prefix1,
          "2.2.2.2",
          /*med=*/std::nullopt,
          /*medMissingAsWorst=*/tc.enableMedMissingAsWorst,
          /*localPref=*/100,
          /*asCount=*/2,
          /*routerId=*/tc.routeWithoutMedRouterId));

  rt.runBestPathSelection();

  auto* entry = rt.getEntry(kV4Prefix1);
  ASSERT_NE(nullptr, entry);
  ASSERT_NE(nullptr, entry->getBestPath());
  EXPECT_EQ(tc.expectedWinner, entry->getBestPath()->peerAddr);
}

INSTANTIATE_TEST_SUITE_P(
    MedSelection,
    MedSelectionParamTest,
    ::testing::Values(
        /*
         * enableMedMissingAsWorst=false: missing MED is treated as 0, which is
         * lower than 500 → "2.2.2.2" wins on MED.
         */
        MedSelectionTestCase{/*description=*/"MedMissingAsWorstDisabled",
                             /*enableMedComparison=*/true,
                             /*enableMedMissingAsWorst=*/false,
                             /*routeWithMedRouterId=*/0x0A010001,
                             /*routeWithoutMedRouterId=*/0x0A020002,
                             /*expectedWinner=*/"2.2.2.2"},
        /*
         * enableMedMissingAsWorst=true: missing MED is treated as MAX, which is
         * higher than 500 → "1.1.1.1" wins on MED.
         */
        MedSelectionTestCase{/*description=*/"MedMissingAsWorstEnabled",
                             /*enableMedComparison=*/true,
                             /*enableMedMissingAsWorst=*/true,
                             /*routeWithMedRouterId=*/0x0A010001,
                             /*routeWithoutMedRouterId=*/0x0A020002,
                             /*expectedWinner=*/"1.1.1.1"},
        /*
         * enableMedComparison=false: MED is skipped entirely, both routes
         * survive multipath, and the router-ID tiebreaker (lower wins) selects
         * "2.2.2.2" (0x0A010001 < 0x0A020002).
         */
        MedSelectionTestCase{/*description=*/"MedComparisonDisabledSkipsMed",
                             /*enableMedComparison=*/false,
                             /*enableMedMissingAsWorst=*/true,
                             /*routeWithMedRouterId=*/0x0A020002,
                             /*routeWithoutMedRouterId=*/0x0A010001,
                             /*expectedWinner=*/"2.2.2.2"}),
    [](const ::testing::TestParamInfo<MedSelectionTestCase>& info) {
      return info.param.description;
    });

/*
 * Verify getBgpMedValue() returns correct values per medMissingAsWorst flag.
 */
TEST_F(MedRoutingTableTest, GetBgpMedValueDirectCheck) {
  // Route with MED set to 42
  auto routeWithMed = makeRouteWithMed(
      kV4Prefix1, "1.1.1.1", /*med=*/42, /*medMissingAsWorst=*/false);
  EXPECT_EQ(42, routeWithMed->getBgpMedValue());

  // Route without MED, medMissingAsWorst=false → 0
  auto routeNoMedFalse = makeRouteWithMed(
      kV4Prefix1, "1.1.1.1", /*med=*/std::nullopt, /*medMissingAsWorst=*/false);
  EXPECT_EQ(0, routeNoMedFalse->getBgpMedValue());

  // Route without MED, medMissingAsWorst=true → MAX
  auto routeNoMedTrue = makeRouteWithMed(
      kV4Prefix1, "1.1.1.1", /*med=*/std::nullopt, /*medMissingAsWorst=*/true);
  EXPECT_EQ(static_cast<int64_t>(kMedMax), routeNoMedTrue->getBgpMedValue());
}

// ---- Originated route removal and consistency tests ----

/*
 * removeOriginatedRoute clears both entries_ and originatedRoutes_.
 */
TEST_F(RoutingTableTest, RemoveOriginatedRoute) {
  RoutingTable rt(testConfig());
  rt.addOriginatedRoute(kV4Prefix1, makeOriginatedBgpPath(100), "policy-a");
  EXPECT_EQ(1u, rt.size());
  EXPECT_EQ(1u, rt.originatedSize());

  rt.removeOriginatedRoute(kV4Prefix1);
  EXPECT_EQ(0u, rt.size());
  EXPECT_EQ(0u, rt.originatedSize());

  // populateCollection should export nothing
  FBOSSBgpRoutingInfoCollection collection;
  rt.populateCollection(collection);
  EXPECT_EQ(0u, collection.originated_routes()->size());
  EXPECT_EQ(0u, collection.rib_entries()->size());
}

/*
 * removeOriginatedRoute leaves other originated prefixes untouched.
 */
TEST_F(RoutingTableTest, RemoveOriginatedRouteKeepsOthers) {
  RoutingTable rt(testConfig());
  rt.addOriginatedRoute(kV4Prefix1, makeOriginatedBgpPath(100), "policy-a");
  rt.addOriginatedRoute(kV4Prefix2, makeOriginatedBgpPath(100), "policy-b");
  EXPECT_EQ(2u, rt.size());
  EXPECT_EQ(2u, rt.originatedSize());

  rt.removeOriginatedRoute(kV4Prefix1);
  EXPECT_EQ(1u, rt.size());
  EXPECT_EQ(1u, rt.originatedSize());
  EXPECT_EQ(nullptr, rt.getEntry(kV4Prefix1));
  EXPECT_NE(nullptr, rt.getEntry(kV4Prefix2));
}

/*
 * Removing an originated route preserves a learned path for the same prefix.
 */
TEST_F(RoutingTableTest, RemoveOriginatedRouteKeepsLearnedPath) {
  RoutingTable rt(testConfig());

  rt.insertPath(
      kV4Prefix1,
      kPeerAddrStr1,
      makeRoute(kV4Prefix1, kPeerAddrStr1, /*localPref=*/100));
  rt.addOriginatedRoute(kV4Prefix1, makeOriginatedBgpPath(100), "policy-a");

  EXPECT_EQ(1u, rt.size());
  EXPECT_EQ(1u, rt.originatedSize());

  rt.removeOriginatedRoute(kV4Prefix1);

  // entries_ still has the learned path
  EXPECT_EQ(1u, rt.size());
  EXPECT_EQ(0u, rt.originatedSize());

  auto* entry = rt.getEntry(kV4Prefix1);
  ASSERT_NE(nullptr, entry);
  EXPECT_EQ(1u, entry->getAllPaths().size());
  EXPECT_NE(
      entry->getAllPaths().end(), entry->getAllPaths().find(kPeerAddrStr1));
}

/*
 * withdrawAllFromPeer("local") cleans up originatedRoutes_ consistently.
 */
TEST_F(RoutingTableTest, WithdrawAllFromLocalPeer) {
  RoutingTable rt(testConfig());
  rt.addOriginatedRoute(kV4Prefix1, makeOriginatedBgpPath(100), "policy-a");
  rt.addOriginatedRoute(kV4Prefix2, makeOriginatedBgpPath(100), "policy-b");

  // Also add a learned path on kV4Prefix1
  rt.insertPath(
      kV4Prefix1,
      kPeerAddrStr1,
      makeRoute(kV4Prefix1, kPeerAddrStr1, /*localPref=*/100));

  EXPECT_EQ(2u, rt.originatedSize());

  rt.withdrawAllFromPeer(std::string(RoutingTable::kLocalPeerAddr));

  // Both originated routes are cleaned from originatedRoutes_
  EXPECT_EQ(0u, rt.originatedSize());

  // kV4Prefix1 still has the learned path, kV4Prefix2 is gone
  EXPECT_EQ(1u, rt.size());
  EXPECT_NE(nullptr, rt.getEntry(kV4Prefix1));
  EXPECT_EQ(nullptr, rt.getEntry(kV4Prefix2));

  // populateCollection exports zero originated routes
  FBOSSBgpRoutingInfoCollection collection;
  rt.populateCollection(collection);
  EXPECT_EQ(0u, collection.originated_routes()->size());
}

/*
 * Re-adding an originated route after removal works correctly.
 */
TEST_F(RoutingTableTest, ReAddOriginatedRouteAfterRemoval) {
  RoutingTable rt(testConfig());
  rt.addOriginatedRoute(kV4Prefix1, makeOriginatedBgpPath(100), "policy-a");
  rt.removeOriginatedRoute(kV4Prefix1);

  EXPECT_EQ(0u, rt.size());
  EXPECT_EQ(0u, rt.originatedSize());

  rt.addOriginatedRoute(kV4Prefix1, makeOriginatedBgpPath(200), "policy-b");

  EXPECT_EQ(1u, rt.size());
  EXPECT_EQ(1u, rt.originatedSize());

  FBOSSBgpRoutingInfoCollection collection;
  rt.populateCollection(collection);
  ASSERT_EQ(1u, collection.originated_routes()->size());
  EXPECT_EQ("policy-b", *collection.originated_routes()->at(0).policy_name());
}

} // namespace facebook::bgp
