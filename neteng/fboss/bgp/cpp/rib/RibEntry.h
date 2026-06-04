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
#include <folly/IntrusiveList.h>
#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/common/RouteInfo.h"
#include "neteng/fboss/bgp/cpp/common/Structs.h"
#include "neteng/fboss/bgp/cpp/common/Utils.h"
#include "neteng/fboss/bgp/cpp/config/ConfigStructs.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/NexthopCache.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/NexthopInfo.h"
#include "neteng/fboss/bgp/cpp/rib/RouteInfoSelector.h"

namespace facebook::bgp {

class PathSelectionPolicy;

class RibEntry {
 public:
  explicit RibEntry(const folly::CIDRNetwork& prefix) : prefix_(prefix) {}

  /*
   * [Accessor Methods]
   */
  const folly::CIDRNetwork& getPrefix() const {
    return prefix_;
  }

  // best-path calculated V.S. advertised(may lag behind)
  std::shared_ptr<RouteInfo> getBestPath() const {
    return bestpath_;
  }
  std::shared_ptr<RouteInfo> getAdvertisedBestPath() const {
    return advertisedBestpath_;
  }

  // multi-path calculated V.S. advertised(may lag behind)
  const folly::F14NodeMap<uint32_t, std::shared_ptr<RouteInfo>>& getMultipaths()
      const {
    return multipaths_;
  }
  const folly::F14NodeMap<uint32_t, std::shared_ptr<RouteInfo>>&
  getAdvertisedMultipaths() const {
    return advertisedMultipaths_;
  }

  // nexthops with weight calculated V.S. advertised(may lag behind)
  std::shared_ptr<const WeightedNexthopMap> getMultipathWeightedNexthops()
      const {
    return weightedNexthops_;
  }
  std::shared_ptr<const WeightedNexthopMap>
  getAdvertisedMultipathWeightedNexthops() const {
    return advertisedWeightedNexthops_;
  }

  // routeInfo(1-1 mapping to BgpPath) accessor methods
  std::vector<std::shared_ptr<RouteInfo>> getAllPaths() const {
    std::vector<std::shared_ptr<RouteInfo>> vec;
    for (const auto& itr : routeInfos_) {
      for (const auto& routeIter : itr.second) {
        vec.emplace_back(routeIter.second);
      }
    }
    return vec;
  }

  // retrieve size of all paths received from all peers
  uint64_t getAllPathsCnt() const {
    uint64_t cnt{0};
    for (const auto& peerItr : routeInfos_) {
      cnt += peerItr.second.size();
    }
    return cnt;
  }

  // retrieve paths received from one dedicated peer
  folly::F14NodeMap<uint32_t, std::shared_ptr<RouteInfo>> getRouteInfos(
      const nettools::bgplib::BgpPeerId& peer) {
    auto kv = routeInfos_.find(peer);
    if (kv != routeInfos_.end()) {
      return kv->second;
    }
    return {};
  }

  /**
   * Helper method to get a specific RouteInfo by peer and path ID.
   *
   * @param peer The peer ID
   * @param receivedPathId The path ID
   * @return A pointer to the RouteInfo if found, nullptr otherwise
   */
  std::shared_ptr<RouteInfo> getRouteInfo(
      const nettools::bgplib::BgpPeerId& peer,
      uint32_t receivedPathId) const {
    auto kv = routeInfos_.find(peer);
    if (kv != routeInfos_.end()) {
      auto routeInfoIter = kv->second.find(receivedPathId);
      if (routeInfoIter != kv->second.end()) {
        return routeInfoIter->second;
      }
    }
    return nullptr;
  }

  std::string getMultipathNexthopsStr() const;

  // GAR(Global Adaptive Routing) functionality with topo information
  std::shared_ptr<const NexthopTopoInfoMap> getNexthopTopoInfoMap() const {
    return nexthopsAndTopoInfo_;
  }

  // TODO: reconcile this with per-path installToFib flag
  inline bool getInstallToFib() const {
    return installToFib_;
  }

  /**
   * Return aggregate UCMP weight of all ECMP paths. Will be none if any of the
   * ECMP path is missing the UCMP weight community.
   */
  inline std::optional<float> getAggregateReceivedUcmpWeight() const {
    if (aggregateReceivedUcmpWeight_) {
      return aggregateReceivedUcmpWeight_;
    }
    return std::nullopt;
  }

  /**
   * Return aggregate UCMP weight of all ECMP path-peers. Will be none if any
   * of the ECMP path-peer is missing link-bandwidth-bps in configuration.
   */
  inline std::optional<float> getAggregateLocalUcmpWeight() const {
    if (aggregateLocalUcmpWeight_) {
      return aggregateLocalUcmpWeight_;
    }
    return std::nullopt;
  }

  /**
   * Return rib policy set UCMP weight. Will be none if the weight is default
   * zero
   */
  inline std::optional<float> getRibPolicyUcmpWeight() const {
    if (ribPolicyUcmpWeight_ != 0) {
      return ribPolicyUcmpWeight_;
    }
    return std::nullopt;
  }

  /*
   * [Mutator Methods]
   */

  // commit best path selection when it's safe to advertise path back
  // return true if advertisedBestpath_ has changed or aggregated LBW changed
  // which indicates rib needs to advertise new path to peers
  bool commitBestpath() {
    bool ret = (advertisedBestpath_ != bestpath_);
    ret |=
        (advertisedAggregateReceivedUcmpWeight_ !=
         aggregateReceivedUcmpWeight_);
    ret |= (advertisedAggregateLocalUcmpWeight_ != aggregateLocalUcmpWeight_);
    ret |= (advertisedRibPolicyUcmpWeight_ != ribPolicyUcmpWeight_);

    advertisedAggregateReceivedUcmpWeight_ = aggregateReceivedUcmpWeight_;
    advertisedAggregateLocalUcmpWeight_ = aggregateLocalUcmpWeight_;
    advertisedRibPolicyUcmpWeight_ = ribPolicyUcmpWeight_;

    if (!advertisedBestpath_ && !bestpath_) {
      // if old and new bestpaths are nullptr don't bother advertise anything
      return false;
    }
    advertisedBestpath_ = bestpath_;
    return ret;
  }

  bool commitMultipathNexthops() {
    bool ret = (advertisedWeightedNexthops_ != weightedNexthops_);
    advertisedWeightedNexthops_ = weightedNexthops_;
    return ret;
  }

  bool multipathChanged() const {
    return advertisedMultipaths_ != multipaths_;
  }

  // only used in add path feature to help determine if multipaths has
  // been changed.
  bool commitMultipaths() {
    bool ret = (advertisedMultipaths_ != multipaths_);
    advertisedMultipaths_ = multipaths_;
    return ret;
  }

  // update routeInfos based on that peer
  // return true if a new route is added
  //             or an existing route is updated with new attributes
  //        false if there's a same route with the same attributes exists
  bool updatePath(
      const TinyPeerInfo& peer,
      std::shared_ptr<const BgpPath> attrs,
      const bool installToFib = true,
      const uint32_t receivedPathId = kDefaultPathID,
      std::optional<facebook::bgp::NexthopInfo*> nexthopInfo =
          std::nullopt) noexcept;

  // Select the bestpath for this entry. The return value is used to indicate
  // if a new bestpath or ECMP nexthops is selected.
  std::pair<bool, bool> selectBestPath(
      const std::unique_ptr<RouteInfoSelector>& multipathSelector,
      const std::unique_ptr<RouteInfoSelector>& bestpathSelector,
      bool computeUcmp,
      uint32_t ucmpWidth,
      std::optional<BgpUcmpQuantizer> quantizer = std::nullopt,
      const std::unique_ptr<PathSelectionPolicy>& pathSelectionPolicy = nullptr,
      bool enableRibAllocatedPathId = false) noexcept;

  // Check if the entry is already on Fib batch processing list by checking
  // if the list hook is linked or not.
  bool isOnFibBatchList() const {
    return fibBatchListHook_.is_linked();
  }

  /**
   * Set rib policy ucmp
   */
  inline void setRibPolicyUcmpWeight(const float ribPolicyUcmpWeight) {
    ribPolicyUcmpWeight_ = ribPolicyUcmpWeight;
  }

  inline bool needPathSelection() const {
    return needPathSelection_;
  }

  /**
   * Get the RIB version when this entry was last updated (best path or
   * multipath change).
   */
  inline uint64_t getRibVersion() const {
    return ribVersion_;
  }

  /**
   * Set the RIB version for this entry. Called by Rib when a material
   * change occurs (best path or multipath change).
   */
  inline void setRibVersion(uint64_t version) {
    ribVersion_ = version;
  }

  // Set the needPathSelection_ = true so that the entry will be picked up for
  // path selection when preparing the Fib programming
  inline void requirePathSelection() {
    needPathSelection_ = true;
  }

  /**
   * @brief Override route's weighted next-hops with the supplied value.
   *
   * @param weightedNexthops  Pre-computed collection of next-hop addresses and
   * corresponding weights.
   */
  void overrideWeightedNexthops(WeightedNexthopMap& weigtedNexthops);

  // gets the first ID from this entry's free interval, to be assigned to
  // selected paths
  uint32_t getPathIdToSend();

 private:
  // v4 or v6 prefix for this RibEntry
  const folly::CIDRNetwork prefix_;

  // core data structure containing BgpPath/BgpPeer/etc.
  // {peerId: {receivedPathID: routeInfo}}
  folly::F14NodeMap<
      nettools::bgplib::BgpPeerId,
      folly::F14NodeMap<uint32_t, std::shared_ptr<RouteInfo>>>
      routeInfos_;

  /*
   * Aggregated value of received UCMP weight of all ECMP paths. Will be 0 if
   * any ECMP path is missing the UCMP weight.
   */
  float aggregateReceivedUcmpWeight_{0};
  float advertisedAggregateReceivedUcmpWeight_{0};

  /*
   * Aggregated value of peer link bandwidth bps of all ECMP path-peers. Will
   * be 0 if any ECMP path-peer doesn't have link-bps specified
   */
  float aggregateLocalUcmpWeight_{0};
  float advertisedAggregateLocalUcmpWeight_{0};

  /*
   * link bandwidth bps set by rib policy. Will be 0 if no rib policy specified.
   */
  float ribPolicyUcmpWeight_{0};
  float advertisedRibPolicyUcmpWeight_{0};

  /*
   * This is set to true when:
   *  1. updatePath is called and any path is changed;
   *  2. requirePathSelection is called;
   *
   * NOTE: flag is set to false after selectBestPath is called.
   */
  bool needPathSelection_{true};

  // Multipath nexthop and associated weights
  std::shared_ptr<const WeightedNexthopMap> weightedNexthops_;
  std::shared_ptr<const WeightedNexthopMap> advertisedWeightedNexthops_;

  // Multipath nexthop and associated topology information
  std::shared_ptr<const NexthopTopoInfoMap> nexthopsAndTopoInfo_;

  // map of add path IDs to routeInfos
  folly::F14NodeMap<uint32_t, std::shared_ptr<RouteInfo>> multipaths_{};
  folly::F14NodeMap<uint32_t, std::shared_ptr<RouteInfo>>
      advertisedMultipaths_{};

  // bestpath routeInfo
  std::shared_ptr<RouteInfo> bestpath_;
  std::shared_ptr<RouteInfo> advertisedBestpath_;

  // Time stmap when the entry was first installed in local-RIB
  std::chrono::time_point<std::chrono::system_clock> installTimeStamp_;

  // false if bestpath is local route and install_to_fib is configured false;
  // true, otherwise.
  bool installToFib_{true};

  // ucmp active indicator
  bool isUcmpActive_{false};

  /*
   * RIB version when this entry was last updated (best path or multipath
   * change). Used for tracking routing table version per prefix.
   */
  uint64_t ribVersion_{0};

  folly::IntrusiveListHook fibBatchListHook_;

  /**
   * @brief Helper method to unlink a RouteInfo from a NexthopInfo
   *
   * @param peerId The peer ID
   * @param receivedPathId The path ID
   * @param nexthopInfo Optional pointer to the NexthopInfo
   */
  void unlinkRouteInfoFromNexthop(
      const nettools::bgplib::BgpPeerId& peerId,
      const uint32_t receivedPathId,
      std::optional<facebook::bgp::NexthopInfo*> nexthopInfo);

  /**
   * @brief Helper method to link a RouteInfo to a NexthopInfo
   *
   * @param routeInfo The RouteInfo to link
   * @param nexthopInfo Optional pointer to the NexthopInfo
   */
  void linkRouteInfoToNexthop(
      RouteInfo& routeInfo,
      std::optional<facebook::bgp::NexthopInfo*> nexthopInfo);

  // Rib needs to be able to access this list hook for its operation
  friend class RibBase;

  // store a free ID interval for this prefix, starting as [minPathId,
  // maxPathId] (inclusive)
  std::pair<uint32_t, uint32_t> freePathIdInterval_{
      kMinPathIDToSend,
      kMaxPathIDToSend};

#ifdef RibEntry_TEST_FRIENDS
  RibEntry_TEST_FRIENDS
#endif
};
} // namespace facebook::bgp
