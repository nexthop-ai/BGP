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
 * Tests for the BGP Config Summary tool.
 * Exercises printConfigSummary (the unit under test) against
 * sample configs to verify the output contains expected fields.
 */

#include <gtest/gtest.h>

#include <sstream>

#include "neteng/fboss/bgp/cpp/sim/BgpConfigSummary.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

namespace facebook::bgp {

class BgpConfigSummaryTest : public ::testing::Test {};

/*
 * Test printConfigSummary with bgpcpp-dryrun.conf — exercises 2-byte ASN,
 * zero originated routes, peer policies, and no peer groups.
 */
TEST_F(BgpConfigSummaryTest, printConfigSummaryOutput) {
  auto configFilePath = getAbsoluteFilePath(
      "neteng/fboss/bgp/cpp/tests/sample_configs/bgpcpp-dryrun.conf");

  std::ostringstream oss;
  printConfigSummary(configFilePath, oss);
  std::string output = oss.str();

  // Router identity — 2-byte ASN path
  EXPECT_NE(std::string::npos, output.find("Router ID: 10.46.0.41"));
  EXPECT_NE(std::string::npos, output.find("Local AS: 64551"));

  // Originated routes (none in this config)
  EXPECT_NE(std::string::npos, output.find("IPv4 networks: (none)"));
  EXPECT_NE(std::string::npos, output.find("IPv6 networks: (none)"));

  // Peers
  EXPECT_NE(std::string::npos, output.find("Peer Count: 1"));
  EXPECT_NE(std::string::npos, output.find("ingress=Ingress"));
  EXPECT_NE(std::string::npos, output.find("egress=EgressNameDifferent"));

  // No peer groups in this config
  EXPECT_NE(std::string::npos, output.find("(none)"));
}

/*
 * Test printConfigSummary with stand_alone_conf.json — exercises 2-byte ASN,
 * peer count, absence of peer groups, and policy statements.
 */
TEST_F(BgpConfigSummaryTest, printConfigSummaryStandAloneConfJson) {
  auto configFilePath = getAbsoluteFilePath(
      "neteng/fboss/bgp/cpp/tests/sample_configs/stand_alone_conf.json");

  std::ostringstream oss;
  printConfigSummary(configFilePath, oss);
  std::string output = oss.str();

  // Router identity — ASN path
  EXPECT_NE(std::string::npos, output.find("Router ID: 10.191.0.26"));
  EXPECT_NE(std::string::npos, output.find("Local AS: 65401"));

  // Peer count and addresses
  EXPECT_NE(std::string::npos, output.find("Peer Count: 1"));

  // Peer group
  EXPECT_NE(std::string::npos, output.find("Peer Groups: (none)"));

  // Originated routes
  EXPECT_NE(std::string::npos, output.find("IPv4 networks: (none)"));
  EXPECT_NE(std::string::npos, output.find("IPv6 networks: (none)"));

  // 2 policies
  EXPECT_NE(std::string::npos, output.find("Policy Statements: 2"));
  EXPECT_NE(std::string::npos, output.find(" - RSW-SLB-IN"));
  EXPECT_NE(std::string::npos, output.find(" - PROPAGATE_NOTHING"));
}

/*
 * Test printConfigSummary with a non-existing file — exercises the
 * catch block (lines 30-33): logs an error and returns early without
 * printing any config summary fields.
 */
TEST_F(BgpConfigSummaryTest, printConfigSummaryNonExistingFile) {
  std::ostringstream oss;
  printConfigSummary("/tmp/this_file_does_not_exist.json", oss);
  std::string output = oss.str();

  EXPECT_NE(std::string::npos, output.find("Error parsing config file:"));
  EXPECT_EQ(std::string::npos, output.find("=== BGP Config Summary ==="));
}

/*
 * Test printConfigSummary with bgpd.conf — a fully populated config
 * exercising every output section: router ID, 4-byte ASN, originated
 * routes (IPv4 + IPv6), peers with policies, peer groups with
 * description/ingress/egress, and policy statements.
 */
TEST_F(BgpConfigSummaryTest, FullyPopulatedConfig) {
  auto configFilePath = getAbsoluteFilePath(
      "neteng/fboss/bgp/cpp/tests/sample_configs/bgpd.conf");

  std::ostringstream oss;
  printConfigSummary(configFilePath, oss);
  std::string output = oss.str();

  // Header
  EXPECT_NE(std::string::npos, output.find("=== BGP Config Summary ==="));

  // Router identity — 4-byte ASN
  EXPECT_NE(std::string::npos, output.find("Router ID: 10.46.0.41"));
  EXPECT_NE(std::string::npos, output.find("Local AS (4-byte): 12364551"));

  // Originated routes — both populated
  EXPECT_NE(std::string::npos, output.find("IPv4 networks: 1"));
  EXPECT_NE(std::string::npos, output.find("10.50.139.0/24"));
  EXPECT_NE(std::string::npos, output.find("IPv6 networks: 2"));

  // Peers
  EXPECT_NE(std::string::npos, output.find("Peer Count: 13"));

  // Peer group with description and policies
  EXPECT_NE(std::string::npos, output.find("PEERGROUP_A"));
  EXPECT_NE(
      std::string::npos, output.find("(dummy peergroup to test add_path)"));
  EXPECT_NE(std::string::npos, output.find("[ingress: Ingress]"));
  EXPECT_NE(std::string::npos, output.find("[egress: Egress]"));

  // Policy statements
  EXPECT_NE(std::string::npos, output.find("Policy Statements: 2"));
  EXPECT_NE(std::string::npos, output.find("- Ingress"));
  EXPECT_NE(std::string::npos, output.find("- Egress"));
}

/*
 * Test printConfigSummary with empty_conf.json — a minimal valid config
 * with no fields set. Exercises all the "not set" / "(none)" branches.
 */
TEST_F(BgpConfigSummaryTest, EmptyConfig) {
  auto configFilePath = getAbsoluteFilePath(
      "neteng/fboss/bgp/cpp/tests/sample_configs/empty_conf.json");

  std::ostringstream oss;
  printConfigSummary(configFilePath, oss);
  std::string output = oss.str();

  // Header present
  EXPECT_NE(std::string::npos, output.find("=== BGP Config Summary ==="));

  // Router ID not set
  EXPECT_NE(std::string::npos, output.find("Router ID: (not set)"));

  // No ASN
  EXPECT_EQ(std::string::npos, output.find("Local AS"));

  // No originated routes
  EXPECT_NE(std::string::npos, output.find("IPv4 networks: (none)"));
  EXPECT_NE(std::string::npos, output.find("IPv6 networks: (none)"));

  // No peers
  EXPECT_NE(std::string::npos, output.find("Peer Count: (none)"));

  // No peer groups
  EXPECT_NE(std::string::npos, output.find("Peer Groups: (none)"));

  // No policies
  EXPECT_EQ(std::string::npos, output.find("Policy Statements:"));
}

} // namespace facebook::bgp
