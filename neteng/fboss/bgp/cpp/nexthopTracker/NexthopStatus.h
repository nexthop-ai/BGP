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
#include <optional>
#include <tuple>
#include "openr/if/gen-cpp2/Network_types.h"
#include "openr/if/gen-cpp2/Platform_types.h"

namespace facebook::bgp {

/**
 * @class NexthopStatus
 * @brief Class to store and manage the nexthop IP address and its IGP cost.
 */
class NexthopStatus {
 public:
  /**
   * @brief Constructor for NexthopStatus
   * @param nexthop The IP address of the nexthop
   * @param isReachable Whether the nexthop is reachable
   * @param igpCost Optional IGP cost to reach the nexthop. If IgpCost is unset,
   * the value is considered as INT_MAX by BGP++
   * @param isConnected whether the nexthop is directly connected or not. If
   * isConnected is unset, then it is considered as unknown.
   */
  explicit NexthopStatus(
      const folly::IPAddress& nexthop,
      bool isReachable,
      std::optional<uint32_t> igpCost = std::nullopt,
      std::optional<bool> isConnected = std::nullopt)
      : nexthop_(nexthop),
        igpCost_(isReachable ? igpCost : std::nullopt),
        isConnected_(isConnected) {}

  /**
   * @brief Constructor for NexthopStatus from openr::thrift::NextHopStatus
   * @param nexthop The IP address of the nexthop
   * @param thriftStatus The thrift NextHopStatus to convert from
   * @param isConnected whether the nexthop is directly connected or not. If
   * isConnected is unset, then it is considered as unknown.
   */
  explicit NexthopStatus(
      const folly::IPAddress& nexthop,
      const openr::thrift::NextHopStatus& thriftStatus,
      std::optional<bool> isConnected = std::nullopt)
      : nexthop_(nexthop), isConnected_(isConnected) {
    bool isReachable = thriftStatus.isReachable().value();
    std::optional<uint32_t> igpCost = thriftStatus.igpCost().to_optional();
    igpCost_ = isReachable ? igpCost : std::nullopt;
  }

  /**
   * @brief Get the nexthop IP address
   * @return The nexthop IP address
   */
  const folly::IPAddress& getNexthop() const {
    return nexthop_;
  }

  /**
   * @brief Check if the nexthop is reachable
   * @return true if the nexthop is reachable, false otherwise
   */
  bool isReachable() const {
    return igpCost_.has_value();
  }

  /**
   * @brief Get the IGP cost to reach the nexthop
   * @return Optional IGP cost, nullopt if the nexthop is unreachable
   */
  std::optional<uint32_t> getIgpCost() const {
    return igpCost_;
  }

  /**
   * @brief Get the whether the nexthop is directly connected or not
   * @return bool, true if the nexthop is directly connected,
   * false if not directly connected, nullopt if unknown
   */
  std::optional<bool> isConnected() const {
    return isConnected_;
  }

 private:
  // v4 or v6 nexthop prefix
  folly::IPAddress nexthop_;

  // If metric/cost to reach a reachable nexthop, nullopt if unreachable
  // If IgpCost is unset, the value is considered as INT_MAX by BGP++
  std::optional<uint32_t> igpCost_;

  // True if the nexthop is directly connected, false if not directly connected,
  // nullopt if unknown
  std::optional<bool> isConnected_;
};

/**
 * @brief A tuple containing a NexthopStatus and a flag indicating if it's
 * registered from RIB This allows tracking both the nexthop status and its
 * registration state separately
 */
using NexthopStatusWithRegistration = std::tuple<NexthopStatus, bool>;

} // namespace facebook::bgp
