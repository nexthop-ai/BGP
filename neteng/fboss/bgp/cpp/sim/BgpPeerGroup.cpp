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

#include "neteng/fboss/bgp/cpp/sim/BgpPeerGroup.h"

#include "configerator/structs/neteng/fboss/bgp/gen-cpp2/bgp_config_types.h"

namespace facebook::bgp {

BgpPeerGroup::BgpPeerGroup(const thrift::PeerGroup& peerGroup)
    : name_{*peerGroup.name()},
      description_{peerGroup.description().value_or(std::string{})} {}

} // namespace facebook::bgp
