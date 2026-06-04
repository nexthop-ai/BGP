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

#include "neteng/fboss/bgp/cpp/sim/SimRouteInfoFilter.h"

#include "neteng/fboss/bgp/cpp/sim/SimRouteInfo.h"

using facebook::nettools::edge::RouteBase;
using facebook::nettools::edge::RouteFilter;
using facebook::nettools::edge::RouteFilterConfig;
using facebook::nettools::edge::RouteMetric;

namespace facebook::bgp {

std::unique_ptr<RouteFilter> SimRouteInfoFilter::fromRouteFilterConfig(
    const RouteFilterConfig& routeFilterConfig) {
  if (routeFilterConfig.metric == RouteMetric::LOCAL_ROUTE) {
    return RouteFilter::createRouteFilter(
        routeFilterConfig, SimRouteInfoFilter::getIsRouteLocal);
  }
  return RouteFilter::fromRouteFilterConfig(routeFilterConfig);
}

int64_t SimRouteInfoFilter::getIsRouteLocal(
    const std::shared_ptr<const RouteBase>& ri) {
  return ((static_cast<const SimRouteInfo*>(ri.get())->origin) ==
          RouteOrigin::LOCAL)
      ? 1
      : 0;
}

} // namespace facebook::bgp
