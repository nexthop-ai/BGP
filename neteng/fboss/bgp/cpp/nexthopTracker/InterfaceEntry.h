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
#include <folly/container/F14Map.h>
#include <folly/io/async/AsyncTimeout.h>

namespace facebook::bgp {

/**
 * Holds interface attributes, and detect changes in interface on every update
 */
class InterfaceEntry final {
 public:
  explicit InterfaceEntry(std::string const& ifName);

  /**
   * Update functions return true if there was a change
   */
  bool updateAddr(folly::CIDRNetwork const& addr, bool isValid);
  bool updateIfIndex(int ifIndex);

  /**
   * Update reachability for a specific IP address. Returns true if changed.
   */
  bool updateReachability(const folly::IPAddress& ip, bool reachability);

  /**
   * Update reachability for all IP addresses on the interface. Returns true if
   * any IP's reachability changed.
   */
  bool updateReachabilityForAllIPs(bool reachability);

  /**
   * Get reachability status for a specific IP address
   */
  bool isReachable(const folly::IPAddress& ip) const;

  std::string getIfName() const;
  int getIfIndex() const;

  /**
   * Get reachability map for all IP addresses
   */
  const folly::F14NodeMap<folly::IPAddress, bool>& getIpReachabilityMap() const;

 private:
  // Interface name
  std::string ifName_;
  // Interface Index
  int ifIndex_{-1};
  // Map of IP address to its reachability status
  folly::F14NodeMap<folly::IPAddress, bool> ipReachabilityMap_{};
};

} // namespace facebook::bgp
