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
#include <stdexcept>
#include <utility>

#include <fmt/format.h>
#include <folly/IPAddress.h>
#include <folly/container/F14Map.h>

#include "neteng/fboss/bgp/cpp/config/ConfigStructs.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyManager.h"

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

} // namespace facebook::bgp
