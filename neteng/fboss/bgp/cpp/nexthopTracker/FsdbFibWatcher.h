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

#pragma once

#include "fboss/fsdb/client/FsdbSubscriber.h"
#include "fboss/fsdb/client/instantiations/FsdbCowStateSubManager.h"
#include "fboss/fsdb/if/FsdbModel.h"
#include "neteng/fboss/bgp/cpp/common/RibMessage.h"
#include "neteng/fboss/bgp/cpp/lib/coro/BackPressuredQueue.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/NexthopCache.h"
#include "neteng/fboss/bgp/cpp/watchdog/MonitoredQueue.h"

#include <folly/IPAddress.h>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <folly/coro/Task.h>
#include <folly/io/async/EventBase.h>

namespace facebook::bgp {

/**
 * FsdbFibWatcher: Tracks FIB host routes for statically configured BGP peers.
 *
 * Subscribes to per-peer host-route paths in FSDB (e.g.,
 * agent/switchState/fibsInfoMap["0"]/fibsMap[0]/fibV4["10.0.0.1/32"]) for
 * all switch IDs on the device (fetched from the agent), kDefaultVrfId=0.
 * Only static peers (those with exact IP peer_addr from
 * getPeerToConfig()) are subscribed — dynamic peers (CIDR peer_addr from
 * getDynamicPeerToConfig()) are NOT tracked here.
 *
 * Dynamic peers are always directly connected: ARP/NDP must resolve before
 * a TCP/BGP session can form, so FsdbNeighborWatcher handles them with
 * isConnected=true. There is no need for FIB-based tracking.
 *
 * Updates pushed to NexthopCache use isConnected=nullopt (FIB entries do not
 * imply L2 adjacency). NexthopCache enforces source-priority: if a nexthop
 * is already marked isConnected=true by NeighborWatcher, FsdbFibWatcher
 * updates are silently skipped — NeighborWatcher is the sole authority for
 * directly connected nexthops (both reachability and igpCost).
 **/
class FsdbFibWatcher : public std::enable_shared_from_this<FsdbFibWatcher> {
 public:
  FsdbFibWatcher(
      std::shared_ptr<NexthopCache> nexthopCache,
      nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage>& ribInQ,
      folly::EventBase* evb,
      std::shared_ptr<fboss::fsdb::FsdbCowStateSubManager> fsdbSubMgr);

  FsdbFibWatcher(const FsdbFibWatcher&) = delete;
  FsdbFibWatcher& operator=(const FsdbFibWatcher&) = delete;
  FsdbFibWatcher(FsdbFibWatcher&&) = delete;
  FsdbFibWatcher& operator=(FsdbFibWatcher&&) = delete;

  ~FsdbFibWatcher() = default;

  /**
   * Add a peer address to track. Must be called before addPaths() to
   * include in the FSDB subscription. If called after addPaths(), logs a
   * warning (paths are already registered with the shared sub manager).
   **/
  void addPeerAddress(const folly::IPAddress& peerAddr);

  /**
   * Remove a peer address from tracking. If called after addPaths(), logs
   * a warning (paths are already registered with the shared sub manager).
   **/
  void removePeerAddress(const folly::IPAddress& peerAddr);

  /**
   * Add peer addresses and register their FSDB FIB host-route paths on the
   * shared sub manager. Schedules addPaths() on evb_. Must be called before
   * NeighborWatcher::subscribe() so paths are registered before subscribe().
   **/
  void registerPeers(const std::vector<folly::IPAddress>& peerAddresses);

  void stop() noexcept;

  /**
   * Called on evb_ for each FSDB update. Purely event-driven: scans
   * updatedPaths for subscribed prefix strings (map keys appear as literal
   * tokens even when struct fields use numeric IDs) and checks only
   * affected peers. Non-FIB updates (e.g., interfaceMap) are skipped
   * because no subscribed prefix token will match.
   *
   * No notification for a prefix means the route does not exist yet.
   * As notifications arrive, nexthop status is updated. On reconnect,
   * markNeedsReconcile() clears all reachable state so status is rebuilt
   * purely from new notifications.
   **/
  folly::coro::Task<void> co_processFibUpdate(
      fboss::fsdb::FsdbCowStateSubManager::SubUpdate update);

  /**
   * Reset all reachable nexthops to unreachable. Called from the FSDB
   * connection state callback on (re)connect so that reachability is
   * rebuilt purely from new FSDB notifications.
   **/
  folly::coro::Task<void> co_markNeedsReconcile();

 private:
  /**
   * Update nexthop cache and push changed statuses to ribInQ.
   **/
  folly::coro::Task<void> co_updateCacheAndPushToRib(
      const std::vector<NexthopStatus>& updates);

  /**
   * Add FSDB paths for all tracked peer addresses to the shared sub manager.
   * Fetches switch IDs from the agent and registers FIB host-route paths.
   **/
  void addPaths() noexcept;

  /**
   * Check if a route exists in the COW tree for the given prefix and
   * extract the minimum IGP cost across its resolved nexthops.
   * Returns {exists, igpCost} where igpCost is nullopt when the route
   * does not exist or no nexthop carries a cost value.
   **/
  std::pair<bool, std::optional<uint32_t>> routeExistsInCowTree(
      const std::string& prefixStr,
      bool isV4,
      const fboss::fsdb::FsdbCowStateSubManager::Data& data);

  /**
   * Shared FSDB COW subscription manager — the same instance used by
   * FsdbNeighborWatcher. All addPath() calls (interfaceMaps + FIB routes)
   * must complete before the unified subscribe() in NeighborWatcher.
   **/
  std::shared_ptr<fboss::fsdb::FsdbCowStateSubManager> fsdbSubMgr_;
  thriftpath::RootThriftPath<fboss::fsdb::FsdbOperStateRoot> fsdbStateRootPath_;

  // Switch IDs fetched from agent in addPaths(), used in routeExistsInCowTree()
  std::vector<std::string> switchIds_;

  // Current set of nexthop IPs reachable via FIB host routes
  folly::F14FastSet<folly::IPAddress> reachableNexthops_;

  /**
   * Peer addresses being tracked — the authoritative set of peers whose FIB
   * host routes are subscribed (or pending subscription) in FSDB.
   **/
  folly::F14FastSet<folly::IPAddress> subscribedPeers_;

  /**
   * Map from prefix string (e.g., "fdad:500::d:0/128") to {peerAddr, isV4}.
   * Built in addPaths() for fast lookup when scanning updatedPath tokens.
   **/
  folly::F14FastMap<std::string, std::pair<folly::IPAddress, bool>>
      subscribedPrefixes_;

  /**
   * Whether addPaths() has been called. After this point, peer addresses
   * cannot be added or removed (paths are already registered with the
   * shared sub manager).
   **/
  bool pathsAdded_{false};

  std::shared_ptr<NexthopCache> nexthopCache_;
  nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage>& ribInQ_;
  folly::EventBase* evb_;
};

} // namespace facebook::bgp
