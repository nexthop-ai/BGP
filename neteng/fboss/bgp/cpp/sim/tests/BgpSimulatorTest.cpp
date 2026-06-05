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
 * Tests for BgpSimulator — loading switches from configs and resolving peer
 * links from config addresses into direct in-process remote-switch references.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

#include <folly/IPAddress.h>

#include <folly/logging/LoggerDB.h>
#include <folly/logging/test/TestLogHandler.h>

#include "configerator/structs/neteng/fboss/bgp/gen-cpp2/bgp_config_types.h"
#include "configerator/structs/neteng/fboss/bgp/if/gen-cpp2/bgp_attr_types.h"
#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/config/Config.h"
#include "neteng/fboss/bgp/cpp/config/ConfigUtils.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/sim/BgpSimulator.h"
#include "neteng/fboss/bgp/cpp/sim/BgpSwitch.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

namespace facebook::bgp {

namespace {

thrift::BgpConfig makeConfig(const std::string& routerId, int64_t localAs) {
  thrift::BgpConfig config;
  config.router_id() = routerId;
  config.local_as_4_byte() = localAs;
  return config;
}

// A peer talking to neighbor `peerAddr` over local address `localAddr`.
thrift::BgpPeer makePeer(
    const std::string& peerAddr,
    const std::string& localAddr) {
  thrift::BgpPeer peer;
  peer.peer_addr() = peerAddr;
  peer.local_addr() = localAddr;
  peer.next_hop4() = localAddr;
  peer.next_hop6() = "::1";
  peer.remote_as_4_byte() = 65000;
  return peer;
}

thrift::BgpNetwork makeNetwork(const std::string& prefix) {
  thrift::BgpNetwork net;
  net.prefix() = prefix;
  return net;
}

// Borrow the sample config's policies (includes PROPAGATE_NOTHING).
auto loadSamplePolicies() {
  const auto path = getAbsoluteFilePath(
      "neteng/fboss/bgp/cpp/tests/sample_configs/stand_alone_conf.json");
  Config config(
      path, /*peerSubnetLbwMap=*/std::nullopt, /*populateConfigDb=*/false);
  return *config.getConfig().policies();
}

// Build a published BgpPath whose AS-path is the given AS sequence.
std::shared_ptr<const BgpPath> makePathWithAsPath(
    const std::vector<uint32_t>& asns) {
  neteng::fboss::bgp_attr::TAsPathSeg seg;
  seg.seg_type() = neteng::fboss::bgp_attr::TAsPathSegType::AS_SEQUENCE;
  seg.asns_4_byte() = std::vector<int64_t>(asns.begin(), asns.end());
  nettools::bgplib::BgpPathC pathC;
  pathC.nexthop = folly::IPAddress("10.0.0.1");
  nettools::bgplib::BgpAttributesC attrs;
  attrs.origin = nettools::bgplib::BgpAttrOrigin::BGP_ORIGIN_IGP;
  attrs.localPref = 100;
  attrs.asPath = createBgpAttrAsPathDedup({seg});
  pathC.attrs = std::move(attrs);
  auto path = std::make_shared<BgpPath>(static_cast<BgpPathFields>(pathC));
  path->publish();
  return path;
}

} // namespace

class BgpSimulatorTest : public ::testing::Test {};

/*
 * Two switches whose peers point at each other's local addresses resolve to
 * reciprocal remote-switch links.
 */
TEST_F(BgpSimulatorTest, TwoSwitchReciprocalLinks) {
  thrift::BgpConfig cfgA = makeConfig("10.0.0.1", 65000);
  cfgA.peers() = {makePeer(/*peerAddr=*/"10.0.0.2", /*localAddr=*/"10.0.0.1")};
  thrift::BgpConfig cfgB = makeConfig("10.0.0.2", 65000);
  cfgB.peers() = {makePeer(/*peerAddr=*/"10.0.0.1", /*localAddr=*/"10.0.0.2")};

  BgpSimulator sim;
  auto switchA = std::make_shared<BgpSwitch>("switchA", cfgA);
  auto switchB = std::make_shared<BgpSwitch>("switchB", cfgB);
  sim.addSwitch(switchA);
  sim.addSwitch(switchB);

  sim.resolvePeerLinks();

  ASSERT_EQ(1u, switchA->peers().size());
  ASSERT_EQ(1u, switchB->peers().size());
  EXPECT_TRUE(switchA->peers().front().isLinked());
  EXPECT_EQ(switchB.get(), switchA->peers().front().getRemoteSwitch().get());
  EXPECT_TRUE(switchB->peers().front().isLinked());
  EXPECT_EQ(switchA.get(), switchB->peers().front().getRemoteSwitch().get());
}

/*
 * A peer whose neighbor address belongs to no modeled switch is left unlinked
 * (models an external / unmodeled neighbor) rather than failing.
 */
TEST_F(BgpSimulatorTest, UnresolvedPeerLeftUnlinked) {
  thrift::BgpConfig cfg = makeConfig("10.0.0.1", 65000);
  cfg.peers() = {
      makePeer(/*peerAddr=*/"10.99.99.99", /*localAddr=*/"10.0.0.1")};

  BgpSimulator sim;
  sim.addSwitch(std::make_shared<BgpSwitch>("switchA", cfg));

  sim.resolvePeerLinks();

  EXPECT_FALSE(sim.switches().front()->peers().front().isLinked());
}

/*
 * loadConfigs() parses a real config file into a BgpSwitch named after the
 * file stem.
 */
TEST_F(BgpSimulatorTest, LoadConfigsFromFile) {
  const auto configPath = getAbsoluteFilePath(
      "neteng/fboss/bgp/cpp/tests/sample_configs/stand_alone_conf.json");

  BgpSimulator sim;
  sim.loadConfigs({configPath});

  ASSERT_EQ(1u, sim.numSwitches());
  EXPECT_EQ("stand_alone_conf", sim.switches().front()->name());
  EXPECT_EQ(65401u, sim.switches().front()->localAsn());
}

/*
 * Two config paths that share a file stem (here the same sample config loaded
 * twice) yield two switches with the same name; both are still loaded and
 * loadConfigs() emits a WARN about the collision. The warning's exact wording
 * is an implementation detail, so assert only that a WARN-level message was
 * emitted rather than coupling to its text.
 */
TEST_F(BgpSimulatorTest, DuplicateSwitchNamesWarn) {
  const auto configPath = getAbsoluteFilePath(
      "neteng/fboss/bgp/cpp/tests/sample_configs/stand_alone_conf.json");

  auto logHandler = std::make_shared<folly::TestLogHandler>();
  folly::LoggerDB::get().getCategory("")->addHandler(logHandler);
  folly::LoggerDB::get().setLevel("", folly::LogLevel::WARN);

  BgpSimulator sim;
  sim.loadConfigs({configPath, configPath});

  EXPECT_EQ(2u, sim.numSwitches());
  const auto& messages = logHandler->getMessages();
  EXPECT_TRUE(
      std::any_of(
          messages.begin(),
          messages.end(),
          [](const auto& entry) {
            return entry.first.getLevel() >= folly::LogLevel::WARN;
          }))
      << "expected a WARN about the duplicate switch name";
}

} // namespace facebook::bgp
