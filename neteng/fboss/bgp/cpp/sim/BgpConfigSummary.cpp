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

#include "neteng/fboss/bgp/cpp/sim/BgpConfigSummary.h"

#include <string>

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/config/Config.h"

namespace facebook::bgp {

namespace {

void printRouterId(const thrift::BgpConfig& bgpConfig, std::ostream& os) {
  os << "=== BGP Config Summary ===" << "\n";
  if (bgpConfig.router_id()->empty()) {
    os << "Router ID: (not set)" << "\n";
  } else {
    os << "Router ID: " << *bgpConfig.router_id() << "\n";
  }
}

void printAsn(const thrift::BgpConfig& bgpConfig, std::ostream& os) {
  if (bgpConfig.local_as_4_byte().has_value()) {
    os << "Local AS (4-byte): " << *bgpConfig.local_as_4_byte() << "\n";
  } else if (bgpConfig.local_as().has_value()) {
    os << "Local AS: " << *bgpConfig.local_as() << "\n";
  }
}

void printOriginatedRoutes(
    const thrift::BgpConfig& bgpConfig,
    std::ostream& os) {
  os << "\nOriginated Routes:" << "\n";
  if (bgpConfig.networks4()->empty()) {
    os << "  IPv4 networks: (none)" << "\n";
  } else {
    os << "  IPv4 networks: " << bgpConfig.networks4()->size() << "\n";
    for (const auto& net : *bgpConfig.networks4()) {
      os << "    - " << *net.prefix() << "\n";
    }
  }

  if (bgpConfig.networks6()->empty()) {
    os << "  IPv6 networks: (none)" << "\n";
  } else {
    os << "  IPv6 networks: " << bgpConfig.networks6()->size() << "\n";
    for (const auto& net : *bgpConfig.networks6()) {
      os << "    - " << *net.prefix() << "\n";
    }
  }
}

void printPeerCount(const thrift::BgpConfig& bgpConfig, std::ostream& os) {
  if (bgpConfig.peers()->empty()) {
    os << "\nPeer Count: (none)" << "\n";
  } else {
    os << "\nPeer Count: " << bgpConfig.peers()->size() << "\n";
  }
}

void printPeerGroups(const thrift::BgpConfig& bgpConfig, std::ostream& os) {
  if (bgpConfig.peer_groups().has_value()) {
    if (bgpConfig.peer_groups()->empty()) {
      os << "\nPeer Groups: (none)" << "\n";
    } else {
      os << "\nPeer Groups:" << "\n";
      for (const auto& pg : bgpConfig.peer_groups().value()) {
        os << "  - " << *pg.name();
        if (pg.description().has_value()) {
          os << " (" << *pg.description() << ")";
        }
        if (pg.ingress_policy_name().has_value()) {
          os << " [ingress: " << *pg.ingress_policy_name() << "]";
        }
        if (pg.egress_policy_name().has_value()) {
          os << " [egress: " << *pg.egress_policy_name() << "]";
        }
        os << "\n";
      }
    }
  } else {
    os << "\nPeer Groups: (none)" << "\n";
  }
}

void printPeersAndPolicies(
    const thrift::BgpConfig& bgpConfig,
    std::ostream& os) {
  os << "\nPeers & Policies:" << "\n";
  if (bgpConfig.peers()->empty()) {
    os << "  (none)" << "\n";
  } else {
    for (const auto& peer : *bgpConfig.peers()) {
      os << "  " << *peer.peer_addr();
      if (peer.peer_group_name().has_value()) {
        os << " [group: " << *peer.peer_group_name() << "]";
      }
      if (peer.ingress_policy_name().has_value()) {
        os << " ingress=" << *peer.ingress_policy_name();
      }
      if (peer.egress_policy_name().has_value()) {
        os << " egress=" << *peer.egress_policy_name();
      }
      if (peer.remote_as_4_byte().has_value()) {
        os << " remote_as=" << *peer.remote_as_4_byte();
      }
      os << "\n";
    }
  }
}

void printPolicyStatements(
    const thrift::BgpConfig& bgpConfig,
    std::ostream& os) {
  if (bgpConfig.policies().has_value()) {
    const auto& policies = bgpConfig.policies().value();
    os << "\nPolicy Statements: " << policies.bgp_policy_statements()->size()
       << "\n";
    for (const auto& stmt : *policies.bgp_policy_statements()) {
      os << "  - " << *stmt.name() << "\n";
    }
  }
}

} // namespace

void printConfigSummary(const std::string& configFile, std::ostream& os) {
  /*
   * Reuse BGPCPP's existing Config class — it handles:
   *   1. folly::readFile()
   *   2. validateJsonSyntax() via folly::parseJson()
   *   3. SimpleJSONSerializer::deserialize() -> thrift::BgpConfig
   *
   * populateConfigDb=false avoids fb303/FeatureFlags initialization
   * but still gives us the fully deserialized thrift struct.
   */
  std::unique_ptr<Config> config;

  try {
    config = std::make_unique<Config>(
        configFile,
        /*peerSubnetLbwMap=*/std::nullopt,
        /*populateConfigDb=*/false);
  } catch (const std::exception& ex) {
    XLOGF(ERR, "Error parsing config file {}: {}", configFile, ex.what());
    os << "Error parsing config file: " << ex.what() << "\n";
    return;
  }

  const auto& bgpConfig = config->getConfig();

  printRouterId(bgpConfig, os);
  printAsn(bgpConfig, os);
  printOriginatedRoutes(bgpConfig, os);
  printPeerCount(bgpConfig, os);
  printPeerGroups(bgpConfig, os);
  printPeersAndPolicies(bgpConfig, os);
  printPolicyStatements(bgpConfig, os);
}

} // namespace facebook::bgp
