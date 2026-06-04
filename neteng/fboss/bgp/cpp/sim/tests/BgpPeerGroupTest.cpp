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

/*
 * Tests for BgpPeerGroup — the simulator's thin named peer-group wrapper.
 * Builds thrift::PeerGroup structs in memory and verifies the name and
 * description accessors, with and without an optional description.
 */

#include <gtest/gtest.h>

#include "configerator/structs/neteng/fboss/bgp/gen-cpp2/bgp_config_types.h"
#include "neteng/fboss/bgp/cpp/sim/BgpPeerGroup.h"

namespace facebook::bgp {

class BgpPeerGroupTest : public ::testing::Test {};

// A group with a description exposes both name and description.
TEST_F(BgpPeerGroupTest, NameAndDescription) {
  thrift::PeerGroup group;
  group.name() = "PEERGROUP_A";
  group.description() = "dummy peergroup to test add_path";

  BgpPeerGroup pg(group);

  EXPECT_EQ("PEERGROUP_A", pg.getName());
  EXPECT_EQ("dummy peergroup to test add_path", pg.getDescription());
}

// A group without a description reports an empty description.
TEST_F(BgpPeerGroupTest, NameWithoutDescription) {
  thrift::PeerGroup group;
  group.name() = "PEERGROUP_B";

  BgpPeerGroup pg(group);

  EXPECT_EQ("PEERGROUP_B", pg.getName());
  EXPECT_TRUE(pg.getDescription().empty());
}

} // namespace facebook::bgp
