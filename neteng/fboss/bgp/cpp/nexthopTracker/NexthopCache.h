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

#include <folly/IPAddress.h>
#include <folly/Synchronized.h>
#include <folly/container/F14Map.h>
#include <folly/logging/xlog.h>
#include <optional>
#include <tuple>
#include <vector>
#include "neteng/fboss/bgp/cpp/nexthopTracker/NexthopStatus.h"

namespace facebook::bgp {

/**
 * NexthopCache: Thread-safe cache of nexthop reachability and IGP cost.
 *
 * Two independent sources feed this cache:
 *
 *   1. FsdbFibWatcher — subscribes to FSDB FIB host routes for statically
 *      configured BGP peers (multi-hop or directly connected). Updates carry
 *      isConnected=nullopt because FIB entries do not imply L2 adjacency.
 *
 *   2. FsdbNeighborWatcher — watches ARP/NDP resolution on local interfaces.
 *      Updates carry isConnected=true, marking the nexthop as directly
 *      connected. This is the sole authority for directly connected nexthops.
 *
 * Source-priority rule (enforced in addOrUpdateNextHopStatus):
 *   Once a nexthop is marked isConnected=true (by NeighborWatcher), only
 *   another isConnected=true update can modify it. Non-connected sources
 *   (FsdbFibWatcher with isConnected=nullopt) are silently skipped. This
 *   prevents stale FIB data from overriding authoritative ARP/NDP-based
 *   connectivity and IGP cost for directly connected nexthops.
 *
 * Dynamic peers:
 *   Dynamic peers (configured with CIDR peer_addr) are always directly
 *   connected — ARP/NDP must resolve before a TCP/BGP session can form.
 *   NeighborWatcher reports them with isConnected=true before any routes
 *   arrive. They are NOT subscribed in FsdbFibWatcher (which only tracks
 *   static peers with exact IP peer_addr), so there is no conflict.
 **/
class NexthopCache {
 public:
  NexthopCache() = default;

  /**
   * @brief Updates the cache with new or modified NextHop statuses.
   *
   * Called by FsdbFibWatcher (with isConnected=nullopt) and by
   * FsdbNeighborWatcher (with isConnected=true). Enforces source-priority:
   * when an entry is already marked isConnected=true, updates from
   * non-connected sources are skipped to preserve NeighborWatcher authority.
   *
   * @param nexthopStatusList A vector of NexthopStatus objects containing
   *                          the updated status information
   * @return Vector of NexthopStatus objects that changed and are registered
   *         from RIB, to be pushed to ribInQ by the caller
   **/
  std::vector<NexthopStatus> addOrUpdateNextHopStatus(
      const std::vector<NexthopStatus>& nexthopStatusList);

  /**
   * @brief Retrieves a nexthop's status while registering it with the RIB.
   *
   * This method is called by the RIB thread to:
   * 1. Check if the specified nexthop IP exists in the cache
   * 2. Register this nexthop as being referenced by the RIB
   * 3. Return its current status information
   *
   * If the nexthop is not found in the cache, this method:
   * - Creates a new entry with default values (unreachable status, no IGP cost)
   * - Sets the registration flag to true
   * - Returns this newly created default status
   *
   * @param nexthopIp The IP address of the nexthop to check and register
   * @return NexthopStatus object containing the current or default status
   */
  NexthopStatus registerAndGetNexthopStatus(const folly::IPAddress& nexthopIp);

  /**
   * @brief Remove nexthop status from the map if nexthopIp exists in the map
   *        and is unreachable. If reachable, just unregister it from RIB.
   *        Called by RIB thread when nexthopAssociation becomes empty.
   * @param nexthopIp IP address of the nexthop to remove
   * @return true if the nexthop was removed, false otherwise
   */
  bool unregisterAndRemoveNexthopStatus(const folly::IPAddress& nexthopIp);

 private:
  /**
   * Nexthop address -> NexthopStatusWithRegistration map
   *
   * This map stores the reachability, IGP cost, isConnected flag, and
   * registration information for each nexthop.
   *
   * Write calls: FsdbFibWatcher and FsdbNeighborWatcher push updates via
   *              addOrUpdateNextHopStatus(). Source-priority ensures
   *              NeighborWatcher (isConnected=true) is authoritative for
   *              directly connected nexthops.
   *
   * Read calls: BGP Rib thread reads the nexthopStatuses for best-path
   *             computations via registerAndGetNexthopStatus().
   **/
  folly::Synchronized<
      folly::F14NodeMap<folly::IPAddress, NexthopStatusWithRegistration>>
      nexthopStatusMap_;

  friend class NexthopCacheTestFixture;

  // per class placeholder for test code injection
  // only need to be setup once here
#ifdef NexthopCache_TEST_FRIENDS
  NexthopCache_TEST_FRIENDS
#endif
};

} // namespace facebook::bgp
