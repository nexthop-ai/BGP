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
#include "neteng/fboss/bgp/cpp/common/Consts.h"
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

// A network to originate, optionally guarded by an origination policy.
thrift::BgpNetwork makeNetwork(
    const std::string& prefix,
    const std::optional<std::string>& policyName = std::nullopt) {
  thrift::BgpNetwork net;
  net.prefix() = prefix;
  if (policyName.has_value()) {
    net.policy_name() = *policyName;
  }
  return net;
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

  EXPECT_THROW(BgpSwitch("rsw004", config), std::runtime_error);
}

/*
 * Originated networks (no policy) become local routes with production default
 * attributes: IGP origin, default local-pref, and the local-route weight.
 */
TEST_F(BgpSwitchTest, OriginateRoutesDefaultAttributes) {
  thrift::BgpConfig config = makeBaseConfig();
  config.networks4() = {makeNetwork("10.50.0.0/24")};
  config.networks6() = {makeNetwork("2401:db00::/32")};

  BgpSwitch sw("rsw005", config);
  sw.originateRoutes();

  EXPECT_EQ(2u, sw.routingTable().originatedSize());

  const auto v4Prefix = folly::IPAddress::createNetwork("10.50.0.0/24");
  const auto* entry = sw.routingTable().getEntry(v4Prefix);
  ASSERT_NE(nullptr, entry);
  const auto& paths = entry->getAllPaths();
  const auto it = paths.find(std::string(RoutingTable::kLocalPeerAddr));
  ASSERT_NE(it, paths.end());
  const SimRouteInfo& route = *it->second;

  EXPECT_EQ(kDefaultLocalPref, route.getBgpLocalPreference());
  EXPECT_EQ(kLocalRouteWeight, route.getBgpWeightValue());
  EXPECT_EQ(
      static_cast<int64_t>(nettools::bgplib::BgpAttrOrigin::BGP_ORIGIN_IGP),
      route.getBgpOriginCode());

  // The IPv6 network is originated too.
  EXPECT_NE(
      nullptr,
      sw.routingTable().getEntry(
          folly::IPAddress::createNetwork("2401:db00::/32")));
}

/*
 * A network whose origination policy rejects it is dropped, while a sibling
 * network with no policy is still originated.
 */
TEST_F(BgpSwitchTest, OriginationPolicyRejectsRoute) {
  const auto configPath = getAbsoluteFilePath(
      "neteng/fboss/bgp/cpp/tests/sample_configs/stand_alone_conf.json");
  Config parsed(
      configPath,
      /*peerSubnetLbwMap=*/std::nullopt,
      /*populateConfigDb=*/false);

  thrift::BgpConfig config = parsed.getConfig();
  // PROPAGATE_NOTHING is the config's "block all" policy.
  config.networks4() = {
      makeNetwork("10.60.0.0/24"),
      makeNetwork("10.61.0.0/24", "PROPAGATE_NOTHING")};

  BgpSwitch sw("rsw006", config);
  sw.originateRoutes();

  EXPECT_EQ(1u, sw.routingTable().originatedSize());
  EXPECT_NE(
      nullptr,
      sw.routingTable().getEntry(
          folly::IPAddress::createNetwork("10.60.0.0/24")));
  EXPECT_EQ(
      nullptr,
      sw.routingTable().getEntry(
          folly::IPAddress::createNetwork("10.61.0.0/24")));
}

/*
 * originateRoutes() is idempotent: calling it more than once does not
 * re-originate or duplicate routes.
 */
TEST_F(BgpSwitchTest, OriginateRoutesIsIdempotent) {
  thrift::BgpConfig config = makeBaseConfig();
  config.networks4() = {makeNetwork("10.50.0.0/24")};

  BgpSwitch sw("rsw005", config);
  sw.originateRoutes();
  sw.originateRoutes();

  EXPECT_EQ(1u, sw.routingTable().originatedSize());
}

/*
 * A network whose origin value is outside the valid BgpAttrOrigin range is
 * rejected (mirrors RibBase::createLocalRoute validation) and not originated.
 */
TEST_F(BgpSwitchTest, InvalidOriginRejected) {
  thrift::BgpConfig config = makeBaseConfig();
  thrift::BgpNetwork net = makeNetwork("10.70.0.0/24");
  // 99 is well outside the valid BgpAttrOrigin range (IGP/EGP/INCOMPLETE).
  net.origin() = 99;
  config.networks4() = {net};

  BgpSwitch sw("rsw007", config);
  sw.originateRoutes();

  EXPECT_EQ(0u, sw.routingTable().originatedSize());
  EXPECT_EQ(
      nullptr,
      sw.routingTable().getEntry(
          folly::IPAddress::createNetwork("10.70.0.0/24")));
}

/*
 * A network that references an origination policy which is not configured is a
 * config error: originateNetwork() throws. Here no policies are configured at
 * all (policyManager() is null), so the named policy cannot be present.
 */
TEST_F(BgpSwitchTest, OriginationPolicyNotConfiguredThrows) {
  thrift::BgpConfig config = makeBaseConfig();
  config.networks4() = {makeNetwork("10.80.0.0/24", "missing_policy")};

  BgpSwitch sw("rsw008", config);
  ASSERT_EQ(nullptr, sw.policyManager());

  EXPECT_THROW(sw.originateRoutes(), std::runtime_error);
}

} // namespace facebook::bgp
