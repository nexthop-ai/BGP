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

#include <gtest/gtest.h>
#include "configerator/structs/neteng/fboss/bgp/gen-cpp2/bgp_config_types.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

namespace facebook::bgp {

class ConfigTestFixture : public ::testing::Test {
 public:
  thrift::BgpPeerTimers timers1_;
  thrift::BgpPeerTimers timers2_;

  thrift::RouteLimit preRouteLimit;
  thrift::RouteLimit postRouteLimit;
  thrift::RouteLimit groupPreRouteLimit;
  thrift::RouteLimit groupPostRouteLimit;

  thrift::PeerGroup peergroup1_;

  thrift::BgpPeer dynamicPeer1_;
  thrift::BgpPeer dynamicPeer2_;
  thrift::BgpPeer staticPeer1_;
  thrift::BgpPeer staticPeer2_;
  thrift::BgpPeer staticPeer3_;
  thrift::BgpPeer staticPeer4_;

  thrift::BgpConfig defaultConfig_;

  void SetUp() override;
};

} // namespace facebook::bgp
