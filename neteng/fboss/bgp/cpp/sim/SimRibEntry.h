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
#include "neteng/fboss/bgp/cpp/sim/SimRouteInfo.h"
#include "neteng/fboss/bgp/cpp/sim/SimRouteSelector.h"

namespace facebook::neteng::fboss::bgp::thrift {
class TRibEntry;
} // namespace facebook::neteng::fboss::bgp::thrift

namespace facebook::bgp {

/*
 * Multipath and bestpath selectors built from a RoutingTableConfig.
 */
struct SimSelectors {
  SimRouteSelector multipath;
  SimRouteSelector bestpath;
};

/*
 * Build the multipath and bestpath selectors from a RoutingTableConfig.
 * Single source of truth for selector construction, used by
 * RoutingTable::runBestPathSelection to build selectors once per batch.
 */
SimSelectors makeSimSelectors(const RoutingTableConfig& config);

/*
 * Per-prefix RIB entry
 *
 * Stores all received paths keyed by peer address and performs synchronous
 * best-path and multipath selection using the production RouteSelector engine
 * with standard filter configs.
 */
class SimRibEntry {
 public:
  explicit SimRibEntry(const folly::CIDRNetwork& prefix);

  // Path management
  void insertPath(
      const std::string& peerAddr,
      std::shared_ptr<SimRouteInfo> route);
  void withdrawPath(const std::string& peerAddr);

  // Best-path selection using pre-built SimRouteSelectors
  void selectBestPath(
      const SimRouteSelector& multipathSelector,
      const SimRouteSelector& bestpathSelector);

  // Accessors
  const folly::CIDRNetwork& prefix() const {
    return prefix_;
  }
  const std::shared_ptr<SimRouteInfo>& getBestPath() const {
    return bestPath_;
  }
  const std::vector<std::shared_ptr<SimRouteInfo>>& getMultipaths() const {
    return multipaths_;
  }
  const folly::F14FastMap<std::string, std::shared_ptr<SimRouteInfo>>&
  getAllPaths() const {
    return paths_;
  }
  bool isDirty() const {
    return dirty_;
  }
  bool isEmpty() const {
    return paths_.empty();
  }

  // Export
  facebook::neteng::fboss::bgp::thrift::TRibEntry toTRibEntry() const;
  std::string toDebugString() const;

 private:
  folly::CIDRNetwork prefix_;
  folly::F14FastMap<std::string, std::shared_ptr<SimRouteInfo>> paths_;
  std::shared_ptr<SimRouteInfo> bestPath_;
  std::vector<std::shared_ptr<SimRouteInfo>> multipaths_;
  bool dirty_{false};
};

} // namespace facebook::bgp
