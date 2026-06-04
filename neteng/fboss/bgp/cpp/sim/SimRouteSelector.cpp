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

#include "neteng/fboss/bgp/cpp/sim/SimRouteSelector.h"

#include "neteng/fboss/bgp/cpp/sim/SimRouteInfoFilter.h"

namespace facebook::bgp {

using nettools::edge::RouteBase;
using nettools::edge::RouteFilterConfig;
using std::shared_ptr;
using std::static_pointer_cast;
using std::vector;

SimRouteSelector::SimRouteSelector(
    const vector<RouteFilterConfig>& routeFilterConfigs) {
  for (const auto& config : routeFilterConfigs) {
    filters_.push_back(SimRouteInfoFilter::fromRouteFilterConfig(config));
  }
}

vector<shared_ptr<SimRouteInfo>> SimRouteSelector::selectRoutes(
    const vector<shared_ptr<SimRouteInfo>>& routes) const {
  vector<shared_ptr<RouteBase>> rawRoutes;
  rawRoutes.reserve(routes.size());
  for (const auto& route : routes) {
    rawRoutes.emplace_back(static_pointer_cast<RouteBase>(route));
  }

  const auto selectedRoutes =
      RouteSelector::selectRoutesAndSetRejectionFilter(rawRoutes);

  vector<shared_ptr<SimRouteInfo>> selectedSimRoutes;
  selectedSimRoutes.reserve(selectedRoutes.size());
  for (const auto& rawRoute : selectedRoutes) {
    selectedSimRoutes.emplace_back(static_pointer_cast<SimRouteInfo>(rawRoute));
  }
  return selectedSimRoutes;
}

} // namespace facebook::bgp
