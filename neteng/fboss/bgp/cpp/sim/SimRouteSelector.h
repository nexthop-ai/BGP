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

#include "neteng/fboss/bgp/cpp/routelib/RouteSelector.h"
#include "neteng/fboss/bgp/cpp/sim/SimRouteInfo.h"

namespace facebook::bgp {

/*
 * RouteSelector wrapper for SimRouteInfo objects.
 * Uses SimRouteInfoFilter to construct filters that handle LOCAL_ROUTE,
 * mirroring the production RouteInfoSelector pattern.
 */
class SimRouteSelector : private nettools::edge::RouteSelector {
 public:
  explicit SimRouteSelector(
      const std::vector<nettools::edge::RouteFilterConfig>& routeFilterConfigs);

  std::vector<std::shared_ptr<SimRouteInfo>> selectRoutes(
      const std::vector<std::shared_ptr<SimRouteInfo>>& routes) const;
};

} // namespace facebook::bgp
