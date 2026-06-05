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

#include <folly/CppAttributes.h>
#include <folly/IPAddress.h>

#include "configerator/structs/neteng/fboss/bgp/gen-cpp2/bgp_config_types.h"
#include "neteng/fboss/bgp/cpp/sim/BgpPeer.h"
#include "neteng/fboss/bgp/cpp/sim/RoutingTable.h"

namespace facebook::bgp {

class PolicyManager;

/*
 * Weight applied to locally originated routes so they win path selection over
 * learned routes when weight comparison is enabled. Mirrors the value used by
 * production RibBase::createLocalRoute(); kept sim-local to avoid touching
 * production headers.
 */
constexpr uint32_t kLocalRouteWeight = 1 << 15;

/*
 * One route advertised over a peering session: a prefix and its (post-egress)
 * path. Used as the inter-switch message in propagate/receive.
 */
struct RouteUpdate {
  folly::CIDRNetwork prefix;
  std::shared_ptr<const BgpPath> path;
};

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

  /*
   * Originate the switch's configured networks into the local RIB. Each network
   * becomes a locally originated BgpPath (default IGP origin, default
   * local-pref, local-route weight), optionally transformed/rejected by its
   * origination policy, and inserted so it participates in best-path selection.
   *
   * Idempotent: repeated calls after the first are no-ops.
   */
  void originateRoutes();

  /*
   * Advertise this switch's best paths to every linked peer. For each peer the
   * peer's egress policy is applied; accepted routes are delivered directly to
   * the remote switch's receiveRoutes() (in-process, no TCP).
   */
  void propagateRoutes();

  /*
   * Receive routes advertised by a remote switch over the session described by
   * fromPeer (the sender's peer, identifying the link). Applies this switch's
   * ingress policy, drops AS-path loops, records the routes on the local
   * receiving peer and inserts them into the RIB.
   */
  void receiveRoutes(
      const BgpPeer& fromPeer,
      const std::string& fromSwitchName,
      const std::vector<RouteUpdate>& routes);

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

  // Originate a single configured network into the local RIB.
  void originateNetwork(const thrift::BgpNetwork& network);

  // Candidate prefixes this switch could advertise (originated + received).
  std::vector<folly::CIDRNetwork> advertisablePrefixes() const;

  // The local peer facing neighbor `neighborAddr`, or nullptr if none.
  BgpPeer* FOLLY_NULLABLE findPeerToward(const folly::IPAddress& neighborAddr);

  /*
   * Run a named ingress/egress/origination policy on a path. Returns the
   * (possibly transformed) path, or nullptr if the policy rejected the prefix.
   * Throws if the policy is not configured.
   */
  std::shared_ptr<const BgpPath> applyRoutePolicy(
      const std::string& policyName,
      const folly::CIDRNetwork& prefix,
      const std::shared_ptr<const BgpPath>& path);

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

  // Guards originateRoutes() so routes are originated at most once.
  bool routesOriginated_{false};
};

} // namespace facebook::bgp
