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
 * Tests for BgpSwitch construction — extraction of global identity from a
 * BgpConfig, building per-session BgpPeers with peer > peer-group inheritance,
 * filling local ASN / router-id from global config, and creating a
 * PolicyManager iff policies are configured.
 */

#include <gtest/gtest.h>

#include <folly/IPAddress.h>

#include "configerator/structs/neteng/fboss/bgp/gen-cpp2/bgp_config_types.h"
#include "neteng/fboss/bgp/cpp/config/Config.h"
#include "neteng/fboss/bgp/cpp/sim/BgpSwitch.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

namespace facebook::bgp {

namespace {

// A peer with the non-optional address fields populated.
thrift::BgpPeer makeMinimalPeer(const std::string& peerAddr) {
  thrift::BgpPeer peer;
  peer.peer_addr() = peerAddr;
  peer.local_addr() = "10.0.0.254";
  peer.next_hop4() = "10.0.0.254";
  peer.next_hop6() = "::1";
  return peer;
}

// A config with a router-id and 4-byte local ASN but no peers/policies.
thrift::BgpConfig makeBaseConfig() {
  thrift::BgpConfig config;
  config.router_id() = "10.0.0.1";
  config.local_as_4_byte() = 65000;
  return config;
}

} // namespace

class BgpSwitchTest : public ::testing::Test {};

/*
 * Build a switch from a real on-disk config and verify the global identity,
 * peer count, that the peer inherits local ASN / router-id from global config,
 * and that a PolicyManager is created (the config has policies).
 */
TEST_F(BgpSwitchTest, ConstructFromStandAloneConf) {
  const auto configPath = getAbsoluteFilePath(
      "neteng/fboss/bgp/cpp/tests/sample_configs/stand_alone_conf.json");
  Config parsed(
      configPath,
      /*peerSubnetLbwMap=*/std::nullopt,
      /*populateConfigDb=*/false);

  BgpSwitch sw("rsw001", parsed.getConfig());

  const uint64_t expectedRouterId =
      folly::IPAddress("10.191.0.26").asV4().toLongHBO();
  EXPECT_EQ(expectedRouterId, sw.routerId());
  EXPECT_EQ(65401u, sw.localAsn());
  EXPECT_EQ(2027u, sw.localConfedAsn().value());
  EXPECT_EQ(30u, sw.holdTime());

  ASSERT_EQ(1u, sw.peers().size());
  const BgpPeer& peer = sw.peers().front();
  // The peer is filled with the switch's global local ASN and router-id.
  EXPECT_EQ(65401u, peer.localAsn());
  EXPECT_EQ(expectedRouterId, peer.routerId());
  // local 65401 != remote 65000 -> EXTERNAL.
  EXPECT_EQ(PeerType::EXTERNAL, peer.peerType());

  // Policies are configured -> a PolicyManager exists.
  EXPECT_NE(nullptr, sw.policyManager());
}

/*
 * A peer that omits fields inherits policy and local-AS-related fields from its
 * peer group, while the switch fills in local ASN and router-id from global
 * config. remote_as is the exception: it is peer-only and is NOT inherited from
 * the group (production Config.cpp:609-611, group remote_as* is render-only).
 */
TEST_F(BgpSwitchTest, PeerGroupInheritanceReflected) {
  thrift::BgpConfig config = makeBaseConfig();

  thrift::PeerGroup group;
  group.name() = "fabric";
  group.remote_as_4_byte() = 65010;
  group.ingress_policy_name() = "group_ingress";
  group.egress_policy_name() = "group_egress";
  config.peer_groups() = {group};

  thrift::BgpPeer peer = makeMinimalPeer("10.0.0.2");
  peer.peer_group_name() = "fabric";
  config.peers() = {peer};

  BgpSwitch sw("rsw002", config);

  ASSERT_EQ(1u, sw.peers().size());
  const BgpPeer& built = sw.peers().front();
  // remote_as is peer-only: the group's remote_as_4_byte is NOT inherited.
  EXPECT_EQ(0u, built.remoteAsn());
  EXPECT_EQ("group_ingress", built.ingressPolicyName());
  EXPECT_EQ("group_egress", built.egressPolicyName());
  EXPECT_EQ(65000u, built.localAsn());
  EXPECT_EQ(folly::IPAddress("10.0.0.1").asV4().toLongHBO(), built.routerId());
}

/*
 * No policies configured -> policyManager() is null.
 */
TEST_F(BgpSwitchTest, NoPoliciesMeansNoPolicyManager) {
  thrift::BgpConfig config = makeBaseConfig();
  config.peers() = {makeMinimalPeer("10.0.0.2")};

  BgpSwitch sw("rsw003", config);

  EXPECT_EQ(nullptr, sw.policyManager());
}

/*
 * A peer referencing an undefined peer group is a config error and throws
 * during construction.
 */
TEST_F(BgpSwitchTest, UndefinedPeerGroupThrows) {
  thrift::BgpConfig config = makeBaseConfig();
  thrift::BgpPeer peer = makeMinimalPeer("10.0.0.2");
  peer.peer_group_name() = "does_not_exist";
  config.peers() = {peer};

  EXPECT_THROW(BgpSwitch("rsw004", config), std::runtime_error);
}

/*
 * A config with neither local_as_4_byte nor local_as leaves the local ASN at
 * the reserved/invalid sentinel 0 (RFC 7607), which is rejected during
 * construction.
 */
TEST_F(BgpSwitchTest, MissingLocalAsnThrows) {
  thrift::BgpConfig config;
  config.router_id() = "10.0.0.1";

  EXPECT_THROW(BgpSwitch("rsw005", config), std::runtime_error);
}

} // namespace facebook::bgp
