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

#include "neteng/fboss/bgp/cpp/sim/BgpPeer.h"

#include <optional>
#include <type_traits>

#include <fmt/format.h>
#include <folly/logging/xlog.h>

#include "configerator/structs/neteng/fboss/bgp/gen-cpp2/bgp_config_types.h"
#include "neteng/fboss/bgp/cpp/common/BgpError.h"

namespace facebook::bgp {

namespace {

/*
 * Convert a thrift (optional) field ref to a std::optional carrying a decayed
 * copy of the value, so the resolution helpers below can treat peer and
 * peer-group fields uniformly.
 */
template <typename Ref>
auto toOpt(const Ref& ref) -> std::optional<std::decay_t<decltype(*ref)>> {
  using ValueT = std::decay_t<decltype(*ref)>;
  if (ref.has_value()) {
    return std::optional<ValueT>(*ref);
  }
  return std::nullopt;
}

/*
 * Strip a "/len" suffix from an address string. Passive peers use a subnet
 * (e.g. "10.0.0.0/24") as peer_addr; we only want the address portion.
 */
std::string stripPrefixLen(const std::string& addr) {
  const auto pos = addr.find('/');
  if (pos == std::string::npos) {
    return addr;
  }
  return addr.substr(0, pos);
}

/*
 * Parse an address string into a folly::IPAddress, returning a default-
 * constructed address if parsing fails rather than throwing.
 */
folly::IPAddress parseIpDefensive(const std::string& addr) {
  auto maybeIp = folly::IPAddress::tryFromString(stripPrefixLen(addr));
  if (maybeIp.hasValue()) {
    return maybeIp.value();
  }
  return folly::IPAddress();
}

/*
 * Validate a configured next-hop string at construction. Empty means unset
 * (callers fall back to the local router-id). A non-empty value must parse and
 * match the expected address family; reject malformed or wrong-family values
 * here with a descriptive error rather than throwing opaquely later during
 * route propagation.
 */
void validateNextHop(
    const std::string& value,
    bool isV4,
    const std::string& peerAddr) {
  if (value.empty()) {
    return;
  }
  const auto maybeIp = folly::IPAddress::tryFromString(value);
  if (maybeIp.hasError()) {
    throw BgpError(
        fmt::format(
            "Peer {} has a malformed next_hop{} '{}'",
            peerAddr,
            isV4 ? "4" : "6",
            value));
  }
  if (maybeIp.value().isV4() != isV4) {
    throw BgpError(
        fmt::format(
            "Peer {} next_hop{} '{}' is not an IPv{} address",
            peerAddr,
            isV4 ? "4" : "6",
            value,
            isV4 ? "4" : "6"));
  }
}

/*
 * Return the first present optional (a wins over b). Used to apply peer > group
 * inheritance within a single AS width before the 4-byte > 2-byte preference.
 */
template <typename T>
std::optional<T> coalesce(
    const std::optional<T>& a,
    const std::optional<T>& b) {
  return a.has_value() ? a : b;
}

// Resolve a boolean with peer > group precedence, defaulting to false.
bool resolveBool(
    const std::optional<bool>& peerVal,
    const std::optional<bool>& groupVal) {
  if (peerVal.has_value()) {
    return *peerVal;
  }
  if (groupVal.has_value()) {
    return *groupVal;
  }
  return false;
}

// Resolve a string with peer > group precedence, defaulting to empty.
std::string resolveString(
    const std::optional<std::string>& peerVal,
    const std::optional<std::string>& groupVal) {
  if (peerVal.has_value()) {
    return *peerVal;
  }
  if (groupVal.has_value()) {
    return *groupVal;
  }
  return std::string{};
}

std::string_view peerTypeToString(PeerType type) {
  switch (type) {
    case PeerType::INTERNAL:
      return "INTERNAL";
    case PeerType::EXTERNAL:
      return "EXTERNAL";
    case PeerType::CONFED_EXTERNAL:
      return "CONFED_EXTERNAL";
  }
  return "UNKNOWN";
}

} // namespace

BgpPeer::BgpPeer(const thrift::BgpPeer& peer, const thrift::PeerGroup* group) {
  // Identity — addresses (defensively parsed, stripping any subnet suffix).
  peerIp_ = parseIpDefensive(*peer.peer_addr());
  localIp_ = parseIpDefensive(*peer.local_addr());

  /*
   * Remote ASN (the neighbor's AS identity): peer-only, 4-byte preferred over
   * 2-byte, matching production Config.cpp:609-618. Peer-group remote_as* is
   * NOT inherited at runtime — peers in a group may have different remote ASes,
   * so group inheritance would be unsafe (group remote_as* exists only for
   * Arista-style rendering). Setting both widths is a config error → throw.
   */
  const auto remoteOpt4 = toOpt(peer.remote_as_4_byte());
  const auto remoteOpt2 = toOpt(peer.remote_as());
  if (remoteOpt4.has_value() && remoteOpt2.has_value()) {
    throw BgpError(
        fmt::format(
            "Both remote_as_4_byte and remote_as are set for peer {}. Use remote_as_4_byte only",
            peerIp_.str()));
  }
  remoteAsn_ = remoteOpt4.has_value()
      ? static_cast<uint32_t>(*remoteOpt4)
      : static_cast<uint32_t>(remoteOpt2.value_or(0));

  /*
   * Local ASN override (RFC 7705 local-as): resolve each width across
   * peer > group, then prefer the resolved 4-byte over the resolved 2-byte
   * (net order peer4 > group4 > peer2 > group2), matching production
   * Config.cpp:591-608. Setting both resolved widths logs an ERR (not fatal).
   * The override stays unset (nullopt) if neither width is configured; the
   * effective local ASN is then filled later by the BgpSwitch (Phase 3) via
   * setLocalAsn from the global switch ASN.
   */
  const auto localR4 = coalesce(
      toOpt(peer.local_as_4_byte()),
      group ? toOpt(group->local_as_4_byte()) : std::nullopt);
  const auto localR2 = coalesce(
      toOpt(peer.local_as()), group ? toOpt(group->local_as()) : std::nullopt);
  if (localR4.has_value() && localR2.has_value()) {
    XLOGF(
        ERR,
        "Both local_as_4_byte and local_as are set for peer {}. Use local_as_4_byte only",
        peerIp_.str());
  }
  if (localR4.has_value()) {
    localAsOverride_ = static_cast<uint32_t>(*localR4);
  } else if (localR2.has_value()) {
    localAsOverride_ = static_cast<uint32_t>(*localR2);
  }
  localAsn_ = localAsOverride_.value_or(0);

  // Classification booleans: peer > group.
  isPassive_ = resolveBool(
      toOpt(peer.is_passive()),
      group ? toOpt(group->is_passive()) : std::nullopt);
  isRrClient_ = resolveBool(
      toOpt(peer.is_rr_client()),
      group ? toOpt(group->is_rr_client()) : std::nullopt);
  isConfedPeer_ = resolveBool(
      toOpt(peer.is_confed_peer()),
      group ? toOpt(group->is_confed_peer()) : std::nullopt);
  nextHopSelf_ = resolveBool(
      toOpt(peer.next_hop_self()),
      group ? toOpt(group->next_hop_self()) : std::nullopt);
  v4OverV6Nexthop_ = resolveBool(
      toOpt(peer.v4_over_v6_nexthop()),
      group ? toOpt(group->v4_over_v6_nexthop()) : std::nullopt);
  disableIpv4Afi_ = resolveBool(
      toOpt(peer.disable_ipv4_afi()),
      group ? toOpt(group->disable_ipv4_afi()) : std::nullopt);
  disableIpv6Afi_ = resolveBool(
      toOpt(peer.disable_ipv6_afi()),
      group ? toOpt(group->disable_ipv6_afi()) : std::nullopt);

  // Policy names: peer > group.
  ingressPolicyName_ = resolveString(
      toOpt(peer.ingress_policy_name()),
      group ? toOpt(group->ingress_policy_name()) : std::nullopt);
  egressPolicyName_ = resolveString(
      toOpt(peer.egress_policy_name()),
      group ? toOpt(group->egress_policy_name()) : std::nullopt);

  // Nexthops and metadata come straight from the peer config.
  validateNextHop(*peer.next_hop4(), /*isV4=*/true, peerIp_.str());
  validateNextHop(*peer.next_hop6(), /*isV4=*/false, peerIp_.str());
  nextHop4_ = *peer.next_hop4();
  nextHop6_ = *peer.next_hop6();
  description_ = resolveString(toOpt(peer.description()), std::nullopt);
  peerGroupName_ = resolveString(toOpt(peer.peer_group_name()), std::nullopt);
}

/*
 * Apply the global switch ASN (Phase 3). Preserve any per-peer local-AS
 * override and run production's same-value validation chain
 * (Config.cpp:620-628, first match wins), which requires the global ASN to
 * evaluate.
 */
void BgpPeer::setLocalAsn(uint32_t globalAsn) {
  if (localAsOverride_.has_value()) {
    const uint32_t localOverride = *localAsOverride_;
    if (localOverride == globalAsn) {
      XLOGF(
          ERR,
          "Peer-Local-AS {} for peer {} is configured with same value as global-AS {}.",
          localOverride,
          peerIp_.str(),
          globalAsn);
    } else if (localOverride == remoteAsn_) {
      throw BgpError(
          fmt::format(
              "Peer-Local-AS {} for peer {} is configured with same value as remote-AS {}.",
              localOverride,
              peerIp_.str(),
              remoteAsn_));
    } else if (remoteAsn_ == globalAsn) {
      throw BgpError(
          fmt::format(
              "Peer-Local-AS configured for non-EBGP peer {} (remote-AS {} == global-AS {}).",
              peerIp_.str(),
              remoteAsn_,
              globalAsn));
    }
  }
  localAsn_ = localAsOverride_.value_or(globalAsn);
}

PeerType BgpPeer::computePeerType(
    uint32_t localAs,
    uint32_t remoteAs,
    bool isConfedPeer) {
  /*
   * Mirror production AdjRibCommonUtils::getBgpSessionType: an intra-confed
   * session (confed peer that shares the local sub-AS) is treated as internal,
   * so test equal-AS first. Only a confed peer with a differing AS is
   * CONFED_EXTERNAL (ConfedEBGP); any other differing-AS peer is EXTERNAL.
   */
  if (localAs == remoteAs) {
    return PeerType::INTERNAL;
  }
  if (isConfedPeer) {
    return PeerType::CONFED_EXTERNAL;
  }
  return PeerType::EXTERNAL;
}

void BgpPeer::printSummary() const {
  XLOGF(
      INFO,
      "BgpPeer peer={} local={} remoteAsn={} localAsn={} peerType={} group={} "
      "ingress={} egress={} receivedRoutes={}",
      peerIp_.str(),
      localIp_.str(),
      remoteAsn_,
      localAsn_,
      peerTypeToString(peerType()),
      peerGroupName_,
      ingressPolicyName_,
      egressPolicyName_,
      receivedRoutes_.size());
}

} // namespace facebook::bgp
