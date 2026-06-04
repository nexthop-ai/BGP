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

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/adjrib/AdjRib.h"

namespace facebook::bgp {

using namespace facebook::nettools::bgplib;

bool AdjRib::hasAsPathLoop(
    const std::shared_ptr<const facebook::bgp::BgpPath>& attrs,
    const facebook::bgp::PeeringParams& params) {
  // Handle both basic and confed cases
  auto globalAs = facebook::bgp::AsNum(params.globalAs);
  auto localAs = facebook::bgp::AsNum(params.localAs);

  std::optional<facebook::bgp::AsNum> localConfedAs{std::nullopt};
  if (params.isConfedPeer) {
    CHECK(params.asConfedId) << "asConfedId is not set for confed peer";
    CHECK(params.localConfedAs) << "localConfedAs is not set for confed peer";
    localAs = facebook::bgp::AsNum(*params.asConfedId);
    localConfedAs = facebook::bgp::AsNum(*params.localConfedAs);
  }

  // Determine if our AS exists in the as path
  for (const auto& seg : attrs->getAsPath().get()) {
    if ((seg.asSet.find(globalAs) != seg.asSet.cend()) ||
        (seg.asSet.find(localAs) != seg.asSet.cend())) {
      return true;
    }
    if (std::find(seg.asSequence.cbegin(), seg.asSequence.cend(), globalAs) !=
        seg.asSequence.cend()) {
      return true;
    }
    if (std::find(seg.asSequence.cbegin(), seg.asSequence.cend(), localAs) !=
        seg.asSequence.cend()) {
      return true;
    }
    if (localConfedAs &&
        std::find(
            seg.asConfedSequence.begin(),
            seg.asConfedSequence.end(),
            *localConfedAs) != seg.asConfedSequence.end()) {
      return true;
    }
    if (localConfedAs && seg.asConfedSet.count(*localConfedAs)) {
      return true;
    }
  }
  return false;
}

bool AdjRib::hasRRLoop(
    const std::shared_ptr<const facebook::bgp::BgpPath>& attrs,
    const facebook::bgp::PeeringParams& params) {
  auto localBgpId = params.localBgpId.toLongHBO();
  auto clusterId = params.localClusterId.toLongHBO();
  auto isOriginatedByMe = attrs->getOriginatorId() == localBgpId;
  auto& clusterList = attrs->getClusterList();
  bool isFromSameCluster = !clusterList.nullOrEmpty() &&
      std::find(clusterList->cbegin(), clusterList->cend(), clusterId) !=
          clusterList->cend();
  return (isOriginatedByMe || isFromSameCluster);
}

folly::Expected<folly::Unit, std::string> AdjRib::validateAsPath(
    const std::shared_ptr<const facebook::bgp::BgpPath>& attrs,
    bool isConfedEBgpPeer,
    bool isEBgpPeer) {
  const auto& asPathDedup = attrs->getAsPath();
  if (isEBgpPeer && !asPathDedup.nullOrEmpty()) {
    for (const auto& asPathSeg : *asPathDedup) {
      if (asPathSeg.isConfedSegment()) {
        return folly::makeUnexpected<std::string>(
            "Must not have confed in Aspath for EBGP peer");
      }
    }
  }

  if (isConfedEBgpPeer) {
    if (asPathDedup.nullOrEmpty()) {
      return folly::makeUnexpected<std::string>(
          "AsPath must not be empty for confed EBGP peer");
    }
    const auto& asPathSeg = (*asPathDedup)[0];
    if (!asPathSeg.isConfedSegment()) {
      return folly::makeUnexpected<std::string>(
          "Confed EBGP peer, first segment in AsPath must be confed");
    }
  }
  return folly::Unit();
}

} // namespace facebook::bgp
