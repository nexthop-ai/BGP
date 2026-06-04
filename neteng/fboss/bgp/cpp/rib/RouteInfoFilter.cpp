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

/*
 * Implementation of RouteFilter wrapper for RouteInfo objects
 */

#include "RouteInfoFilter.h"

#include "neteng/fboss/bgp/cpp/routelib/RouteFilter.h"

using namespace facebook::nettools::edge;

namespace facebook::bgp {

std::unique_ptr<RouteFilter> RouteInfoFilter::fromRouteFilterConfig(
    const RouteFilterConfig& routeFilterConfig) {
  if (routeFilterConfig.metric == RouteMetric::LOCAL_ROUTE) {
    auto routeFilter = RouteFilter::createRouteFilter(
        routeFilterConfig, RouteInfoFilter::getIsRouteLocal);
    return routeFilter;
  }
  return RouteFilter::fromRouteFilterConfig(routeFilterConfig);
}

int64_t RouteInfoFilter::getIsRouteLocal(
    const std::shared_ptr<const RouteBase> ri) {
  return std::static_pointer_cast<const RouteInfo>(ri)->getIsRouteLocal();
}
} // namespace facebook::bgp
