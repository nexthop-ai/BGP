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

#include <cstdint>

namespace facebook::bgp {

/*
 * Config for the RoutingTable.
 */
struct RoutingTableConfig {
  uint32_t routerId{0};
  uint64_t localAs4Byte{0};
  uint64_t localConfedAs4Byte{0};

  /* Bestpath feature flags — match production
   * getBaseRouteFilterConfigsMultiPath() */
  bool enableMedComparison{false};
  bool enableMedMissingAsWorst{false};
  bool enableWeightComparison{false};
  bool enableEiBgpMultipath{false};
  bool countConfedsInAsPathLen{false};
};

} // namespace facebook::bgp
