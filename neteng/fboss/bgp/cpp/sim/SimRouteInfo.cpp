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

#include "neteng/fboss/bgp/cpp/sim/SimRouteInfo.h"

#include <fmt/core.h>
#include <folly/container/Enumerate.h>
#include <folly/gen/Base.h>

#include "neteng/fboss/bgp/cpp/BgpServiceUtil.h"
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"

namespace facebook::bgp {
SimRouteInfo::SimRouteInfo(
    const folly::CIDRNetwork& prefix,
    std::shared_ptr<const BgpPath> attrs,
    const std::string& peerAddr,
    uint64_t routerId,
    const folly::IPAddress& peerIp,
    RouteOrigin origin,
    bool medMissingAsWorst)
    : prefix(prefix),
      attrs(std::move(attrs)),
      peerAddr(peerAddr),
      routerId(routerId),
      peerIp(peerIp),
      origin(origin),
      peerIpAsInt_(transformIP2Int(peerIp)),
      nexthopAsInt_(transformIP2Int(this->attrs->getNexthop())),
      medMissingAsWorst_(medMissingAsWorst) {}

uint8_t SimRouteInfo::getBgpPrefixLength() const {
  return prefix.second;
}

int64_t SimRouteInfo::getBgpLocalPreference() const {
  return attrs->getLocalPref().value_or(kDefaultLocalPref);
}

int64_t SimRouteInfo::getBgpAsPathLen() const {
  return attrs->getBgpAsPathLen();
}

int64_t SimRouteInfo::getBgpAsPathLenWithConfed() const {
  return attrs->getBgpAsPathLenWithConfed();
}

int64_t SimRouteInfo::getBgpOriginCode() const {
  return static_cast<int64_t>(attrs->getOrigin());
}

int64_t SimRouteInfo::getBgpMedValue() const {
  if (!attrs->getIsMedSet()) {
    return medMissingAsWorst_ ? static_cast<int64_t>(kMedMax) : 0;
  }
  return static_cast<int64_t>(attrs->getMed());
}

uint16_t SimRouteInfo::getBgpWeightValue() const {
  return attrs->getWeight();
}

bool SimRouteInfo::getIsRoutePreferred() const {
  return isPreferred_;
}

void SimRouteInfo::setRoutePreferred() {
  isPreferred_ = true;
}

void SimRouteInfo::clearRoutePreferred() {
  isPreferred_ = false;
}

/*
 * Match production RouteInfo::getBgpRouterId() semantics:
 * combine originator ID (upper 32 bits) with peer router ID (lower 32 bits)
 * to enable tie-breaking when multiple sessions on the same peer inject
 * the same prefix (e.g. VIP injector scenario).
 */
uint64_t SimRouteInfo::getBgpRouterId() const {
  const auto originatorId = attrs->getOriginatorId();
  return ((originatorId ? originatorId : routerId) << 32) + routerId;
}

__uint128_t SimRouteInfo::transformIP2Int(const folly::IPAddress& addr) {
  __uint128_t ipBytes = 0;
  const auto& bytes =
      addr.isV4() ? addr.asV4().toBinary() : addr.asV6().toBinary();
  const auto numBytes = bytes.size();
  for (const auto it : folly::enumerate(bytes)) {
    const auto byteIdx = it.index;
    const auto byteValue = *it;
    const auto intByteNum = numBytes - byteIdx;
    const auto shift = ((intByteNum - 1) * 8);
    ipBytes |= ((__uint128_t)byteValue) << shift;
  }
  return ipBytes;
}

__uint128_t SimRouteInfo::getBgpPeerIPAsInt() const {
  return peerIpAsInt_;
}

__uint128_t SimRouteInfo::getBgpNexthopAsInt() const {
  return nexthopAsInt_;
}

bool SimRouteInfo::getIsRouteExternal() const {
  return origin == RouteOrigin::EXTERNAL;
}

bool SimRouteInfo::getIsRouteConfedExternal() const {
  return origin == RouteOrigin::CONFED_EXTERNAL;
}

bool SimRouteInfo::getIsRouteDeleted() const {
  return isDeleted_;
}

void SimRouteInfo::setRouteDeleted() {
  isDeleted_ = true;
}

std::pair<uint32_t, uint32_t> SimRouteInfo::getOriginAsnAndPeerAsn() const {
  uint32_t originAsn{0};
  uint32_t peerAsn{0};
  bool foundFirst = false;

  for (const auto& segment : attrs->getAsPath().get()) {
    if (!segment.asSequence.empty()) {
      if (!foundFirst) {
        peerAsn = segment.asSequence.front();
        foundFirst = true;
      }
      originAsn = segment.asSequence.back();
    } else if (!segment.asSet.empty()) {
      if (!foundFirst) {
        peerAsn = *segment.asSet.begin();
        foundFirst = true;
      }
      originAsn = *segment.asSet.rbegin();
    }
  }
  return {originAsn, peerAsn};
}

std::vector<uint32_t> SimRouteInfo::getBgpAsPath() const {
  using folly::gen::appendTo;
  using folly::gen::from;

  std::vector<uint32_t> asPath;
  for (const auto& asSeg : attrs->getAsPath().get()) {
    (from(asSeg.asSet) + from(asSeg.asSequence)) | appendTo(asPath);
  }
  return asPath;
}

int64_t SimRouteInfo::getBgpClusterListLen() const {
  const nettools::bgplib::DeDuplicatedClusterList& clusterList =
      attrs->getClusterList();
  return clusterList.nullOrEmpty() ? 0 : clusterList->size();
}

std::vector<uint32_t> SimRouteInfo::getBgpClusterList() const {
  return attrs->getClusterList().get();
}

int64_t SimRouteInfo::getRouterLevelPreferenceFromControllerCommunities()
    const {
  return 0;
}

int64_t SimRouteInfo::getMetroLevelPreferenceFromControllerCommunities() const {
  return 0;
}

uint32_t SimRouteInfo::getIgpCostValue() const {
  return 0;
}

facebook::neteng::fboss::bgp::thrift::TBgpPath SimRouteInfo::toTBgpPath()
    const {
  facebook::neteng::fboss::bgp::thrift::TBgpPath path;

  path.next_hop() = createTIpPrefix(attrs->getNexthop());
  path.local_pref() = getBgpLocalPreference();
  path.origin() = static_cast<int32_t>(attrs->getOrigin());
  path.med() = attrs->getMed();
  path.weight() = attrs->getWeight();
  path.is_best_path() = isPreferred_;
  path.router_id() = routerId;
  path.peer_id() = createTIpPrefix(peerIp);

  /*
   * AS_PATH attribute export (RFC 4271 §5.1.2, RFC 5065 §5).
   *
   * Each BgpAttrAsPathSegmentC carries exactly one of four segment types
   * defined by the BGP protocol — only one sub-field is ever non-empty:
   *
   *  - asSequence   (AS_SEQUENCE, type 2): ordered list of ASNs the route
   *                  traversed; the standard segment in most AS_PATH attrs.
   *  - asSet        (AS_SET, type 1): unordered set of ASNs produced by
   *                  route aggregation (RFC 4271 §9.2.2.2).
   *  - asConfedSequence (AS_CONFED_SEQUENCE, type 3): ordered list of
   *                  Member-AS numbers inside a confederation (RFC 5065).
   *  - asConfedSet  (AS_CONFED_SET, type 4): unordered set of Member-AS
   *                  numbers from aggregation within a confederation.
   *
   * This mutual exclusivity is a protocol-level invariant: each segment on
   * the wire carries a single type byte. The parser (BgpMessageParserUtils)
   * creates a fresh struct per segment and populates only one field, and the
   * serializer (BgpMessageSerializer) enforces numUsedSegmentTypes == 1,
   * throwing INVALID_ASPATH_INFO otherwise. The if/else-if chain below is
   * therefore safe — at most one branch will execute per segment.
   *
   * Note: getBgpAsPath() above concatenates asSet + asSequence from each
   * segment for a flat ASN list, but that is a convenience projection —
   * in practice only one of them is populated per segment.
   */
  std::vector<facebook::neteng::fboss::bgp_attr::TAsPathSeg> asPathSegs;
  for (const auto& seg : attrs->getAsPath().get()) {
    facebook::neteng::fboss::bgp_attr::TAsPathSeg tSeg;
    if (!seg.asSequence.empty()) {
      tSeg.seg_type() =
          facebook::neteng::fboss::bgp_attr::TAsPathSegType::AS_SEQUENCE;
      tSeg.asns_4_byte() =
          std::vector<int64_t>(seg.asSequence.begin(), seg.asSequence.end());
    } else if (!seg.asSet.empty()) {
      tSeg.seg_type() =
          facebook::neteng::fboss::bgp_attr::TAsPathSegType::AS_SET;
      tSeg.asns_4_byte() =
          std::vector<int64_t>(seg.asSet.begin(), seg.asSet.end());
    } else if (!seg.asConfedSequence.empty()) {
      tSeg.seg_type() =
          facebook::neteng::fboss::bgp_attr::TAsPathSegType::AS_CONFED_SEQUENCE;
      tSeg.asns_4_byte() = std::vector<int64_t>(
          seg.asConfedSequence.begin(), seg.asConfedSequence.end());
    } else if (!seg.asConfedSet.empty()) {
      tSeg.seg_type() =
          facebook::neteng::fboss::bgp_attr::TAsPathSegType::AS_CONFED_SET;
      tSeg.asns_4_byte() =
          std::vector<int64_t>(seg.asConfedSet.begin(), seg.asConfedSet.end());
    }
    asPathSegs.push_back(std::move(tSeg));
  }
  path.as_path() = std::move(asPathSegs);

  // Communities
  std::vector<facebook::neteng::fboss::bgp_attr::TBgpCommunity> comms;
  for (const auto& comm : attrs->getCommunities().get()) {
    facebook::neteng::fboss::bgp_attr::TBgpCommunity tComm;
    tComm.asn() = comm.asn;
    tComm.value() = comm.value;
    tComm.community() = (static_cast<int64_t>(comm.asn) << 16) | comm.value;
    comms.push_back(std::move(tComm));
  }
  if (!comms.empty()) {
    path.communities() = std::move(comms);
  }

  // Cluster list
  if (!attrs->getClusterList().nullOrEmpty()) {
    std::vector<int64_t> clusterList;
    for (const auto& cl : attrs->getClusterList().get()) {
      clusterList.push_back(cl);
    }
    path.cluster_list() = std::move(clusterList);
  }

  path.originator_id() = attrs->getOriginatorId();

  return path;
}

std::string SimRouteInfo::toDebugString() const {
  auto asPathVec = getBgpAsPath();
  std::string asPathStr;
  for (size_t i = 0; i < asPathVec.size(); ++i) {
    if (i > 0) {
      asPathStr += " ";
    }
    asPathStr += std::to_string(asPathVec[i]);
  }

  return fmt::format(
      "peer={} prefix={} lp={} as_path=[{}] origin={} med={} weight={} "
      "nh={} best={} route_origin={}",
      peerAddr,
      folly::IPAddress::networkToString(prefix),
      getBgpLocalPreference(),
      asPathStr,
      getBgpOriginCode(),
      getBgpMedValue(),
      getBgpWeightValue(),
      attrs->getNexthop().str(),
      isPreferred_,
      static_cast<int>(origin));
}

} // namespace facebook::bgp
