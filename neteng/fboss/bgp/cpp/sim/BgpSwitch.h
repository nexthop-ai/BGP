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
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "configerator/structs/neteng/fboss/bgp/gen-cpp2/bgp_config_types.h"
#include "neteng/fboss/bgp/cpp/sim/BgpPeer.h"
#include "neteng/fboss/bgp/cpp/sim/RoutingTable.h"

namespace facebook::bgp {

class PolicyManager;

/*
 * A single simulated BGP switch (router).
 *
 * Owns the per-switch view derived from one BgpConfig: the global identity
 * (router-id, local ASN, confed ASN, hold-time), the set of per-session
 * BgpPeer objects (with peer > peer-group inheritance resolved), the
 * production PolicyManager used to evaluate ingress/egress/origination
 * policy, and the local RIB (RoutingTable) on which best-path selection runs.
 *
 * Construction does NOT depend on the production Config runtime
 * (FeatureFlags, netwhoami, fb303). Fields are extracted directly from the
 * deserialized thrift::BgpConfig so a switch can be built cheaply in tests
 * and in the simulator.
 */
class BgpSwitch {
 public:
  BgpSwitch(std::string name, thrift::BgpConfig config);
  ~BgpSwitch();

  // Non-copyable and non-movable: owns a RoutingTable and PolicyManager.
  BgpSwitch(const BgpSwitch&) = delete;
  BgpSwitch& operator=(const BgpSwitch&) = delete;
  BgpSwitch(BgpSwitch&&) = delete;
  BgpSwitch& operator=(BgpSwitch&&) = delete;

  // Identity accessors
  const std::string& name() const {
    return name_;
  }
  uint64_t routerId() const {
    return routerId_;
  }
  uint32_t localAsn() const {
    return localAsn_;
  }
  const std::optional<uint32_t>& localConfedAsn() const {
    return localConfedAsn_;
  }
  uint32_t holdTime() const {
    return holdTime_;
  }

  // Originated networks (captured from config for route origination).
  const std::vector<thrift::BgpNetwork>& networks4() const {
    return networks4_;
  }
  const std::vector<thrift::BgpNetwork>& networks6() const {
    return networks6_;
  }

  /*
   * Peers, mutable so the simulator can resolve remote-switch links after all
   * switches are constructed.
   */
  std::vector<BgpPeer>& peers() {
    return peers_;
  }
  const std::vector<BgpPeer>& peers() const {
    return peers_;
  }

  RoutingTable& routingTable() {
    return routingTable_;
  }
  const RoutingTable& routingTable() const {
    return routingTable_;
  }

  // Null when no policies are configured.
  const PolicyManager* policyManager() const {
    return policyManager_.get();
  }

 private:
  /*
   * Build peers_ from the config, resolving each peer's peer_group_name to the
   * matching thrift::PeerGroup and filling local ASN / router-id from global
   * config. Throws if a peer references an undefined peer group.
   */
  void buildPeers(const thrift::BgpConfig& config);

  /*
   * Mirror Config::createPolicyManager(): construct a PolicyManager when
   * policies are configured, leaving policyManager_ null otherwise.
   */
  void initPolicyManager(const thrift::BgpConfig& config);

  std::string name_;
  uint64_t routerId_{0};
  uint32_t localAsn_{0};
  std::optional<uint32_t> localConfedAsn_;
  uint32_t holdTime_{0};

  std::vector<thrift::BgpNetwork> networks4_;
  std::vector<thrift::BgpNetwork> networks6_;

  RoutingTable routingTable_;
  std::vector<BgpPeer> peers_;
  std::unique_ptr<PolicyManager> policyManager_;
};

} // namespace facebook::bgp
