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

#include "neteng/fboss/bgp/cpp/sim/BgpSwitch.h"

#include <chrono>
#include <optional>
#include <stdexcept>
#include <utility>

#include <fmt/format.h>
#include <folly/IPAddress.h>
#include <folly/container/F14Map.h>
#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/config/ConfigStructs.h"
#include "neteng/fboss/bgp/cpp/config/ConfigUtils.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyManager.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyStructs.h"

namespace facebook::bgp {

namespace {

/*
 * Parse the config router-id string into a folly::IPAddress, defaulting to
 * 0.0.0.0 when unset/unparseable (matches the simulator's defensive parsing).
 */
folly::IPAddress routerIdToIp(const thrift::BgpConfig& config) {
  auto maybeIp = folly::IPAddress::tryFromString(*config.router_id());
  if (maybeIp.hasValue()) {
    return maybeIp.value();
  }
  return folly::IPAddress("0.0.0.0");
}

/*
 * Numeric router-id used for best-path tie-breaking. BGP router-ids are 4-byte
 * (IPv4) values; non-v4/unset ids resolve to 0.
 */
uint32_t extractRouterId(const thrift::BgpConfig& config) {
  const auto ip = routerIdToIp(config);
  return ip.isV4() ? ip.asV4().toLongHBO() : 0;
}

// Local ASN with 4-byte preferred over the deprecated 2-byte field.
uint32_t extractLocalAsn(const thrift::BgpConfig& config) {
  if (config.local_as_4_byte().has_value()) {
    return static_cast<uint32_t>(*config.local_as_4_byte());
  }
  if (config.local_as().has_value()) {
    return static_cast<uint32_t>(*config.local_as());
  }
  return 0;
}

// Confederation ASN (if any), 4-byte preferred over the 2-byte field.
std::optional<uint32_t> extractLocalConfedAsn(const thrift::BgpConfig& config) {
  if (config.local_confed_as_4_byte().has_value()) {
    return static_cast<uint32_t>(*config.local_confed_as_4_byte());
  }
  if (config.local_confed_as().has_value()) {
    return static_cast<uint32_t>(*config.local_confed_as());
  }
  return std::nullopt;
}

/*
 * Derive the RoutingTable's config from global config, mirroring the bestpath
 * feature flags production reads from BgpSettingConfig.
 */
RoutingTableConfig makeRoutingTableConfig(
    const thrift::BgpConfig& config,
    uint32_t routerId,
    uint32_t localAsn,
    const std::optional<uint32_t>& localConfedAsn) {
  RoutingTableConfig rtConfig;
  rtConfig.routerId = routerId;
  rtConfig.localAs4Byte = localAsn;
  rtConfig.localConfedAs4Byte = localConfedAsn.value_or(0);

  if (const auto setting = config.bgp_setting_config()) {
    rtConfig.enableMedComparison =
        setting->enable_med_comparison().value_or(false);
    rtConfig.enableMedMissingAsWorst =
        setting->enable_med_missing_as_worst().value_or(false);
    rtConfig.enableWeightComparison =
        setting->enable_weight_comparison().value_or(false);
    rtConfig.enableEiBgpMultipath =
        setting->enable_eibgp_multipath().value_or(false);
  }
  rtConfig.countConfedsInAsPathLen =
      config.count_confeds_in_as_path_len().value_or(false);
  return rtConfig;
}

/*
 * Build the BgpPath for a locally originated network, mirroring
 * RibBase::createLocalRoute(): default nexthop, IGP origin, default local-pref,
 * optional communities/as-path, and the local-route weight applied up front so
 * an origination policy can observe and override it. Returns nullptr (and logs)
 * when the configured origin is outside the valid BgpAttrOrigin range, matching
 * the production validation. The returned path is unpublished.
 */
std::shared_ptr<BgpPath> buildOriginatedPath(
    const folly::CIDRNetwork& prefix,
    const thrift::BgpNetwork& network) {
  nettools::bgplib::BgpPathC pathC;
  if (prefix.first.isV4()) {
    pathC.nexthop = network.nexthop().has_value()
        ? folly::IPAddress(*network.nexthop())
        : kLocalRouteV4Nexthop;
  } else {
    pathC.nexthop = network.nexthop().has_value()
        ? folly::IPAddress(*network.nexthop())
        : kLocalRouteV6Nexthop;
  }

  nettools::bgplib::BgpAttributesC attrs;
  if (network.origin().has_value()) {
    const auto origin = *network.origin();
    if (origin < static_cast<int>(apache::thrift::TEnumTraits<
                                  nettools::bgplib::BgpAttrOrigin>::min()) ||
        origin > static_cast<int>(apache::thrift::TEnumTraits<
                                  nettools::bgplib::BgpAttrOrigin>::max())) {
      XLOGF(
          ERR,
          "Invalid origin value {} for originated network {}",
          origin,
          *network.prefix());
      return nullptr;
    }
    attrs.origin = static_cast<nettools::bgplib::BgpAttrOrigin>(origin);
  } else {
    attrs.origin = nettools::bgplib::BgpAttrOrigin::BGP_ORIGIN_IGP;
  }
  attrs.localPref = network.local_pref().has_value()
      ? static_cast<uint32_t>(*network.local_pref())
      : kDefaultLocalPref;
  if (network.communities().has_value()) {
    attrs.communities = createBgpAttrCommunitiesC(*network.communities());
  }
  if (network.as_path().has_value()) {
    attrs.asPath = createBgpAttrAsPathDedup(*network.as_path());
  }
  pathC.attrs = std::move(attrs);

  auto path = std::make_shared<BgpPath>(static_cast<BgpPathFields>(pathC));
  /*
   * Set the local-route weight before policy application so an origination
   * policy can observe and override it (matches RibBase::createLocalRoute,
   * where the weight is set on attrs before getBgpPathFromPolicy()).
   */
  path->setWeight(kLocalRouteWeight);
  return path;
}

} // namespace

BgpSwitch::BgpSwitch(std::string name, thrift::BgpConfig config)
    : name_(std::move(name)),
      routerId_(extractRouterId(config)),
      localAsn_(extractLocalAsn(config)),
      localConfedAsn_(extractLocalConfedAsn(config)),
      holdTime_(static_cast<uint32_t>(*config.hold_time())),
      networks4_(*config.networks4()),
      networks6_(*config.networks6()),
      routingTable_(makeRoutingTableConfig(
          config,
          static_cast<uint32_t>(routerId_),
          localAsn_,
          localConfedAsn_)) {
  /*
   * A BgpSwitch requires a valid local ASN; AS 0 is reserved/invalid
   * (RFC 7607). Reject it rather than silently propagating the sentinel to
   * every peer and the routing table.
   */
  if (localAsn_ == 0) {
    throw std::runtime_error(
        fmt::format(
            "BgpSwitch {}: missing or invalid local ASN (0); a valid local ASN is required",
            name_));
  }
  buildPeers(config);
  initPolicyManager(config);
}

BgpSwitch::~BgpSwitch() = default;

void BgpSwitch::buildPeers(const thrift::BgpConfig& config) {
  folly::F14FastMap<std::string, const thrift::PeerGroup*> groupsByName;
  if (config.peer_groups().has_value()) {
    for (const auto& group : *config.peer_groups()) {
      groupsByName.emplace(*group.name(), &group);
    }
  }

  peers_.reserve(config.peers()->size());
  for (const auto& peerConfig : *config.peers()) {
    const thrift::PeerGroup* group = nullptr;
    if (peerConfig.peer_group_name().has_value()) {
      const auto it = groupsByName.find(*peerConfig.peer_group_name());
      if (it == groupsByName.end()) {
        throw std::runtime_error(
            fmt::format(
                "BgpSwitch {}: peer {} references undefined peer group '{}'",
                name_,
                *peerConfig.peer_addr(),
                *peerConfig.peer_group_name()));
      }
      group = it->second;
    }
    auto& peer = peers_.emplace_back(peerConfig, group);
    peer.setLocalAsn(localAsn_);
    peer.setRouterId(routerId_);
  }
}

void BgpSwitch::initPolicyManager(const thrift::BgpConfig& config) {
  /*
   * Mirror Config::createPolicyManager(): a PolicyManager exists iff policies
   * are configured.
   */
  if (!config.policies().has_value()) {
    return;
  }

  /*
   * PolicyManager only reads the global config during construction (it does not
   * retain the pointer), so a stack-local instance is sufficient.
   */
  const auto routerIp = routerIdToIp(config);
  const BgpGlobalConfig globalConfig(
      localAsn_,
      routerIp, /* routerId */
      routerIp, /* clusterId */
      std::chrono::seconds(holdTime_),
      std::nullopt, /* listenAddr */
      std::nullopt, /* grRestartTime */
      {}, /* networksV4 */
      {}, /* networksV6 */
      localConfedAsn_);
  policyManager_ =
      std::make_unique<PolicyManager>(*config.policies(), &globalConfig);
}

void BgpSwitch::originateRoutes() {
  if (routesOriginated_) {
    return;
  }
  for (const auto& network : networks4_) {
    originateNetwork(network);
  }
  for (const auto& network : networks6_) {
    originateNetwork(network);
  }
  /*
   * Mark as originated only after a full successful pass. originateNetwork()
   * throws if a network references an unconfigured policy; leaving the guard
   * unset on a thrown config error keeps origination all-or-nothing rather
   * than marking the switch done after a partial pass (addOriginatedRoute is
   * idempotent per-prefix).
   */
  routesOriginated_ = true;
}

void BgpSwitch::originateNetwork(const thrift::BgpNetwork& network) {
  const auto prefix = folly::IPAddress::createNetwork(*network.prefix());
  auto path = buildOriginatedPath(prefix, network);
  if (!path) {
    /* Invalid origin value; buildOriginatedPath already logged the error. */
    return;
  }

  std::string policyName;
  if (network.policy_name().has_value() && !network.policy_name()->empty()) {
    policyName = *network.policy_name();
    if (!policyManager_ || !policyManager_->isPolicyPresent(policyName)) {
      throw std::runtime_error(
          fmt::format(
              "BgpSwitch {}: network {} references origination policy '{}' but "
              "the policy is not configured",
              name_,
              *network.prefix(),
              policyName));
    }
    auto policyOut = policyManager_->applyPolicy(
        policyName, PolicyInMessage({prefix}, path));
    const auto it = policyOut.result.find(prefix);
    /* Rejected prefixes are either absent or present with null attrs. */
    if (it == policyOut.result.end() || !it->second->attrs) {
      XLOGF(
          DBG2,
          "BgpSwitch {}: origination policy {} rejected prefix {}",
          name_,
          policyName,
          *network.prefix());
      return;
    }
    path = it->second->attrs;
  }

  path->publish();
  routingTable_.addOriginatedRoute(prefix, path, policyName);
}

} // namespace facebook::bgp
