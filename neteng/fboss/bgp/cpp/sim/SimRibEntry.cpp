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

#include "neteng/fboss/bgp/cpp/sim/SimRibEntry.h"

#include <fmt/core.h>
#include <fmt/format.h>
#include <folly/logging/xlog.h>
#include <iterator>
#include <optional>
#include <string_view>

#include "neteng/fboss/bgp/cpp/BgpServiceUtil.h"
#include "neteng/fboss/bgp/cpp/rib/RouteFilterConfig.h"
#include "neteng/fboss/bgp/cpp/sim/SimRouteSelector.h"

using facebook::neteng::fboss::bgp::thrift::TBgpPath;
using facebook::neteng::fboss::bgp::thrift::TRibEntry;

namespace facebook::bgp {

namespace {

/*
 * Build multipath filter configs for the simulator.
 * Mirrors getBaseRouteFilterConfigsMultiPath() with configurable feature flags
 * and LOCAL_ROUTE support via SimRouteInfoFilter.
 */
std::vector<nettools::edge::RouteFilterConfig> getSimMultipathConfigs(
    const RoutingTableConfig& config) {
  using namespace nettools::edge;
  std::vector<RouteFilterConfig> configs;

  configs.emplace_back(getMembershipFilterConfig(
      RouteMetric::DELETED_ROUTE,
      MembershipRouteFilterAction::CHOOSE_MEMBER,
      {0}));
  if (config.enableWeightComparison) {
    configs.emplace_back(getHighestLowestRouteFilterConfig(
        RouteMetric::BGP_WEIGHT_VALUE,
        HighestLowestRouteFilterAction::CHOOSE_HIGHEST));
  }
  configs.emplace_back(getHighestLowestRouteFilterConfig(
      RouteMetric::BGP_LOCAL_PREFERENCE,
      HighestLowestRouteFilterAction::CHOOSE_HIGHEST));
  configs.emplace_back(getHighestLowestRouteFilterConfig(
      RouteMetric::LOCAL_ROUTE,
      HighestLowestRouteFilterAction::CHOOSE_HIGHEST));
  if (!config.countConfedsInAsPathLen) {
    configs.emplace_back(getHighestLowestRouteFilterConfig(
        RouteMetric::BGP_AS_PATH_LEN,
        HighestLowestRouteFilterAction::CHOOSE_LOWEST));
  } else {
    configs.emplace_back(getHighestLowestRouteFilterConfig(
        RouteMetric::BGP_AS_PATH_LEN_WITH_CONFED,
        HighestLowestRouteFilterAction::CHOOSE_LOWEST));
  }
  configs.emplace_back(getHighestLowestRouteFilterConfig(
      RouteMetric::BGP_ORIGIN_CODE,
      HighestLowestRouteFilterAction::CHOOSE_LOWEST));
  if (config.enableMedComparison) {
    configs.emplace_back(getHighestLowestRouteFilterConfig(
        RouteMetric::BGP_MED_VALUE,
        HighestLowestRouteFilterAction::CHOOSE_LOWEST));
  }
  if (!config.enableEiBgpMultipath) {
    configs.emplace_back(getHighestLowestRouteFilterConfig(
        RouteMetric::EXTERNAL_ROUTE,
        HighestLowestRouteFilterAction::CHOOSE_HIGHEST));
  }
  configs.emplace_back(getHighestLowestRouteFilterConfig(
      RouteMetric::CONFED_EXTERNAL_ROUTE,
      HighestLowestRouteFilterAction::CHOOSE_HIGHEST));

  return configs;
}

} // namespace

SimSelectors makeSimSelectors(const RoutingTableConfig& config) {
  return SimSelectors{
      SimRouteSelector(getSimMultipathConfigs(config)),
      SimRouteSelector(getRouteFilterConfigsBestPath(
          CountConfedsInAsPathLen{config.countConfedsInAsPathLen}))};
}

SimRibEntry::SimRibEntry(const folly::CIDRNetwork& prefix) : prefix_(prefix) {}

void SimRibEntry::insertPath(
    const std::string& peerAddr,
    std::shared_ptr<SimRouteInfo> route) {
  paths_[peerAddr] = std::move(route);
  dirty_ = true;
}

void SimRibEntry::withdrawPath(const std::string& peerAddr) {
  if (paths_.erase(peerAddr) > 0) {
    dirty_ = true;
  }
}

void SimRibEntry::selectBestPath(
    const SimRouteSelector& multipathSelector,
    const SimRouteSelector& bestpathSelector) {
  bestPath_.reset();
  multipaths_.clear();

  if (paths_.empty()) {
    dirty_ = false;
    return;
  }

  // Collect all non-deleted paths
  std::vector<std::shared_ptr<SimRouteInfo>> candidates;
  candidates.reserve(paths_.size());
  for (auto& [_, route] : paths_) {
    route->clearRoutePreferred();
    if (!route->getIsRouteDeleted()) {
      candidates.push_back(route);
    }
  }

  if (candidates.empty()) {
    dirty_ = false;
    return;
  }

  // Single path: it's both best and only multipath
  if (candidates.size() == 1) {
    candidates[0]->setRoutePreferred();
    bestPath_ = candidates[0];
    multipaths_.push_back(candidates[0]);
    dirty_ = false;
    return;
  }

  // Step 1: Multipath selection — find ECMP-eligible set
  auto ecmpSet = multipathSelector.selectRoutes(candidates);

  // Step 2: Bestpath selection — from ECMP set, pick single best
  if (!ecmpSet.empty()) {
    auto bestSet = bestpathSelector.selectRoutes(ecmpSet);

    if (bestSet.empty()) {
      throw std::logic_error(
          fmt::format(
              "Bestpath selector returned empty from {} ECMP candidates on {}",
              ecmpSet.size(),
              folly::IPAddress::networkToString(prefix_)));
    }

    multipaths_.clear();
    multipaths_.reserve(ecmpSet.size());
    for (auto& r : ecmpSet) {
      multipaths_.push_back(r);
    }
    bestPath_ = bestSet[0];
    bestPath_->setRoutePreferred();
  } else {
    throw std::logic_error(
        fmt::format(
            "Multipath selector returned empty set for {} candidates on {}",
            candidates.size(),
            folly::IPAddress::networkToString(prefix_)));
  }

  dirty_ = false;
}

TRibEntry SimRibEntry::toTRibEntry() const {
  TRibEntry entry;

  entry.prefix() = createTIpPrefix(prefix_);

  // Populate paths map — use "multiPaths" as the group name
  std::map<std::string, std::vector<TBgpPath>> pathsMap;
  std::vector<TBgpPath> tPaths;
  tPaths.reserve(multipaths_.size());
  std::optional<TBgpPath> tBestPath;
  for (const auto& route : multipaths_) {
    auto tPath = route->toTBgpPath();
    if (bestPath_ && route == bestPath_) {
      tPath.is_best_path() = true;
      tBestPath = tPath;
    }
    tPaths.push_back(std::move(tPath));
  }
  if (!tPaths.empty()) {
    pathsMap["multiPaths"] = std::move(tPaths);
    entry.best_group() = "multiPaths";
  }
  entry.paths() = std::move(pathsMap);

  if (bestPath_) {
    entry.best_next_hop() = createTIpPrefix(bestPath_->attrs->getNexthop());
    if (!tBestPath) {
      // bestPath_ was not present in multipaths_; build it once here.
      tBestPath = bestPath_->toTBgpPath();
      tBestPath->is_best_path() = true;
    }
    entry.best_path() = std::move(*tBestPath);
  }

  return entry;
}

std::string SimRibEntry::toDebugString() const {
  fmt::memory_buffer buf;
  fmt::format_to(
      std::back_inserter(buf),
      "prefix={} paths={} multipaths={} best={}",
      folly::IPAddress::networkToString(prefix_),
      paths_.size(),
      multipaths_.size(),
      bestPath_ ? std::string_view(bestPath_->peerAddr)
                : std::string_view("none"));

  for (const auto& [peer, route] : paths_) {
    fmt::format_to(
        std::back_inserter(buf),
        "\n  [{}] lp={} as_path_len={} med={} weight={} ext={} best={}",
        peer,
        route->getBgpLocalPreference(),
        route->getBgpAsPathLen(),
        route->getBgpMedValue(),
        route->getBgpWeightValue(),
        route->getIsRouteExternal(),
        route->getIsRoutePreferred());
  }

  return fmt::to_string(buf);
}

} // namespace facebook::bgp
