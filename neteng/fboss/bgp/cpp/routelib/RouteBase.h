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

#include <vector>

namespace facebook {
namespace nettools {
namespace edge {

struct RouteFilterConfig;

/**
 * Abstract class containing pure virtual functions used by RouteFilter and
 * RouteSelector.
 */
struct RouteBase {
  virtual ~RouteBase() = default;
  virtual uint8_t getBgpPrefixLength() const = 0;
  virtual int64_t getBgpLocalPreference() const = 0;
  virtual int64_t getBgpAsPathLen() const = 0;
  virtual int64_t getBgpAsPathLenWithConfed() const = 0;
  virtual int64_t getBgpOriginCode() const = 0;
  virtual int64_t getBgpMedValue() const = 0;
  virtual uint16_t getBgpWeightValue() const = 0;
  virtual bool getIsRoutePreferred() const = 0;
  virtual void setRoutePreferred() = 0;
  virtual void clearRoutePreferred() = 0;
  virtual uint64_t getBgpRouterId() const = 0;
  virtual __uint128_t getBgpPeerIPAsInt() const = 0;
  virtual __uint128_t getBgpNexthopAsInt() const = 0;
  virtual bool getIsRouteExternal() const = 0;
  virtual bool getIsRouteConfedExternal() const {
    // should be overridden when this is used
    return 0;
  }
  virtual bool getIsRouteDeleted() const = 0;
  virtual void setRouteDeleted() = 0;
  virtual float getUcmpWeight() const {
    return 0.0f;
  }
  virtual std::pair<uint32_t /* origin asn */, uint32_t /* peer asn */>
  getOriginAsnAndPeerAsn() const = 0;
  virtual std::vector<uint32_t> getBgpAsPath() const = 0;
  virtual int64_t getBgpClusterListLen() const = 0;
  virtual std::vector<uint32_t> getBgpClusterList() const = 0;
  virtual void clearBestPathFilterCriteria() {}
  virtual void setBestPathFilterCriteria(
      const RouteFilterConfig& filterConfig) {}
  virtual std::string getBestPathFilterDescr() {
    return "";
  }
  /*
   * Following functions are defined here as RouteFilter uses them
   * currently but we do not know the type of bgp communities in this class.
   */
  virtual int64_t getRouterLevelPreferenceFromControllerCommunities() const = 0;
  virtual int64_t getMetroLevelPreferenceFromControllerCommunities() const = 0;
  virtual uint32_t getIgpCostValue() const = 0;
};
} // namespace edge
} // namespace nettools
} // namespace facebook
