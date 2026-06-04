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

#include "neteng/fboss/bgp/cpp/sim/RoutingTable.h"

#include <fmt/core.h>
#include <folly/logging/xlog.h>

#include "neteng/emulation/emulator/if/gen-cpp2/emulation_routing_dump_types.h"
#include "neteng/fboss/bgp/cpp/BgpServiceUtil.h"
#include "neteng/fboss/bgp/if/gen-cpp2/bgp_thrift_types.h"

using facebook::neteng::fboss::bgp::thrift::TBgpLocalConfig;
using facebook::neteng::fboss::bgp::thrift::TOriginatedRoute;
using facebook::neteng::fboss::bgp::thrift::TRibEntry;

namespace facebook::bgp {

namespace {
/* Sentinel peer-IP for originated (local) routes: no real peer. */
const folly::IPAddress kOriginatedPeerIpV4("0.0.0.0");
const folly::IPAddress kOriginatedPeerIpV6("::");
} // namespace

RoutingTable::RoutingTable(RoutingTableConfig config)
    : config_(std::move(config)) {}

void RoutingTable::insertPath(
    const folly::CIDRNetwork& prefix,
    const std::string& peerAddr,
    std::shared_ptr<SimRouteInfo> route) {
  DCHECK_NE(peerAddr, kLocalPeerAddr)
      << "Use addOriginatedRoute() for locally originated routes";
  auto [it, inserted] = entries_.try_emplace(prefix, prefix);
  it->second.insertPath(peerAddr, std::move(route));
}

void RoutingTable::withdrawPath(
    const folly::CIDRNetwork& prefix,
    const std::string& peerAddr) {
  auto it = entries_.find(prefix);
  if (it == entries_.end()) {
    return;
  }
  it->second.withdrawPath(peerAddr);
  if (peerAddr == kLocalPeerAddr) {
    originatedRoutes_.erase(prefix);
  }
  if (it->second.isEmpty()) {
    entries_.erase(it);
  }
}

void RoutingTable::withdrawAllFromPeer(const std::string& peerAddr) {
  for (auto it = entries_.begin(); it != entries_.end();) {
    it->second.withdrawPath(peerAddr);
    if (it->second.isEmpty()) {
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }
  /*
   * The local peer applies globally to all originated routes, so clear the
   * entire store once rather than erasing per-entry inside the loop.
   */
  if (peerAddr == kLocalPeerAddr) {
    originatedRoutes_.clear();
  }
}

std::shared_ptr<SimRouteInfo> RoutingTable::makeOriginatedSimRoute(
    const folly::CIDRNetwork& prefix,
    std::shared_ptr<const BgpPath> attrs) const {
  const auto& peerIp =
      prefix.first.isV4() ? kOriginatedPeerIpV4 : kOriginatedPeerIpV6;
  return std::make_shared<SimRouteInfo>(
      prefix,
      std::move(attrs),
      std::string(kLocalPeerAddr),
      config_.routerId,
      peerIp,
      RouteOrigin::LOCAL,
      /*medMissingAsWorst=*/config_.enableMedMissingAsWorst);
}

void RoutingTable::addOriginatedRoute(
    const folly::CIDRNetwork& prefix,
    std::shared_ptr<const BgpPath> attrs,
    const std::string& policyName) {
  originatedRoutes_.insert_or_assign(
      prefix, OriginatedRouteInfo{prefix, attrs, policyName});

  /*
   * Insert into entries_ so it participates in best-path selection.
   * Bypass insertPath() to avoid the kLocalPeerAddr DCHECK guard.
   */
  auto [it, inserted] = entries_.try_emplace(prefix, prefix);
  it->second.insertPath(
      std::string(kLocalPeerAddr),
      makeOriginatedSimRoute(prefix, std::move(attrs)));
}

void RoutingTable::removeOriginatedRoute(const folly::CIDRNetwork& prefix) {
  withdrawPath(prefix, std::string(kLocalPeerAddr));
}

void RoutingTable::runBestPathSelection() {
  // Build selectors once per batch, not per-prefix
  const auto selectors = makeSimSelectors(config_);

  for (auto& [_, entry] : entries_) {
    if (entry.isDirty()) {
      entry.selectBestPath(selectors.multipath, selectors.bestpath);
    }
  }
}

const SimRibEntry* RoutingTable::getEntry(
    const folly::CIDRNetwork& prefix) const {
  auto it = entries_.find(prefix);
  return it != entries_.end() ? &it->second : nullptr;
}

void RoutingTable::populateCollection(
    ::neteng::emulation::emulator::FBOSSBgpRoutingInfoCollection& out) const {
  populateBgpLocalConfig(out);
  populateRibEntries(out);
  populateOriginatedRoutes(out);
}

void RoutingTable::populateBgpLocalConfig(
    ::neteng::emulation::emulator::FBOSSBgpRoutingInfoCollection& out) const {
  TBgpLocalConfig localConfig;
  localConfig.my_router_id() = config_.routerId;
  localConfig.local_as_4_byte() = config_.localAs4Byte;
  localConfig.local_confed_as_4_byte() = config_.localConfedAs4Byte;
  out.bgp_local_config() = std::move(localConfig);
}

void RoutingTable::populateRibEntries(
    ::neteng::emulation::emulator::FBOSSBgpRoutingInfoCollection& out) const {
  std::vector<TRibEntry> ribEntries;
  ribEntries.reserve(entries_.size());
  for (const auto& [_, entry] : entries_) {
    ribEntries.push_back(entry.toTRibEntry());
  }
  out.rib_entries() = std::move(ribEntries);
}

void RoutingTable::populateOriginatedRoutes(
    ::neteng::emulation::emulator::FBOSSBgpRoutingInfoCollection& out) const {
  std::vector<TOriginatedRoute> originatedRoutes;
  originatedRoutes.reserve(originatedRoutes_.size());
  for (const auto& [_, orig] : originatedRoutes_) {
    TOriginatedRoute tOrig;
    tOrig.prefix() = createTIpPrefix(orig.prefix);
    if (!orig.policyName.empty()) {
      tOrig.policy_name() = orig.policyName;
    }
    // Stack-allocate SimRouteInfo to reuse toTBgpPath() without heap alloc
    const auto& peerIp =
        orig.prefix.first.isV4() ? kOriginatedPeerIpV4 : kOriginatedPeerIpV6;
    SimRouteInfo simRoute(
        orig.prefix,
        orig.attrs,
        std::string(kLocalPeerAddr),
        config_.routerId,
        peerIp,
        RouteOrigin::LOCAL,
        /*medMissingAsWorst=*/config_.enableMedMissingAsWorst);
    tOrig.path() = simRoute.toTBgpPath();
    originatedRoutes.push_back(std::move(tOrig));
  }
  out.originated_routes() = std::move(originatedRoutes);
}

std::string RoutingTable::toDebugString() const {
  std::string result = fmt::format(
      "RoutingTable routerId={:#x} localAs={} confedAs={} prefixes={} "
      "originated={}",
      config_.routerId,
      config_.localAs4Byte,
      config_.localConfedAs4Byte,
      entries_.size(),
      originatedRoutes_.size());

  for (const auto& [_, orig] : originatedRoutes_) {
    result += fmt::format(
        "\n  originated: {} policy={}",
        folly::IPAddress::networkToString(orig.prefix),
        orig.policyName);
  }

  for (const auto& [_, entry] : entries_) {
    result += "\n  " + entry.toDebugString();
  }

  return result;
}

} // namespace facebook::bgp
