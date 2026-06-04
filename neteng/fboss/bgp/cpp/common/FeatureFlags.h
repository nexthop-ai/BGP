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

#include <folly/container/F14Set.h>
#include "configerator/structs/neteng/fboss/bgp/gen-cpp2/bgp_config_types.h"

/*
 * This file defines the set of all feature flags on bgp++.
 *
 * These flags are set in matryoshka. They correspond to flags defined in
 * .cconf files in matryoshka/bgp_setting directory. The .cconf files
 * are then used to populate the bgp_setting_config of thrift::BgpConfig.
 *
 * In this file, we parse the thrift::BgpConfig and introduce
 * a global set of feature flags in the FeatureFlags namespace.
 * We also include two additional methods to load feature flags
 * and check if features are enabled.
 *
 * Important to note that FeatureFlags::LoadFromThriftConfig should be
 * called before FeatureFlags::IsFeatureEnabled is.
 *
 */

namespace facebook::bgp {

namespace FeatureFlags {

struct BgpBestpathFeatures {
  /*
   * Enable MED comparison in BGP best path selection
   */
  bool enableMedComparison = false;
  /*
   * Enable MED comparison in BGP best path selection
   */
  bool enableMedMissingAsWorst = false;
  /*
   * Enable Weight comparison in BGP best path selection
   */
  bool enableWeightComparison = false;
  /*
   * Enable Next Hop Tracking and IGP Cost Comparison in BGP best path selection
   */
  bool enableNextHopTracking = false;
  /*
   * Enable eiBGP feature in best path selection -- skip EXTERNAL_ROUTE
   * preference filter
   */
  bool enableEiBgpMultipath = false;
};

extern BgpBestpathFeatures bgpBestpathFeatures;
extern folly::F14NodeSet<std::string> features;

void LoadFromThriftConfig(const thrift::BgpConfig& bgpThriftConfig);
bool IsFeatureEnabled(const std::string& feature);
const BgpBestpathFeatures& getBgpBestpathFeatures();

} // namespace FeatureFlags

} // namespace facebook::bgp
