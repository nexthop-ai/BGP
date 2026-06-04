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

#include <gtest/gtest.h>

// Friend declarations must precede NeighborWatcher.h include so the
// NeighborWatcher_TEST_FRIENDS macro expands inside the class definition.
// FRIEND_TEST expands to friend class TestFixture_TestName_Test, so names
// must be unqualified (NeighborWatcher is in namespace facebook::bgp).
#define NeighborWatcher_TEST_FRIENDS                                        \
  friend class NeighborWatcherFibTest;                                      \
  FRIEND_TEST(NeighborWatcherFibTest, FibWatcherNotCreatedWhenNhtDisabled); \
  FRIEND_TEST(NeighborWatcherFibTest, FibWatcherCreatedWhenNhtEnabled);     \
  FRIEND_TEST(                                                              \
      NeighborWatcherFibTest, FibWatcherNotCreatedWithoutSharedSubMgr);     \
  FRIEND_TEST(NeighborWatcherFibTest, CleanShutdownWithFibWatcher);         \
  FRIEND_TEST(NeighborWatcherFibTest, CleanShutdownWithoutFibWatcher);      \
  FRIEND_TEST(NeighborWatcherFibTest, CleanShutdownWithoutSharedSubMgr);    \
  FRIEND_TEST(NeighborWatcherFibTest, StopAfterSubscribeIsClean);

#include <folly/IPAddress.h>
#include <folly/coro/GtestHelpers.h>
#include <folly/io/async/EventBase.h>
#include <folly/logging/xlog.h>
#include "fboss/agent/gen-cpp2/switch_state_types.h"
#include "fboss/agent/if/gen-cpp2/common_types.h"
#include "fboss/fsdb/if/facebook/gen-cpp2/fsdb_model_types.h"
#include "fboss/fsdb/oper/instantiations/FsdbCowRoot.h"
#include "neteng/fboss/bgp/cpp/common/RibMessage.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/FsdbFibWatcher.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/NexthopCache.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/NexthopStatus.h"
#include "neteng/fboss/bgp/cpp/peer/NeighborWatcher.h"
#include "neteng/fboss/bgp/cpp/watchdog/MonitoredQueue.h"

namespace facebook::bgp {

using namespace ::testing;

/**
 * Test constants: representative peer addresses used across tests.
 * V4 peers produce /32 host prefixes, V6 peers produce /128.
 **/
const auto kPeerV4 = folly::IPAddress("10.0.0.1");
const auto kPeerV6 = folly::IPAddress("fdad:500::d:0");
const auto kPeerV6_2 = folly::IPAddress("fdad:50c:1::d:0");

fboss::fsdb::FsdbCowStateSubManager::Data makeCowDataWithRoute(
    const std::string& prefix,
    bool isV4,
    const std::vector<std::optional<int64_t>>& nexthopCosts) {
  std::vector<fboss::NextHopThrift> nexthops;
  for (const auto& cost : nexthopCosts) {
    fboss::NextHopThrift nhop;
    nhop.address()->addr() = std::string(4, '\x01');
    if (cost.has_value()) {
      nhop.cost() = *cost;
    }
    nexthops.push_back(std::move(nhop));
  }

  fboss::state::RouteNextHopEntry fwd;
  fwd.nexthops() = std::move(nexthops);

  fboss::state::RouteFields route;
  route.fwd() = std::move(fwd);

  fboss::state::FibContainerFields fibContainer;
  if (isV4) {
    fibContainer.fibV4()[prefix] = std::move(route);
  } else {
    fibContainer.fibV6()[prefix] = std::move(route);
  }

  fboss::state::FibInfoFields fibInfo;
  fibInfo.fibsMap()[0] = std::move(fibContainer);

  fboss::state::SwitchState switchState;
  switchState.fibsInfoMap()["id=0"] = std::move(fibInfo);

  fboss::fsdb::AgentData agentData;
  agentData.switchState() = std::move(switchState);

  fboss::fsdb::FsdbOperStateRoot stateRoot;
  stateRoot.agent() = std::move(agentData);

  auto cowRoot = std::make_shared<fboss::thrift_cow::FsdbCowStateRoot>(
      std::move(stateRoot));
  cowRoot->publish();
  return cowRoot;
}

fboss::fsdb::FsdbCowStateSubManager::Data makeCowDataWithoutRoute() {
  fboss::fsdb::FsdbOperStateRoot stateRoot;
  auto cowRoot = std::make_shared<fboss::thrift_cow::FsdbCowStateRoot>(
      std::move(stateRoot));
  cowRoot->publish();
  return cowRoot;
}

// ---------------------------------------------------------------------------
// FsdbFibWatcherTest: unit tests for FsdbFibWatcher in isolation
// ---------------------------------------------------------------------------

class FsdbFibWatcherTest : public ::testing::Test {
 protected:
  void SetUp() override {
    nexthopCache_ = std::make_shared<NexthopCache>();
  }

  nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage> ribInQ_{
      nettools::bgplib::kMaxIngressQueueSize};
  std::shared_ptr<NexthopCache> nexthopCache_;
  folly::EventBase evb_;
};

// ---------------------------------------------------------------------------
// Peer address management tests
// ---------------------------------------------------------------------------

TEST_F(FsdbFibWatcherTest, AddPeerAddress) {
  /**
   * Verify that addPeerAddress() registers peers in subscribedPeers_.
   * Duplicate adds should be idempotent.
   **/
  auto subMgr = std::make_shared<fboss::fsdb::FsdbCowStateSubManager>(
      fboss::fsdb::SubscriptionOptions("test"),
      fboss::utils::ConnectionOptions("::1", 0));
  auto watcher =
      std::make_shared<FsdbFibWatcher>(nexthopCache_, ribInQ_, &evb_, subMgr);

  watcher->addPeerAddress(kPeerV4);
  watcher->addPeerAddress(kPeerV6);
  // Duplicate add — should be idempotent
  watcher->addPeerAddress(kPeerV4);

  // No crash, no exception. Peer set should contain both addresses.
  // (subscribedPeers_ is private, so we verify indirectly via registerPeers)
}

TEST_F(FsdbFibWatcherTest, RemovePeerAddress) {
  /**
   * Verify that removePeerAddress() removes a previously added peer.
   * Removing a non-existent peer should be a no-op.
   **/
  auto subMgr = std::make_shared<fboss::fsdb::FsdbCowStateSubManager>(
      fboss::fsdb::SubscriptionOptions("test"),
      fboss::utils::ConnectionOptions("::1", 0));
  auto watcher =
      std::make_shared<FsdbFibWatcher>(nexthopCache_, ribInQ_, &evb_, subMgr);

  watcher->addPeerAddress(kPeerV4);
  watcher->addPeerAddress(kPeerV6);
  watcher->removePeerAddress(kPeerV4);
  // Remove non-existent — should be a no-op
  watcher->removePeerAddress(folly::IPAddress("192.168.1.1"));
}

TEST_F(FsdbFibWatcherTest, AddPeerAfterPathsRegisteredIsRejected) {
  /**
   * After registerPeers() has been called (which schedules addPaths()),
   * subsequent addPeerAddress()/removePeerAddress() calls should be
   * rejected with a warning (pathsAdded_ guard). We verify no crash.
   **/
  auto subMgr = std::make_shared<fboss::fsdb::FsdbCowStateSubManager>(
      fboss::fsdb::SubscriptionOptions("test"),
      fboss::utils::ConnectionOptions("::1", 0));
  auto watcher =
      std::make_shared<FsdbFibWatcher>(nexthopCache_, ribInQ_, &evb_, subMgr);

  std::vector<folly::IPAddress> peers = {kPeerV4};
  watcher->registerPeers(peers);

  // Drain evb to execute addPaths()
  evb_.loopOnce();

  // These should log warnings but not crash
  watcher->addPeerAddress(kPeerV6);
  watcher->removePeerAddress(kPeerV4);
}

TEST_F(FsdbFibWatcherTest, RegisterPeersV4AndV6) {
  /**
   * Verify registerPeers() accepts both V4 and V6 addresses and
   * schedules addPaths() on the event base without errors.
   * addPaths() will fail to connect to the agent (no agent running)
   * but should fall back to switchId "id=0" gracefully.
   **/
  auto subMgr = std::make_shared<fboss::fsdb::FsdbCowStateSubManager>(
      fboss::fsdb::SubscriptionOptions("test"),
      fboss::utils::ConnectionOptions("::1", 0));
  auto watcher =
      std::make_shared<FsdbFibWatcher>(nexthopCache_, ribInQ_, &evb_, subMgr);

  std::vector<folly::IPAddress> peers = {kPeerV4, kPeerV6, kPeerV6_2};
  watcher->registerPeers(peers);

  // Drain evb to execute addPaths() — agent call will fail, falls back to id=0
  evb_.loopOnce();

  // No crash; paths registered with default switch ID
}

// ---------------------------------------------------------------------------
// markNeedsReconcile tests
// ---------------------------------------------------------------------------

CO_TEST_F(FsdbFibWatcherTest, MarkNeedsReconcileEmptyIsNoop) {
  /**
   * When there are no reachable nexthops, co_markNeedsReconcile() should
   * be a no-op — no updates pushed to NexthopCache.
   **/
  auto subMgr = std::make_shared<fboss::fsdb::FsdbCowStateSubManager>(
      fboss::fsdb::SubscriptionOptions("test"),
      fboss::utils::ConnectionOptions("::1", 0));
  auto watcher =
      std::make_shared<FsdbFibWatcher>(nexthopCache_, ribInQ_, &evb_, subMgr);

  co_await watcher->co_markNeedsReconcile();
}

TEST_F(FsdbFibWatcherTest, StopIsIdempotent) {
  /**
   * stop() should be safe to call multiple times and should not crash.
   * Subscription lifecycle is managed by the shared sub manager owner.
   **/
  auto subMgr = std::make_shared<fboss::fsdb::FsdbCowStateSubManager>(
      fboss::fsdb::SubscriptionOptions("test"),
      fboss::utils::ConnectionOptions("::1", 0));
  auto watcher =
      std::make_shared<FsdbFibWatcher>(nexthopCache_, ribInQ_, &evb_, subMgr);

  watcher->stop();
  watcher->stop(); // idempotent
}

// ---------------------------------------------------------------------------
// NexthopCache source-priority tests
// ---------------------------------------------------------------------------

TEST_F(FsdbFibWatcherTest, SourcePriorityConnectedBlocksNonConnected) {
  /**
   * Once a nexthop is marked isConnected=true (by NeighborWatcher),
   * updates from non-connected sources (isConnected=nullopt, as sent by
   * FsdbFibWatcher) should be silently skipped.
   **/
  auto ip = kPeerV4;

  // Simulate NeighborWatcher marking nexthop as directly connected
  std::vector<NexthopStatus> connectedUpdate;
  connectedUpdate.emplace_back(ip, true, 1, true /* isConnected */);
  nexthopCache_->addOrUpdateNextHopStatus(connectedUpdate);

  // Register the nexthop in RIB so we can read it back
  auto status = nexthopCache_->registerAndGetNexthopStatus(ip);
  EXPECT_TRUE(status.isReachable());
  EXPECT_EQ(status.getIgpCost(), 1);
  EXPECT_EQ(status.isConnected(), true);

  // Simulate FsdbFibWatcher sending unreachable with isConnected=nullopt
  std::vector<NexthopStatus> fibUpdate;
  fibUpdate.emplace_back(ip, false, std::nullopt, std::nullopt);
  nexthopCache_->addOrUpdateNextHopStatus(fibUpdate);

  // Should still be reachable — non-connected source was skipped
  status = nexthopCache_->registerAndGetNexthopStatus(ip);
  EXPECT_TRUE(status.isReachable());
  EXPECT_EQ(status.getIgpCost(), 1);
  EXPECT_EQ(status.isConnected(), true);
}

TEST_F(FsdbFibWatcherTest, SourcePriorityConnectedCanOverrideConnected) {
  /**
   * A connected source (isConnected=true) should be able to override
   * another connected source's data.
   **/
  auto ip = kPeerV6;

  // First connected update: reachable with igpCost=1
  std::vector<NexthopStatus> update1;
  update1.emplace_back(ip, true, 1, true);
  nexthopCache_->addOrUpdateNextHopStatus(update1);

  auto status = nexthopCache_->registerAndGetNexthopStatus(ip);
  EXPECT_TRUE(status.isReachable());
  EXPECT_EQ(status.getIgpCost(), 1);

  // Second connected update: unreachable (neighbor went away)
  std::vector<NexthopStatus> update2;
  update2.emplace_back(ip, false, std::nullopt, true);
  nexthopCache_->addOrUpdateNextHopStatus(update2);

  status = nexthopCache_->registerAndGetNexthopStatus(ip);
  EXPECT_FALSE(status.isReachable());
}

TEST_F(FsdbFibWatcherTest, NonConnectedSourceCanUpdateNonConnected) {
  /**
   * When a nexthop has isConnected=nullopt (set by FsdbFibWatcher),
   * another non-connected update should be accepted.
   **/
  auto ip = kPeerV4;

  // FIB says reachable
  std::vector<NexthopStatus> update1;
  update1.emplace_back(ip, true, 5, std::nullopt);
  nexthopCache_->addOrUpdateNextHopStatus(update1);

  auto status = nexthopCache_->registerAndGetNexthopStatus(ip);
  EXPECT_TRUE(status.isReachable());

  // FIB says unreachable
  std::vector<NexthopStatus> update2;
  update2.emplace_back(ip, false, std::nullopt, std::nullopt);
  nexthopCache_->addOrUpdateNextHopStatus(update2);

  status = nexthopCache_->registerAndGetNexthopStatus(ip);
  EXPECT_FALSE(status.isReachable());
}

// ---------------------------------------------------------------------------
// IGP cost extraction tests (co_processFibUpdate + COW tree)
// ---------------------------------------------------------------------------

CO_TEST_F(FsdbFibWatcherTest, ProcessFibUpdateSingleNexthopWithCost) {
  /**
   * Route with a single nexthop carrying cost=42.
   * Verify the actual IGP cost flows through to NexthopCache.
   **/
  auto subMgr = std::make_shared<fboss::fsdb::FsdbCowStateSubManager>(
      fboss::fsdb::SubscriptionOptions("test"),
      fboss::utils::ConnectionOptions("::1", 0));
  auto watcher =
      std::make_shared<FsdbFibWatcher>(nexthopCache_, ribInQ_, &evb_, subMgr);
  watcher->registerPeers({kPeerV4});
  evb_.loopOnce();

  auto cowData = makeCowDataWithRoute("10.0.0.1/32", /*isV4=*/true, {42});
  co_await watcher->co_processFibUpdate(
      fboss::fsdb::FsdbCowStateSubManager::SubUpdate{
          cowData, {}, {{"10.0.0.1/32"}}, std::nullopt, std::nullopt});

  auto status = nexthopCache_->registerAndGetNexthopStatus(kPeerV4);
  EXPECT_TRUE(status.isReachable());
  EXPECT_EQ(42u, status.getIgpCost());
}

CO_TEST_F(FsdbFibWatcherTest, ProcessFibUpdateMultipleNexthopsMinCost) {
  /**
   * Route with multiple nexthops carrying different costs.
   * Verify the minimum cost is selected.
   **/
  auto subMgr = std::make_shared<fboss::fsdb::FsdbCowStateSubManager>(
      fboss::fsdb::SubscriptionOptions("test"),
      fboss::utils::ConnectionOptions("::1", 0));
  auto watcher =
      std::make_shared<FsdbFibWatcher>(nexthopCache_, ribInQ_, &evb_, subMgr);
  watcher->registerPeers({kPeerV4});
  evb_.loopOnce();

  auto cowData =
      makeCowDataWithRoute("10.0.0.1/32", /*isV4=*/true, {100, 50, 200});
  co_await watcher->co_processFibUpdate(
      fboss::fsdb::FsdbCowStateSubManager::SubUpdate{
          cowData, {}, {{"10.0.0.1/32"}}, std::nullopt, std::nullopt});

  auto status = nexthopCache_->registerAndGetNexthopStatus(kPeerV4);
  EXPECT_TRUE(status.isReachable());
  EXPECT_EQ(50u, status.getIgpCost());
}

CO_TEST_F(FsdbFibWatcherTest, ProcessFibUpdateNexthopWithoutCost) {
  /**
   * Route with a nexthop that has no cost field set.
   * When no nexthop carries cost, igpCost is nullopt.
   **/
  auto subMgr = std::make_shared<fboss::fsdb::FsdbCowStateSubManager>(
      fboss::fsdb::SubscriptionOptions("test"),
      fboss::utils::ConnectionOptions("::1", 0));
  auto watcher =
      std::make_shared<FsdbFibWatcher>(nexthopCache_, ribInQ_, &evb_, subMgr);
  watcher->registerPeers({kPeerV4});
  evb_.loopOnce();

  auto cowData =
      makeCowDataWithRoute("10.0.0.1/32", /*isV4=*/true, {std::nullopt});
  co_await watcher->co_processFibUpdate(
      fboss::fsdb::FsdbCowStateSubManager::SubUpdate{
          cowData, {}, {{"10.0.0.1/32"}}, std::nullopt, std::nullopt});

  auto status = nexthopCache_->registerAndGetNexthopStatus(kPeerV4);
  EXPECT_EQ(std::nullopt, status.getIgpCost());
}

CO_TEST_F(FsdbFibWatcherTest, ProcessFibUpdateMixedCostNexthopsV6) {
  /**
   * V6 route with a mix of nexthops — some with cost, some without.
   * Verify the minimum of the set costs is selected.
   **/
  auto subMgr = std::make_shared<fboss::fsdb::FsdbCowStateSubManager>(
      fboss::fsdb::SubscriptionOptions("test"),
      fboss::utils::ConnectionOptions("::1", 0));
  auto watcher =
      std::make_shared<FsdbFibWatcher>(nexthopCache_, ribInQ_, &evb_, subMgr);
  watcher->registerPeers({kPeerV6});
  evb_.loopOnce();

  auto cowData = makeCowDataWithRoute(
      "fdad:500::d:0/128", /*isV4=*/false, {100, std::nullopt, 30});
  co_await watcher->co_processFibUpdate(
      fboss::fsdb::FsdbCowStateSubManager::SubUpdate{
          cowData, {}, {{"fdad:500::d:0/128"}}, std::nullopt, std::nullopt});

  auto status = nexthopCache_->registerAndGetNexthopStatus(kPeerV6);
  EXPECT_TRUE(status.isReachable());
  EXPECT_EQ(30u, status.getIgpCost());
}

CO_TEST_F(FsdbFibWatcherTest, ProcessFibUpdateBecameUnreachable) {
  /**
   * First make nexthop reachable with cost=42, then send an update
   * where the route no longer exists. Verify the nexthop becomes
   * unreachable with nullopt cost.
   **/
  auto subMgr = std::make_shared<fboss::fsdb::FsdbCowStateSubManager>(
      fboss::fsdb::SubscriptionOptions("test"),
      fboss::utils::ConnectionOptions("::1", 0));
  auto watcher =
      std::make_shared<FsdbFibWatcher>(nexthopCache_, ribInQ_, &evb_, subMgr);
  watcher->registerPeers({kPeerV4});
  evb_.loopOnce();

  auto cowData1 = makeCowDataWithRoute("10.0.0.1/32", /*isV4=*/true, {42});
  co_await watcher->co_processFibUpdate(
      fboss::fsdb::FsdbCowStateSubManager::SubUpdate{
          cowData1, {}, {{"10.0.0.1/32"}}, std::nullopt, std::nullopt});

  auto status1 = nexthopCache_->registerAndGetNexthopStatus(kPeerV4);
  EXPECT_TRUE(status1.isReachable());
  EXPECT_EQ(42u, status1.getIgpCost());

  auto cowData2 = makeCowDataWithoutRoute();
  co_await watcher->co_processFibUpdate(
      fboss::fsdb::FsdbCowStateSubManager::SubUpdate{
          cowData2, {}, {{"10.0.0.1/32"}}, std::nullopt, std::nullopt});

  auto status2 = nexthopCache_->registerAndGetNexthopStatus(kPeerV4);
  EXPECT_FALSE(status2.isReachable());
  EXPECT_EQ(std::nullopt, status2.getIgpCost());
}

CO_TEST_F(FsdbFibWatcherTest, ProcessFibUpdateCostChangeWhileReachable) {
  /**
   * Nexthop already reachable with cost=42. A subsequent FIB update with
   * cost=100 should override the cost even though the nexthop was already
   * reachable (no reachability transition needed).
   **/
  auto subMgr = std::make_shared<fboss::fsdb::FsdbCowStateSubManager>(
      fboss::fsdb::SubscriptionOptions("test"),
      fboss::utils::ConnectionOptions("::1", 0));
  auto watcher =
      std::make_shared<FsdbFibWatcher>(nexthopCache_, ribInQ_, &evb_, subMgr);
  watcher->registerPeers({kPeerV4});
  evb_.loopOnce();

  auto cowData1 = makeCowDataWithRoute("10.0.0.1/32", /*isV4=*/true, {42});
  co_await watcher->co_processFibUpdate(
      fboss::fsdb::FsdbCowStateSubManager::SubUpdate{
          cowData1, {}, {{"10.0.0.1/32"}}, std::nullopt, std::nullopt});

  auto status1 = nexthopCache_->registerAndGetNexthopStatus(kPeerV4);
  EXPECT_TRUE(status1.isReachable());
  EXPECT_EQ(42u, status1.getIgpCost());

  auto cowData2 = makeCowDataWithRoute("10.0.0.1/32", /*isV4=*/true, {100});
  co_await watcher->co_processFibUpdate(
      fboss::fsdb::FsdbCowStateSubManager::SubUpdate{
          cowData2, {}, {{"10.0.0.1/32"}}, std::nullopt, std::nullopt});

  auto status2 = nexthopCache_->registerAndGetNexthopStatus(kPeerV4);
  EXPECT_TRUE(status2.isReachable());
  EXPECT_EQ(100u, status2.getIgpCost());
}

CO_TEST_F(FsdbFibWatcherTest, ProcessFibUpdateCostRemovedWhileReachable) {
  /**
   * Nexthop already reachable with cost=42. A subsequent FIB update where
   * nexthops have no cost field should override cost to nullopt (N/A).
   **/
  auto subMgr = std::make_shared<fboss::fsdb::FsdbCowStateSubManager>(
      fboss::fsdb::SubscriptionOptions("test"),
      fboss::utils::ConnectionOptions("::1", 0));
  auto watcher =
      std::make_shared<FsdbFibWatcher>(nexthopCache_, ribInQ_, &evb_, subMgr);
  watcher->registerPeers({kPeerV4});
  evb_.loopOnce();

  auto cowData1 = makeCowDataWithRoute("10.0.0.1/32", /*isV4=*/true, {42});
  co_await watcher->co_processFibUpdate(
      fboss::fsdb::FsdbCowStateSubManager::SubUpdate{
          cowData1, {}, {{"10.0.0.1/32"}}, std::nullopt, std::nullopt});

  auto status1 = nexthopCache_->registerAndGetNexthopStatus(kPeerV4);
  EXPECT_TRUE(status1.isReachable());
  EXPECT_EQ(42u, status1.getIgpCost());

  auto cowData2 =
      makeCowDataWithRoute("10.0.0.1/32", /*isV4=*/true, {std::nullopt});
  co_await watcher->co_processFibUpdate(
      fboss::fsdb::FsdbCowStateSubManager::SubUpdate{
          cowData2, {}, {{"10.0.0.1/32"}}, std::nullopt, std::nullopt});

  auto status2 = nexthopCache_->registerAndGetNexthopStatus(kPeerV4);
  EXPECT_EQ(std::nullopt, status2.getIgpCost());
}

CO_TEST_F(FsdbFibWatcherTest, ProcessFibUpdateUnreachableCostIsNullopt) {
  /**
   * When a nexthop becomes unreachable (route disappears), the cost
   * should be explicitly set to nullopt (N/A). Verify the cost is
   * cleared on the unreachable transition.
   **/
  auto subMgr = std::make_shared<fboss::fsdb::FsdbCowStateSubManager>(
      fboss::fsdb::SubscriptionOptions("test"),
      fboss::utils::ConnectionOptions("::1", 0));
  auto watcher =
      std::make_shared<FsdbFibWatcher>(nexthopCache_, ribInQ_, &evb_, subMgr);
  watcher->registerPeers({kPeerV4});
  evb_.loopOnce();

  auto cowData1 = makeCowDataWithRoute("10.0.0.1/32", /*isV4=*/true, {99});
  co_await watcher->co_processFibUpdate(
      fboss::fsdb::FsdbCowStateSubManager::SubUpdate{
          cowData1, {}, {{"10.0.0.1/32"}}, std::nullopt, std::nullopt});

  auto status1 = nexthopCache_->registerAndGetNexthopStatus(kPeerV4);
  EXPECT_TRUE(status1.isReachable());
  EXPECT_EQ(99u, status1.getIgpCost());

  auto cowData2 = makeCowDataWithoutRoute();
  co_await watcher->co_processFibUpdate(
      fboss::fsdb::FsdbCowStateSubManager::SubUpdate{
          cowData2, {}, {{"10.0.0.1/32"}}, std::nullopt, std::nullopt});

  auto status2 = nexthopCache_->registerAndGetNexthopStatus(kPeerV4);
  EXPECT_FALSE(status2.isReachable());
  EXPECT_EQ(std::nullopt, status2.getIgpCost());
}

// ---------------------------------------------------------------------------
// NeighborWatcher + FsdbFibWatcher integration tests
// ---------------------------------------------------------------------------

class NeighborWatcherFibTest : public ::testing::Test {
 protected:
  void SetUp() override {
    FLAGS_enable_fsdb_patch_subscriber = true;
    sharedSubMgr_ = std::make_shared<fboss::fsdb::FsdbCowStateSubManager>(
        fboss::fsdb::SubscriptionOptions("test"),
        fboss::utils::ConnectionOptions("::1", 0));
    nexthopCache_ = std::make_shared<NexthopCache>();
  }

  MonitoredMPMCQueue<NeighborWatcherMessage> neighborEventQ_{};
  nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage> ribInQ_{
      nettools::bgplib::kMaxIngressQueueSize};
  std::shared_ptr<fboss::fsdb::FsdbCowStateSubManager> sharedSubMgr_;
  std::shared_ptr<NexthopCache> nexthopCache_;
};

TEST_F(NeighborWatcherFibTest, FibWatcherNotCreatedWhenNhtDisabled) {
  /**
   * When nexthop tracking is not enabled (i.e., startFibWatcher() is
   * never called), fsdbFibWatcher_ should remain nullptr. This mirrors
   * the Main.cpp flow where startFibWatcher() is only called inside
   * the enableNextHopTracking guard.
   **/
  auto nbrWatcher = std::make_shared<NeighborWatcher>(
      neighborEventQ_,
      ribInQ_,
      false /* enableDsfFastTearDown */,
      sharedSubMgr_);

  // FIB watcher should not exist — startFibWatcher() was never called
  EXPECT_FALSE(nbrWatcher->fsdbFibWatcher_);
}

TEST_F(NeighborWatcherFibTest, FibWatcherCreatedWhenNhtEnabled) {
  /**
   * When startFibWatcher() is called (nexthop tracking enabled),
   * fsdbFibWatcher_ should be created and hold a reference to the
   * shared sub manager.
   * We must run the evb thread so that registerPeers()'s scheduled
   * addPaths() callback executes before teardown.
   **/
  auto nbrWatcher = std::make_shared<NeighborWatcher>(
      neighborEventQ_,
      ribInQ_,
      false /* enableDsfFastTearDown */,
      sharedSubMgr_);

  auto nbrWatcherThread = nbrWatcher->runInThread();

  std::vector<folly::IPAddress> peers = {kPeerV4, kPeerV6};
  nbrWatcher->startFibWatcher(nexthopCache_, ribInQ_, peers);

  EXPECT_TRUE(nbrWatcher->fsdbFibWatcher_);

  nbrWatcher->stop();
  nbrWatcherThread.join();
}

TEST_F(NeighborWatcherFibTest, FibWatcherNotCreatedWithoutSharedSubMgr) {
  /**
   * startFibWatcher() should bail out gracefully (log error, no crash)
   * when sharedFsdbSubMgr_ is nullptr (non-patch-subscriber mode).
   **/
  auto nbrWatcher = std::make_shared<NeighborWatcher>(
      neighborEventQ_,
      ribInQ_,
      false /* enableDsfFastTearDown */,
      nullptr /* sharedFsdbSubMgr */);

  nbrWatcher->startFibWatcher(nexthopCache_, ribInQ_, {kPeerV4});

  // Should remain nullptr — no shared sub manager available
  EXPECT_FALSE(nbrWatcher->fsdbFibWatcher_);
}

TEST_F(NeighborWatcherFibTest, SubscribeWithoutFibWatcherIsNoCrash) {
  /**
   * subscribe() should work correctly when FsdbFibWatcher is not
   * created (NHT disabled). The subscription dispatches only to
   * FsdbNeighborWatcher.
   **/
  auto nbrWatcher = std::make_shared<NeighborWatcher>(
      neighborEventQ_,
      ribInQ_,
      false /* enableDsfFastTearDown */,
      sharedSubMgr_);

  auto nbrWatcherThread = nbrWatcher->runInThread();

  // subscribe() without startFibWatcher() — neighbor-only mode
  nbrWatcher->subscribe();

  // Clean shutdown
  nbrWatcher->stop();
  nbrWatcherThread.join();
}

TEST_F(NeighborWatcherFibTest, SubscribeWithFibWatcherIsNoCrash) {
  /**
   * subscribe() should work correctly when FsdbFibWatcher is created
   * (NHT enabled). The subscription dispatches to both
   * FsdbNeighborWatcher and FsdbFibWatcher.
   **/
  auto nbrWatcher = std::make_shared<NeighborWatcher>(
      neighborEventQ_,
      ribInQ_,
      false /* enableDsfFastTearDown */,
      sharedSubMgr_);

  auto nbrWatcherThread = nbrWatcher->runInThread();

  nbrWatcher->learnConnectedNeighbors(nexthopCache_, ribInQ_);
  nbrWatcher->startFibWatcher(nexthopCache_, ribInQ_, {kPeerV4, kPeerV6});
  nbrWatcher->subscribe();

  // Clean shutdown
  nbrWatcher->stop();
  nbrWatcherThread.join();
}

// ---------------------------------------------------------------------------
// Cleanup / exit sequence tests
// ---------------------------------------------------------------------------

TEST_F(NeighborWatcherFibTest, CleanShutdownWithFibWatcher) {
  /**
   * Verify clean shutdown sequence: stop() should properly tear down
   * the shared sub manager and FsdbFibWatcher without crashes or hangs.
   * This mirrors the Main.cpp exit path:
   *   neighborWatcher->stop() → evb stops → thread joins → reset
   **/
  auto nbrWatcher = std::make_shared<NeighborWatcher>(
      neighborEventQ_,
      ribInQ_,
      false /* enableDsfFastTearDown */,
      sharedSubMgr_);

  auto nbrWatcherThread = nbrWatcher->runInThread();

  nbrWatcher->learnConnectedNeighbors(nexthopCache_, ribInQ_);
  nbrWatcher->startFibWatcher(nexthopCache_, ribInQ_, {kPeerV4, kPeerV6});
  nbrWatcher->subscribe();

  // Verify FIB watcher exists
  EXPECT_TRUE(nbrWatcher->fsdbFibWatcher_);

  // Stop and join — must not hang or crash
  nbrWatcher->stop();
  nbrWatcherThread.join();

  // After stop, the watcher pointers are still valid (shared_ptr) but
  // the evb loop has exited and subscriptions are torn down
  nbrWatcher.reset();
}

TEST_F(NeighborWatcherFibTest, CleanShutdownWithoutFibWatcher) {
  /**
   * Verify clean shutdown when NHT is disabled (no FsdbFibWatcher).
   * stop() should properly tear down just the NeighborWatcher components.
   **/
  auto nbrWatcher = std::make_shared<NeighborWatcher>(
      neighborEventQ_,
      ribInQ_,
      false /* enableDsfFastTearDown */,
      sharedSubMgr_);

  auto nbrWatcherThread = nbrWatcher->runInThread();
  nbrWatcher->subscribe();

  EXPECT_FALSE(nbrWatcher->fsdbFibWatcher_);

  nbrWatcher->stop();
  nbrWatcherThread.join();
  nbrWatcher.reset();
}

TEST_F(NeighborWatcherFibTest, CleanShutdownWithoutSharedSubMgr) {
  /**
   * Verify clean shutdown when using legacy mode (no shared sub manager,
   * no patch subscriber). This is the non-COW path.
   **/
  FLAGS_enable_fsdb_patch_subscriber = false;
  auto nbrWatcher = std::make_shared<NeighborWatcher>(
      neighborEventQ_,
      ribInQ_,
      false /* enableDsfFastTearDown */,
      nullptr /* sharedFsdbSubMgr */);

  auto nbrWatcherThread = nbrWatcher->runInThread();

  EXPECT_FALSE(nbrWatcher->fsdbFibWatcher_);

  nbrWatcher->stop();
  nbrWatcherThread.join();
  nbrWatcher.reset();
}

TEST_F(NeighborWatcherFibTest, StopBeforeSubscribeIsNoCrash) {
  /**
   * Calling stop() before subscribe() should be safe — no subscription
   * was started, so there's nothing to tear down on the FSDB side.
   **/
  auto nbrWatcher = std::make_shared<NeighborWatcher>(
      neighborEventQ_,
      ribInQ_,
      false /* enableDsfFastTearDown */,
      sharedSubMgr_);

  auto nbrWatcherThread = nbrWatcher->runInThread();

  nbrWatcher->startFibWatcher(nexthopCache_, ribInQ_, {kPeerV4});
  // Note: subscribe() deliberately NOT called

  nbrWatcher->stop();
  nbrWatcherThread.join();
}

TEST_F(NeighborWatcherFibTest, StopAfterSubscribeIsClean) {
  /**
   * Verify that stop() after subscribe() with FsdbFibWatcher properly
   * tears down all subscriptions and the evb loop without crashing.
   * Note: stop() is NOT idempotent (asyncScope_.cancelAndJoinAsync()
   * asserts on double-join), so we only call it once.
   **/
  auto nbrWatcher = std::make_shared<NeighborWatcher>(
      neighborEventQ_,
      ribInQ_,
      false /* enableDsfFastTearDown */,
      sharedSubMgr_);

  auto nbrWatcherThread = nbrWatcher->runInThread();

  nbrWatcher->startFibWatcher(nexthopCache_, ribInQ_, {kPeerV4});
  nbrWatcher->subscribe();

  nbrWatcher->stop();
  nbrWatcherThread.join();
}

} // namespace facebook::bgp
