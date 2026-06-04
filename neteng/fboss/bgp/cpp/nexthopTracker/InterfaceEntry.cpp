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

#include "neteng/fboss/bgp/cpp/nexthopTracker/InterfaceEntry.h"
#include "neteng/fboss/bgp/cpp/common/Utils.h"

namespace facebook::bgp {
InterfaceEntry::InterfaceEntry(std::string const& ifName) : ifName_(ifName) {}

bool InterfaceEntry::updateAddr(folly::CIDRNetwork const& addr, bool isValid) {
  bool isUpdated = false;

  auto ips = listAllIPsInCIDR(addr);

  if (isValid) {
    // Add all IPs from the CIDR
    for (const auto& ip : ips) {
      auto result = ipReachabilityMap_.insert({ip, false});
      isUpdated |= result.second;
    }
  } else {
    // Remove all IPs from the CIDR
    for (const auto& ip : ips) {
      isUpdated |= (ipReachabilityMap_.erase(ip) == 1);
    }
  }

  return isUpdated;
}

bool InterfaceEntry::updateIfIndex(int ifIndex) {
  bool isUpdated = false;
  if (ifIndex_ != ifIndex) {
    isUpdated = true;
    ifIndex_ = ifIndex;
  }

  return isUpdated;
}

bool InterfaceEntry::updateReachability(
    const folly::IPAddress& ip,
    bool reachability) {
  auto it = ipReachabilityMap_.find(ip);
  if (it == ipReachabilityMap_.end()) {
    return false;
  }

  if (it->second != reachability) {
    it->second = reachability;
    return true;
  }

  return false;
}

bool InterfaceEntry::updateReachabilityForAllIPs(bool reachability) {
  bool isUpdated = false;

  for (auto& [ip, currentReachability] : ipReachabilityMap_) {
    if (currentReachability != reachability) {
      currentReachability = reachability;
      isUpdated = true;
    }
  }

  return isUpdated;
}

bool InterfaceEntry::isReachable(const folly::IPAddress& ip) const {
  auto it = ipReachabilityMap_.find(ip);
  return it != ipReachabilityMap_.end() ? it->second : false;
}

std::string InterfaceEntry::getIfName() const {
  return ifName_;
}

int InterfaceEntry::getIfIndex() const {
  return ifIndex_;
}

const folly::F14NodeMap<folly::IPAddress, bool>&
InterfaceEntry::getIpReachabilityMap() const {
  return ipReachabilityMap_;
}

} // namespace facebook::bgp
