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

#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/routelib/RouteBase.h"

namespace facebook::neteng::fboss::bgp::thrift {
class TBgpPath;
} // namespace facebook::neteng::fboss::bgp::thrift

namespace facebook::bgp {

enum class RouteOrigin { INTERNAL, EXTERNAL, CONFED_EXTERNAL, LOCAL };

/*
 * Lightweight RouteBase adapter for the BGP simulator.
 *
 * Wraps a production BgpPath with peer metadata, enabling direct use
 * with the production RouteSelector best-path selection engine.
 * Unlike production RouteInfo, this has no RibEntry coupling.
 */
struct SimRouteInfo : nettools::edge::RouteBase {
  const folly::CIDRNetwork prefix;
  const std::shared_ptr<const BgpPath> attrs;
  const std::string peerAddr;
  const uint64_t routerId;
  const folly::IPAddress peerIp;
  const RouteOrigin origin;

  SimRouteInfo(
      const folly::CIDRNetwork& prefix,
      std::shared_ptr<const BgpPath> attrs,
      const std::string& peerAddr,
      uint64_t routerId,
      const folly::IPAddress& peerIp,
      RouteOrigin origin,
      bool medMissingAsWorst);

  // RouteBase pure virtual methods
  uint8_t getBgpPrefixLength() const override;
  int64_t getBgpLocalPreference() const override;
  int64_t getBgpAsPathLen() const override;
  int64_t getBgpAsPathLenWithConfed() const override;
  int64_t getBgpOriginCode() const override;
  int64_t getBgpMedValue() const override;
  uint16_t getBgpWeightValue() const override;
  bool getIsRoutePreferred() const override;
  void setRoutePreferred() override;
  void clearRoutePreferred() override;
  uint64_t getBgpRouterId() const override;
  __uint128_t getBgpPeerIPAsInt() const override;
  __uint128_t getBgpNexthopAsInt() const override;
  bool getIsRouteExternal() const override;
  bool getIsRouteConfedExternal() const override;
  bool getIsRouteDeleted() const override;
  void setRouteDeleted() override;
  std::pair<uint32_t, uint32_t> getOriginAsnAndPeerAsn() const override;
  std::vector<uint32_t> getBgpAsPath() const override;
  int64_t getBgpClusterListLen() const override;
  std::vector<uint32_t> getBgpClusterList() const override;
  int64_t getRouterLevelPreferenceFromControllerCommunities() const override;
  int64_t getMetroLevelPreferenceFromControllerCommunities() const override;
  uint32_t getIgpCostValue() const override;

  // Thrift export
  facebook::neteng::fboss::bgp::thrift::TBgpPath toTBgpPath() const;

  // Human-readable debug output
  std::string toDebugString() const;

 private:
  static __uint128_t transformIP2Int(const folly::IPAddress& addr);

  const __uint128_t peerIpAsInt_;
  const __uint128_t nexthopAsInt_;
  const bool medMissingAsWorst_{false};
  bool isPreferred_{false};
  bool isDeleted_{false};
};

} // namespace facebook::bgp
