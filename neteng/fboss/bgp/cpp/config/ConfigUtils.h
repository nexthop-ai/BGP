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

#include <neteng/fboss/bgp/cpp/lib/BgpStructs.h>
#include "configerator/structs/neteng/fboss/bgp/if/gen-cpp2/bgp_attr_types.h"

namespace facebook::bgp {
nettools::bgplib::BgpAttrCommunitiesC createBgpAttrCommunitiesC(
    const std::vector<std::string>& communities);
std::vector<nettools::bgplib::BgpAttrCommunity> createBgpAttrCommunityVec(
    const std::vector<std::string>& communities);
nettools::bgplib::DeDuplicatedAsPath createBgpAttrAsPathDedup(
    const std::vector<neteng::fboss::bgp_attr::TAsPathSeg>& asPaths);
} // namespace facebook::bgp
