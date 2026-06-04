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

#include <folly/container/F14Map.h>

#include "neteng/fboss/bgp/cpp/sim/RoutingTableConfig.h"
#include "neteng/fboss/bgp/cpp/sim/SimRibEntry.h"

namespace neteng::emulation::emulator {
class FBOSSBgpRoutingInfoCollection;
} // namespace neteng::emulation::emulator

namespace facebook::bgp {

/*
 * Info about a locally originated route.
 */
struct OriginatedRouteInfo {
  folly::CIDRNetwork prefix;
  std::shared_ptr<const BgpPath> attrs;
  std::string policyName;
};

/*
 * Main per-switch local RIB for the BGP simulator.
 *
 * Manages prefix-keyed SimRibEntry instances, supports path
 * insertion/withdrawal (including bulk withdrawal for peer session teardown),
 * locally originated routes, and batch best-path selection.
 *
 * Exports results as FBOSSBgpRoutingInfoCollection.
 */
class RoutingTable {
 public:
  // Sentinel peer address for locally originated routes
  static constexpr std::string_view kLocalPeerAddr = "local";

  explicit RoutingTable(RoutingTableConfig config);

  // Path management
  void insertPath(
      const folly::CIDRNetwork& prefix,
      const std::string& peerAddr,
      std::shared_ptr<SimRouteInfo> route);
  void withdrawPath(
      const folly::CIDRNetwork& prefix,
      const std::string& peerAddr);
  void withdrawAllFromPeer(const std::string& peerAddr);

  // Originated routes
  void addOriginatedRoute(
      const folly::CIDRNetwork& prefix,
      std::shared_ptr<const BgpPath> attrs,
      const std::string& policyName);
  void removeOriginatedRoute(const folly::CIDRNetwork& prefix);

  // Best-path selection
  void runBestPathSelection();

  // Accessors
  const SimRibEntry* getEntry(const folly::CIDRNetwork& prefix) const;
  size_t size() const {
    return entries_.size();
  }
  size_t originatedSize() const {
    return originatedRoutes_.size();
  }
  const RoutingTableConfig& config() const {
    return config_;
  }

  // Export
  void populateCollection(
      ::neteng::emulation::emulator::FBOSSBgpRoutingInfoCollection& out) const;
  std::string toDebugString() const;

 private:
  void populateBgpLocalConfig(
      ::neteng::emulation::emulator::FBOSSBgpRoutingInfoCollection& out) const;
  void populateRibEntries(
      ::neteng::emulation::emulator::FBOSSBgpRoutingInfoCollection& out) const;
  void populateOriginatedRoutes(
      ::neteng::emulation::emulator::FBOSSBgpRoutingInfoCollection& out) const;

  /*
   * Build a SimRouteInfo for a locally originated route using the sentinel
   * peer-IP convention (0.0.0.0 for v4 prefixes, :: for v6).
   */
  std::shared_ptr<SimRouteInfo> makeOriginatedSimRoute(
      const folly::CIDRNetwork& prefix,
      std::shared_ptr<const BgpPath> attrs) const;
  RoutingTableConfig config_;
  folly::F14NodeMap<folly::CIDRNetwork, SimRibEntry> entries_;
  folly::F14NodeMap<folly::CIDRNetwork, OriginatedRouteInfo> originatedRoutes_;
};

} // namespace facebook::bgp
