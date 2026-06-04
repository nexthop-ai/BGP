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

#include <fmt/format.h>
#include <folly/logging/xlog.h>
#include <numeric>

#include <configerator/structs/neteng/fboss/bgp/gen-cpp2/bgp_config_types.h>
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/NexthopInfo.h"
#include "neteng/fboss/bgp/cpp/rib/RibEntry.h"
#include "neteng/fboss/bgp/cpp/rib/RibPolicy.h"
#include "neteng/fboss/bgp/cpp/rib/Utils.h"
#include "neteng/fboss/bgp/cpp/stats/Stats.h"

using std::make_pair;
using std::make_shared;
using std::shared_ptr;
using std::vector;

namespace facebook::bgp {
bool RibEntry::updatePath(
    const TinyPeerInfo& peer,
    shared_ptr<const BgpPath> attrs,
    const bool installToFib,
    const uint32_t receivedPathId,
    std::optional<facebook::bgp::NexthopInfo*> nexthopInfo) noexcept {
  nettools::bgplib::BgpPeerId peerId{peer.addr, peer.routerId};
  // withdrawn path
  if (!attrs) {
    if (routeInfos_.find(peerId) != routeInfos_.end()) {
      // nexthopInfo is present only when enableNexthopTracking is set to True
      if (nexthopInfo.has_value() && nexthopInfo.value() != nullptr) {
        unlinkRouteInfoFromNexthop(peerId, receivedPathId, *nexthopInfo);
      }
      auto ret = routeInfos_[peerId].erase(receivedPathId);
      needPathSelection_ |= ret;
      if (routeInfos_[peerId].empty()) {
        RibStats::decrRibPaths();
        routeInfos_.erase(peerId);
      }
      return ret;
    }
    return false;
  }

  // announced path
  auto iter = routeInfos_.find(peerId);
  std::optional<uint32_t> pathIdToSend = std::nullopt;

  // attributes did not change, no need for update
  if (iter != routeInfos_.end()) {
    auto pathIt = iter->second.find(receivedPathId);
    if (pathIt != iter->second.end()) {
      if (pathIt->second->attrs == attrs && pathIt->second->peer == peer) {
        return false;
      }
      // otherwise, proceed with update. Re-use allocated pathID
      pathIdToSend = pathIt->second->pathIdToSend;
    }
  }

  // Create the RouteInfo with ribEntry reference
  auto routeInfo = std::make_shared<RouteInfo>(
      prefix_, peer, attrs, receivedPathId, *this, pathIdToSend, installToFib);

  // Link the RouteInfo to the NexthopInfo if provided
  linkRouteInfoToNexthop(*routeInfo, nexthopInfo);

  if (iter == routeInfos_.end()) {
    folly::F14NodeMap<uint32_t, std::shared_ptr<RouteInfo>> info{
        {receivedPathId, std::move(routeInfo)}};
    // first time receive a route from peer for this prefix
    RibStats::incrRibPaths();
    routeInfos_.emplace(peerId, std::move(info));
  } else {
    iter->second.insert_or_assign(receivedPathId, std::move(routeInfo));
  }
  needPathSelection_ = true;
  return true;
}

std::pair<bool, bool> RibEntry::selectBestPath(
    const std::unique_ptr<RouteInfoSelector>& multipathSelector,
    const std::unique_ptr<RouteInfoSelector>& bestpathSelector,
    bool computeUcmp,
    uint32_t ucmpWidth,
    std::optional<BgpUcmpQuantizer> quantizer,
    const std::unique_ptr<PathSelectionPolicy>& pathSelectionPolicy,
    bool enableRibAllocatedPathId) noexcept {
  // reset path selection flag
  needPathSelection_ = false;

  auto oldBestpath = bestpath_;
  auto const oldAggregateReceivedUcmpWeight = aggregateReceivedUcmpWeight_;
  auto const oldAggregateLocalUcmpWeight = aggregateLocalUcmpWeight_;
  auto oldMultipathWeightedNexthops = weightedNexthops_;
  auto oldNexthopAndTopoInfo = nexthopsAndTopoInfo_;
  auto oldMultipaths = multipaths_;
  aggregateReceivedUcmpWeight_ = 0;
  aggregateLocalUcmpWeight_ = 0;
  const FeatureFlags::BgpBestpathFeatures& bgpBestpathFeatures =
      FeatureFlags::getBgpBestpathFeatures();

  // reset topo info map
  nexthopsAndTopoInfo_ = nullptr;

  // Select best paths (aka multipaths) from all paths of a prefix
  vector<shared_ptr<RouteInfo>> routes{};
  for (const auto& peerIdAndPaths : routeInfos_) {
    for (const auto& pathIdAndRoute : peerIdAndPaths.second) {
      if (bgpBestpathFeatures.enableNextHopTracking &&
          (!pathIdAndRoute.second->isNextHopReachable())) {
        // skip unreachable paths
        continue;
      }
      routes.emplace_back(pathIdAndRoute.second);
    }
  }

  if (routes.empty()) {
    bestpath_ = nullptr;
    if (enableRibAllocatedPathId) {
      multipaths_ = {};
    }
    installToFib_ = true;
    weightedNexthops_ = nullptr;
    return make_pair<bool, bool>(
        bestpath_ != oldBestpath,
        weightedNexthops_ != oldMultipathWeightedNexthops);
  }

  // Try overriding the multipath selection. Notice that multipaths_ could be
  // empty

  bool failedCpsNativeCriteria = false;
  vector<std::shared_ptr<RouteInfo>> selectedPaths;
  if (pathSelectionPolicy != nullptr) {
    selectedPaths = pathSelectionPolicy->overrideMultipathSelection(
        *this, routes, multipathSelector);
    if (selectedPaths.empty()) {
      // either no path is available or cps rejected all the selected paths
      bestpath_ = nullptr;
      multipaths_ = {};
      installToFib_ = true;
      weightedNexthops_ = nullptr;
      return make_pair<bool, bool>(
          bestpath_ != oldBestpath,
          weightedNexthops_ != oldMultipathWeightedNexthops);
    } else {
      const auto& result =
          pathSelectionPolicy->getPathSelectionPolicyResult(getPrefix());
      if (result &&
          (result->outcome ==
               PathSelectionPolicyResult::Outcome::BGP_FAILED_CPS_MIN_NEXTHOP ||
           result->outcome ==
               PathSelectionPolicyResult::Outcome::
                   BGP_FAILED_CPS_MIN_AGG_LBW)) {
        bestpath_ = nullptr;
        installToFib_ = true;
        failedCpsNativeCriteria = true;
      }
    }
  } else {
    // Otherwise, we fall back to the default path selection criteria
    selectedPaths = multipathSelector->selectRoutes(routes);
  }
  XCHECK(!selectedPaths.empty());

  NexthopTopoInfoMap nexthopsAndTopoInfo;

  // Go through all ECMP paths:
  // - Assign a pathIdToSend if one is not already assigned (meaning it's a
  // newly announced path)
  // - Compute aggregate received and peer UCMP weights of all ECMP paths.
  // - Compute class id
  float lbwMultiplier{BpsPerGBps}; // Gbps or Mbps. Defaults to 1.0
  bool missingReceivedUcmpWeight{false};
  bool missingLocalUcmpWeight{false};

  for (const auto& path : selectedPaths) {
    if (!path->pathIdToSend.has_value()) {
      path->pathIdToSend = getPathIdToSend();
    }

    if (auto topoInfo = path->attrs->getTopologyInfo()) {
      nexthopsAndTopoInfo.emplace(path->attrs->getNexthop(), *topoInfo);
    }

    auto receivedUcmpWeight = path->getUcmpWeight();
    missingReceivedUcmpWeight |= (receivedUcmpWeight == 0);
    aggregateReceivedUcmpWeight_ += receivedUcmpWeight;

    auto localUcmpWeight = path->peer.ucmpWeight;
    missingLocalUcmpWeight |= (!localUcmpWeight.has_value());
    aggregateLocalUcmpWeight_ += localUcmpWeight.value_or(0.0);

    // Update multiplier, choose lowest one (Gbps, Mpbs). We choose lowest
    // multiplier to retain the granularity and let it be limited by specified
    // `ucmpWidth` here or in HW.
    // Received LBW is in Bytes-per second and we convert to bits per second
    auto receivedLbwBps = receivedUcmpWeight * 8.0f;
    if (receivedLbwBps > BpsPerGBps) {
      lbwMultiplier = std::min(lbwMultiplier, BpsPerGBps);
    } else if (receivedLbwBps > BpsPerMBps) {
      lbwMultiplier = std::min(lbwMultiplier, BpsPerMBps);
    } else {
      lbwMultiplier = 1.0; // Lowest value
    }
  }
  bool topoInfoChanged = false;
  if (nexthopsAndTopoInfo.size() == selectedPaths.size()) {
    nexthopsAndTopoInfo_ =
        make_shared<NexthopTopoInfoMap>(std::move(nexthopsAndTopoInfo));
    // We need to know if topo info changed to know whether to re-program to fib
    // It changed if:
    // (1) We have received topo info over all paths, and
    // (2) new topo info is different from old topo info on any path
    topoInfoChanged =
        (oldNexthopAndTopoInfo == nullptr ||
         *oldNexthopAndTopoInfo != *nexthopsAndTopoInfo_);
  }

  // Reset aggregate received UCMP weight if any ECMP path is missing it
  //
  // NOTE: Ideally quantizer can be applied on a per-route basis and become part
  // of bgp policy, due to high policy config complexity we support one global
  // quantizer. we don't have use case to quantize AWP weight, disable it here.
  // more details: T105046248
  if (missingReceivedUcmpWeight) {
    aggregateReceivedUcmpWeight_ = 0;
  }

  // Reset aggregate peer UCMP weight if any ECMP path is missing it
  if (missingLocalUcmpWeight) {
    aggregateLocalUcmpWeight_ = 0;
  } else {
    // apply quantizer if enabled
    if (quantizer.has_value()) {
      aggregateLocalUcmpWeight_ =
          quantizer->quantize(aggregateLocalUcmpWeight_);
    }
  }

  if (!failedCpsNativeCriteria) {
    // update best path
    auto bestpath = bestpathSelector->selectRoutes(selectedPaths);
    CHECK(!bestpath.empty());
    bestpath_ = bestpath[0];
    installToFib_ = bestpath[0]->installToFib;
  }

  // Update multipath nexthops.
  // NOTE: Here we normalize the weights which will be passed to FIB for
  // programming. Weights can be expected to be multiple of Mbps/GBps. We first
  // try to normalize them by removing following
  // 1. Mbps/Gbps multiplier
  // 2. Removing common multiplier
  WeightedNexthopMap newNhWtMap;
  uint32_t weightMultiplier{0};
  uint64_t totalUcmpWeight{0};
  bool setUcmpWeights = computeUcmp && aggregateReceivedUcmpWeight_ > 0;
  std::unordered_set<uint32_t> uniqueNexthopWeights;
  multipaths_ = {};
  for (auto& path : selectedPaths) {
    // move paths to map with pathID to quickly find missing paths by ID when
    // constructing RibOut messages after fib programming.
    // pathId comes from the getPathIdToSend() calls above, performed on each
    // selected path. Thus all selected paths should have a pathIdToSend value
    auto pathId = path->pathIdToSend.value();
    multipaths_.emplace(pathId, std::move(path));
    path = multipaths_.at(pathId);

    // Derive UCMP weight of next-hop from link-bandwidth value of path if
    // applicable else fallback to default weight
    if (setUcmpWeights) {
      // 1. Normalize weight by removing lbwMultiplier
      auto ucmpWeight = static_cast<uint32_t>(
          round(path->getUcmpWeight() * 8 / lbwMultiplier));
      totalUcmpWeight += ucmpWeight;

      // Find new GCD
      weightMultiplier = std::gcd(weightMultiplier, ucmpWeight);

      // Add nexthop with weight if it is non-zero
      newNhWtMap.emplace(path->attrs->getNexthop(), ucmpWeight);
    } else {
      // Default UCMP weight is 0. It is a special weight that indicates ECMP
      // i.e. weight=1
      newNhWtMap.emplace(path->attrs->getNexthop(), 0u);
    }
  }
  // 2. Remove common multiplier
  if (setUcmpWeights) {
    // Remove common multiplier from total weight
    totalUcmpWeight = totalUcmpWeight / std::max(weightMultiplier, 1u);
    for (auto it = newNhWtMap.begin(); it != newNhWtMap.end();) {
      // NOTE: We take `ucmpWeight` by reference to update in subsequent code
      auto& [nh, ucmpWeight] = *it;

      // Remove multiplier from the ucmpWeight
      ucmpWeight = ucmpWeight / std::max(weightMultiplier, 1u);

      // Quantize UCMP weight to specified ucmp-width if weight is still greater
      // than specified maximum width
      if (totalUcmpWeight > ucmpWidth) {
        ucmpWeight = round(ucmpWeight * ucmpWidth * 1.0 / totalUcmpWeight);
      }

      // Remove next-hop if UCMP weight is zero else move to next one
      if (ucmpWeight == 0) {
        it = newNhWtMap.erase(it);
      } else {
        uniqueNexthopWeights.emplace(ucmpWeight);
        ++it;
      }
    }
  }

  // set ucmp active indicator
  isUcmpActive_ = uniqueNexthopWeights.size() > 1;

  // Compare the new generated nexthops with the existing one. If the nexthops
  // are same, instead of creating a new shared_ptr, reuse the existing
  // pointer. This is required as the Rib code will just compare the nexthop
  // pointers to determine if the nexthops stays same or not.
  bool bestpathChanged = false, multipathChanged = false;
  if ((oldMultipathWeightedNexthops == nullptr) ||
      (newNhWtMap != *oldMultipathWeightedNexthops)) {
    weightedNexthops_ = make_shared<WeightedNexthopMap>(std::move(newNhWtMap));
    multipathChanged = true;
  } else {
    multipathChanged = false;
  }
  multipathChanged |= topoInfoChanged;

  if (!oldBestpath && !bestpath_) {
    // if old and new best path are nullptr, don't bother notifying peers
    return std::make_pair(false, multipathChanged);
  }

  bestpathChanged = (oldBestpath != bestpath_) ||
      (aggregateReceivedUcmpWeight_ != oldAggregateReceivedUcmpWeight) ||
      (aggregateLocalUcmpWeight_ != oldAggregateLocalUcmpWeight);

  return std::make_pair(bestpathChanged, multipathChanged);
}

std::string RibEntry::getMultipathNexthopsStr() const {
  std::stringstream sstream;
  sstream << "[";
  if (weightedNexthops_) {
    for (const auto& nhwtPair : *weightedNexthops_) {
      if (&nhwtPair != &(*weightedNexthops_->cbegin())) {
        sstream << ", ";
      }
      sstream << "(" << nhwtPair.first.str() << ":" << nhwtPair.second << ")";
    }
  }
  sstream << "]";
  return sstream.str();
}

/**
 * @brief Override route's weighted next-hops with the supplied value.
 *
 * @param weightedNexthops Pre-computed collection of next-hop addresses and
 * corresponding weights.
 */
void RibEntry::overrideWeightedNexthops(WeightedNexthopMap& weigtedNexthops) {
  weightedNexthops_ =
      make_shared<WeightedNexthopMap>(std::move(weigtedNexthops));
}

void RibEntry::unlinkRouteInfoFromNexthop(
    const nettools::bgplib::BgpPeerId& peerId,
    const uint32_t receivedPathId,
    std::optional<facebook::bgp::NexthopInfo*> nexthopInfo) {
  // Only proceed if nexthopInfo is valid
  if (!nexthopInfo.has_value() || *nexthopInfo == nullptr) {
    return;
  }

  // Get the routeInfo before erasing it
  auto routeInfoIter = routeInfos_[peerId].find(receivedPathId);
  if (routeInfoIter != routeInfos_[peerId].end()) {
    auto& routeInfo = routeInfoIter->second;
    // Unlink the RouteInfo from the NexthopInfo
    (*nexthopInfo)->unlinkRouteInfo(*routeInfo);
    XLOGF(
        DBG2,
        "Unlinked RouteInfo for prefix {} from NexthopInfo for {}",
        folly::IPAddress::networkToString(prefix_),
        (*nexthopInfo)->getNextHop().str());
  }
}

void RibEntry::linkRouteInfoToNexthop(
    RouteInfo& routeInfo,
    std::optional<facebook::bgp::NexthopInfo*> nexthopInfo) {
  if (nexthopInfo.has_value() && *nexthopInfo != nullptr) {
    (*nexthopInfo)->linkRouteInfo(routeInfo);
    XLOGF(
        DBG2,
        "Linked RouteInfo for prefix {} to NexthopInfo for {}",
        folly::IPAddress::networkToString(prefix_),
        (*nexthopInfo)->getNextHop().str());
  }
}

// when a path ID is requested, return the left element of the Rib Entry's free
// interval and then increment it. The interval is exhausted when the left
// element exceeds the right one, in which case a new free interval would need
// to be determined
uint32_t RibEntry::getPathIdToSend() {
  // if the interval has no remaining free path IDs, we need to find the largest
  // free interval among assigned path IDs
  if (freePathIdInterval_.first > freePathIdInterval_.second) {
    freePathIdInterval_ = findLargestFreePathIdInterval(
        routeInfos_, kMinPathIDToSend, kMaxPathIDToSend);
  }

  // take the ID out of the interval and return it
  return freePathIdInterval_.first++;
}

} // namespace facebook::bgp
