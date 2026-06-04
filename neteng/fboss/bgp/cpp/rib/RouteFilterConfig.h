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

#include "neteng/fboss/bgp/cpp/common/FeatureFlags.h"
#include "neteng/fboss/bgp/cpp/routelib/RouteFilterConfig.h"

namespace facebook {
namespace bgp {
// Rules that emulate BGP best path algo for selecting multipath routes.
inline std::vector<nettools::edge::RouteFilterConfig>
getBaseRouteFilterConfigsMultiPath(
    CountConfedsInAsPathLen countConfedsInAsPathLen = CountConfedsInAsPathLen{
        false}) {
  const FeatureFlags::BgpBestpathFeatures& bgpBestpathFeatures =
      FeatureFlags::getBgpBestpathFeatures();
  std::vector<nettools::edge::RouteFilterConfig> routeFilterConfigs = {};

  routeFilterConfigs.emplace_back(getMembershipFilterConfig(
      nettools::edge::RouteMetric::DELETED_ROUTE,
      nettools::edge::MembershipRouteFilterAction::CHOOSE_MEMBER,
      {0}));
  if (bgpBestpathFeatures.enableWeightComparison) {
    routeFilterConfigs.emplace_back(getHighestLowestRouteFilterConfig(
        nettools::edge::RouteMetric::BGP_WEIGHT_VALUE,
        nettools::edge::HighestLowestRouteFilterAction::CHOOSE_HIGHEST));
  }
  routeFilterConfigs.emplace_back(getHighestLowestRouteFilterConfig(
      nettools::edge::RouteMetric::BGP_LOCAL_PREFERENCE,
      nettools::edge::HighestLowestRouteFilterAction::CHOOSE_HIGHEST));
  routeFilterConfigs.emplace_back(getHighestLowestRouteFilterConfig(
      nettools::edge::RouteMetric::LOCAL_ROUTE,
      nettools::edge::HighestLowestRouteFilterAction::CHOOSE_HIGHEST));
  if (!countConfedsInAsPathLen) {
    routeFilterConfigs.emplace_back(getHighestLowestRouteFilterConfig(
        nettools::edge::RouteMetric::BGP_AS_PATH_LEN,
        nettools::edge::HighestLowestRouteFilterAction::CHOOSE_LOWEST));
  } else {
    routeFilterConfigs.emplace_back(getHighestLowestRouteFilterConfig(
        nettools::edge::RouteMetric::BGP_AS_PATH_LEN_WITH_CONFED,
        nettools::edge::HighestLowestRouteFilterAction::CHOOSE_LOWEST));
  }
  routeFilterConfigs.emplace_back(getHighestLowestRouteFilterConfig(
      nettools::edge::RouteMetric::BGP_ORIGIN_CODE,
      nettools::edge::HighestLowestRouteFilterAction::CHOOSE_LOWEST));
  /*
   * Implementing simple MED tie-breaker with always-compare-med
   */
  if (bgpBestpathFeatures.enableMedComparison) {
    routeFilterConfigs.emplace_back(getHighestLowestRouteFilterConfig(
        nettools::edge::RouteMetric::BGP_MED_VALUE,
        nettools::edge::HighestLowestRouteFilterAction::CHOOSE_LOWEST));
  }
  /*
   * prefer external routes over internal (Confed eBGP and iBGP) routes
   * Skip when eiBGP multipath is enabled to equalize eBGP and iBGP paths
   */
  if (!bgpBestpathFeatures.enableEiBgpMultipath) {
    routeFilterConfigs.emplace_back(getHighestLowestRouteFilterConfig(
        nettools::edge::RouteMetric::EXTERNAL_ROUTE,
        nettools::edge::HighestLowestRouteFilterAction::CHOOSE_HIGHEST));
  }

  /* prefer path with lowest IGP cost for next-hop
   * The IGP cost set is Infinity for all routes for now until we have a way to
   * get it.
   */
  if (bgpBestpathFeatures.enableNextHopTracking) {
    routeFilterConfigs.emplace_back(getHighestLowestRouteFilterConfig(
        nettools::edge::RouteMetric::IGP_COST,
        nettools::edge::HighestLowestRouteFilterAction::CHOOSE_LOWEST));
  }
  /*
   * Non-RFC, FBOSS-specific.
   * prefer confed external routes over internal (iBGP) routes
   */
  routeFilterConfigs.emplace_back(getHighestLowestRouteFilterConfig(
      nettools::edge::RouteMetric::CONFED_EXTERNAL_ROUTE,
      nettools::edge::HighestLowestRouteFilterAction::CHOOSE_HIGHEST));

  return routeFilterConfigs;
}

/*
 * Extends best path computation with multipath tiebreakers (no recovery)
 * to select the bestpath (used for peer-advertisement [no add-path case]).
 * These filters are designed for Juniper's BGP multipath behavior. Note that
 * cluster list tie breaker's position is as per RFC, different from Juniper.
 */
inline std::vector<nettools::edge::RouteFilterConfig>
getRouteFilterConfigsBestPath(
    CountConfedsInAsPathLen countConfedsInAsPathLen = CountConfedsInAsPathLen{
        false}) {
  std::vector<nettools::edge::RouteFilterConfig> routeFilterConfigs = {};
  /*
   * Add longest-path as the first tie-breaker. This rule is
   *  - non-RFC standard
   *  - for centralized path selection specified by RibPolicy
   *  - based on the longest AS-path rule in
   * https://fburl.com/gdoc/8i9f42vr
   */
  if (!countConfedsInAsPathLen) {
    routeFilterConfigs.emplace_back(getHighestLowestRouteFilterConfig(
        nettools::edge::RouteMetric::BGP_AS_PATH_LEN,
        nettools::edge::HighestLowestRouteFilterAction::CHOOSE_HIGHEST));
  } else {
    routeFilterConfigs.emplace_back(getHighestLowestRouteFilterConfig(
        nettools::edge::RouteMetric::BGP_AS_PATH_LEN_WITH_CONFED,
        nettools::edge::HighestLowestRouteFilterAction::CHOOSE_HIGHEST));
  }
  routeFilterConfigs.emplace_back(getHighestLowestRouteFilterConfig(
      nettools::edge::RouteMetric::BGP_ROUTER_ID,
      nettools::edge::HighestLowestRouteFilterAction::CHOOSE_LOWEST));
  routeFilterConfigs.emplace_back(getHighestLowestRouteFilterConfig(
      nettools::edge::RouteMetric::BGP_CLUSTER_LIST_LEN,
      nettools::edge::HighestLowestRouteFilterAction::CHOOSE_LOWEST));
  routeFilterConfigs.emplace_back(getHighestLowestRouteFilterConfig(
      nettools::edge::RouteMetric::BGP_PEER_IP,
      nettools::edge::HighestLowestRouteFilterAction::CHOOSE_LOWEST));
  /*
   * Non-RFC, FBOSS-specific with add-path capability
   * prefer lowest nexthop in __uint128_t format
   */
  routeFilterConfigs.emplace_back(getHighestLowestRouteFilterConfig(
      nettools::edge::RouteMetric::BGP_PEER_NEXTHOP,
      nettools::edge::HighestLowestRouteFilterAction::CHOOSE_LOWEST));
  return routeFilterConfigs;
}
} // namespace bgp
} // namespace facebook
