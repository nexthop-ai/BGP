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

#include <folly/IPAddress.h>

namespace facebook::bgp {

class BgpPath;
class BgpSwitch;

namespace thrift {
class BgpPeer;
class PeerGroup;
} // namespace thrift

/*
 * Classification of a peering session relative to the local AS.
 *   INTERNAL        - iBGP, peer shares the local AS. Also covers an
 *                     intra-confed peer (confed member that shares the local
 *                     sub-AS), which production treats as internal.
 *   EXTERNAL        - eBGP, peer is in a different (non-confed) AS.
 *   CONFED_EXTERNAL - confederation member (sub-AS) peer with a differing AS
 *                     (production ConfedEBGP). A confed peer that shares the
 *                     local AS is INTERNAL, not CONFED_EXTERNAL. See production
 *                     AdjRibCommonUtils::getBgpSessionType (AdjRibCommon.cpp).
 *
 * This describes the peer session, not a route. Route origin (which also has a
 * LOCAL value for self-originated routes) is a distinct concept owned by the
 * RIB layer (SimRouteInfo::RouteOrigin); BgpSwitch maps PeerType to that.
 */
enum class PeerType : uint8_t {
  INTERNAL,
  EXTERNAL,
  CONFED_EXTERNAL,
};

/*
 * A single route received from a remote switch over this peering session,
 * after ingress policy has been applied. Mirrors the doc's usage:
 *   ReceivedRoute{cidr, fromSwitchName, postPolicyPath}
 */
struct ReceivedRoute {
  folly::CIDRNetwork cidr;
  std::string fromSwitch;
  std::shared_ptr<BgpPath> path;
};

/*
 * Per-session BGP peer config object for the simulator.
 *
 * Resolves the effective configuration of one peering session by applying
 * peer > peer-group inheritance over the thrift config structs. Holds the
 * routes received from the linked remote switch.
 */
class BgpPeer {
 public:
  explicit BgpPeer(
      const thrift::BgpPeer& peerConfig,
      const thrift::PeerGroup* peerGroup = nullptr);

  /*
   * Link this peer to the remote switch it talks to. The simulator wires
   * sessions together after all switches are constructed.
   */
  void setRemoteSwitch(std::shared_ptr<BgpSwitch> remoteSwitch) {
    remoteSwitch_ = std::move(remoteSwitch);
  }

  bool isLinked() const {
    return remoteSwitch_ != nullptr;
  }

  const std::shared_ptr<BgpSwitch>& getRemoteSwitch() const {
    return remoteSwitch_;
  }

  void addReceivedRoute(ReceivedRoute route) {
    receivedRoutes_.push_back(std::move(route));
  }

  const std::vector<ReceivedRoute>& receivedRoutes() const {
    return receivedRoutes_;
  }

  /*
   * Forward-compat setters used by Phase 3 (BgpSwitch) to fill in values that
   * come from global config rather than per-peer config.
   *
   * setLocalAsn applies the global switch ASN. It preserves any per-peer
   * local-AS override (the effective local ASN becomes override ?? globalAsn)
   * and runs production's same-value validation chain (Config.cpp:620-628),
   * which needs the global ASN to evaluate. May throw BgpError.
   */
  void setLocalAsn(uint32_t globalAsn);
  void setRouterId(uint64_t routerId) {
    routerId_ = routerId;
  }

  // Identity accessors
  const folly::IPAddress& peerIp() const {
    return peerIp_;
  }
  const folly::IPAddress& localIp() const {
    return localIp_;
  }
  uint32_t remoteAsn() const {
    return remoteAsn_;
  }
  uint32_t localAsn() const {
    return localAsn_;
  }
  /*
   * The per-peer local-AS override (RFC 7705 local-as), if configured. Mirrors
   * production's peerLocalAs: nullopt means no override and the effective local
   * ASN defaults to the global switch ASN.
   */
  std::optional<uint32_t> localAsOverride() const {
    return localAsOverride_;
  }
  uint64_t routerId() const {
    return routerId_;
  }

  // Nexthop accessors
  const std::string& nextHop4() const {
    return nextHop4_;
  }
  const std::string& nextHop6() const {
    return nextHop6_;
  }

  // Policy accessors
  const std::string& ingressPolicyName() const {
    return ingressPolicyName_;
  }
  const std::string& egressPolicyName() const {
    return egressPolicyName_;
  }

  // Classification accessors
  PeerType peerType() const {
    return computePeerType(localAsn_, remoteAsn_, isConfedPeer_);
  }
  bool isPassive() const {
    return isPassive_;
  }
  bool isRrClient() const {
    return isRrClient_;
  }
  bool isConfedPeer() const {
    return isConfedPeer_;
  }
  bool nextHopSelf() const {
    return nextHopSelf_;
  }
  bool disableIpv4Afi() const {
    return disableIpv4Afi_;
  }
  bool disableIpv6Afi() const {
    return disableIpv6Afi_;
  }

  // Metadata accessors
  const std::string& description() const {
    return description_;
  }
  const std::string& peerGroupName() const {
    return peerGroupName_;
  }

  /*
   * Compute the session classification from AS numbers and the confed flag,
   * mirroring production AdjRibCommonUtils::getBgpSessionType. Equal AS is
   * INTERNAL (this also covers an intra-confed peer that shares the local
   * sub-AS); a confed peer with localAs != remoteAs is CONFED_EXTERNAL;
   * otherwise EXTERNAL.
   */
  static PeerType
  computePeerType(uint32_t localAs, uint32_t remoteAs, bool isConfedPeer);

  // Log a human-readable summary of this peer.
  void printSummary() const;

 private:
  // Identity
  folly::IPAddress peerIp_;
  folly::IPAddress localIp_;
  uint32_t remoteAsn_{0};
  uint32_t localAsn_{0};
  std::optional<uint32_t> localAsOverride_;
  uint64_t routerId_{0};

  // Nexthops
  std::string nextHop4_;
  std::string nextHop6_;

  // Policy names
  std::string ingressPolicyName_;
  std::string egressPolicyName_;

  // Classification
  bool isPassive_{false};
  bool isRrClient_{false};
  bool isConfedPeer_{false};
  bool nextHopSelf_{false};
  bool disableIpv4Afi_{false};
  bool disableIpv6Afi_{false};

  // Metadata
  std::string description_;
  std::string peerGroupName_;

  // Linked remote switch (incomplete type — only held/copied, never built here)
  std::shared_ptr<BgpSwitch> remoteSwitch_;

  // Routes received from the remote switch, post ingress policy
  std::vector<ReceivedRoute> receivedRoutes_;
};

} // namespace facebook::bgp
