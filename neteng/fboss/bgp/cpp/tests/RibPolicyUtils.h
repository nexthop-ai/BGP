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

#include "neteng/fboss/bgp/cpp/rib/RibPolicy.h"

namespace facebook::bgp {

rib_policy::TRouteAttributeStatement createTRouteAttributeStatementLbw(
    const std::vector<folly::CIDRNetwork>& prefixes,
    const int64_t& lbwVal,
    const std::optional<int>& timestamp = std::nullopt);

rib_policy::TRouteAttributePolicy createTRouteAttributePolicyLbw(
    const std::vector<folly::CIDRNetwork>& prefixes,
    const int64_t& lbwVal,
    const std::string& statementName = "stmt1",
    const std::optional<int>& timeStamp = std::nullopt);

rib_policy::TRibPolicy createTRibPolicyLbw(
    const std::vector<folly::CIDRNetwork>& prefixes,
    const std::optional<int64_t>& lbwVal,
    const std::string& statementName = "stmt1",
    const std::optional<int>& timeStamp = std::nullopt);

rib_policy::TRouteAttributeStatement createTRouteAttributeStatementUcmp(
    const std::vector<folly::CIDRNetwork>& prefixes,
    rib_policy::TRouteAttributeUcmpAction& tRouteAttributeUcmpAction,
    const std::optional<int>& timeStamp = std::nullopt);

rib_policy::TRouteAttributePolicy createTRouteAttributePolicyUcmp(
    const std::vector<folly::CIDRNetwork>& prefixes,
    rib_policy::TRouteAttributeUcmpAction& tRouteAttributeUcmpAction,
    const std::string& statementName = "stmt1",
    const std::optional<int>& timeStamp = std::nullopt);

rib_policy::TRibPolicy createTRibPolicyUcmp(
    const std::vector<folly::CIDRNetwork>& prefixes,
    rib_policy::TRouteAttributeUcmpAction& tRouteAttributeUcmpAction,
    const std::string& statementName = "stmt1",
    const std::optional<int>& timeStamp = std::nullopt);

rib_policy::TPathSelectionStatement
createTPathSelectionStatementWithPathSelector(
    const std::vector<folly::CIDRNetwork>& prefixes,
    const rib_policy::TPathSelector& tPathSelector);

rib_policy::TPathSelectionPolicy createTPathSelectionPolicyWithPathSelector(
    const std::vector<folly::CIDRNetwork>& prefixes,
    const rib_policy::TPathSelector& tPathSelector);

rib_policy::TRibPolicy createTRibPolicyWithPathSelector(
    const std::vector<folly::CIDRNetwork>& prefixes,
    const rib_policy::TPathSelector& tPathSelector);

rib_policy::TPathSelector createTPathSlectorWithOneMatcher(
    const rib_policy::TBgpPathMatcher& matcher,
    std::optional<int32_t> criteriaMinNextHop = std::nullopt,
    std::optional<int32_t> defaultMinNextHop = std::nullopt,
    std::optional<int64_t> defaultMinAggLbwbps = std::nullopt,
    std::optional<bool> relaxMinAggLbwbps = std::nullopt);

rib_policy::TBgpCommunityMatch createTBgpCommunityMatch(
    const int32_t& asn,
    const int32_t& value,
    const routing_policy::MatchValueLogicOperator& matchType =
        routing_policy::MatchValueLogicOperator::EQUAL);

rib_policy::TCommunityListMatch createTCommunityListMatch(
    const std::vector<rib_policy::TBgpCommunityMatch>& communities,
    const routing_policy::BooleanOperator& booleanOperator =
        routing_policy::BooleanOperator::OR);

rib_policy::TRibRouteMatcher createTRibRouteMatcher(
    const std::vector<folly::CIDRNetwork>& prefixes,
    const std::optional<rib_policy::TCommunityListMatch>& communityList =
        std::nullopt);

rib_policy::TBgpPathMatcher
createCommunityMatch(int32_t asn, int32_t value, bgp_policy::Origin origin);

rib_policy::TRouteFilter createTRouteFilter(
    const std::vector<folly::CIDRNetwork>& prefixes,
    bool permissive = false);

rib_policy::TRouteFilterStatement createTRouteFilterStatement(
    const std::vector<folly::CIDRNetwork>& prefixes,
    bool permissive = false,
    bool egress = true);

rib_policy::TRouteFilterStatement
createTRouteFilterStatementWithIngressAndEgressFilters(
    const std::vector<folly::CIDRNetwork>& ingressPrefixes,
    const std::vector<folly::CIDRNetwork>& egressPrefixes,
    bool ingressPermissive = false,
    bool egressPermissive = false,
    std::optional<facebook::bgp::routing_policy::IPVersion> ingressIPVersion =
        std::nullopt,
    std::optional<facebook::bgp::routing_policy::IPVersion> egressIPVersion =
        std::nullopt);

rib_policy::TGoldenPrefixPolicy createTGoldenPrefixPolicy(
    const std::vector<folly::CIDRNetwork>& prefixes,
    std::optional<int> maxSubnets,
    const std::vector<int>& allowedMaskLengths,
    const std::optional<std::set<std::string>>& communities = std::nullopt);

rib_policy::TGoldenPrefixPolicy createTGoldenPrefixPolicy(
    const std::vector<std::pair<folly::CIDRNetwork, std::vector<int>>>&
        prefixesAndMaskLengths,
    std::optional<int> maxSubnets,
    const std::optional<std::set<std::string>>& communities = std::nullopt);

rib_policy::TRouteFilterPolicy createTRouteFilterPolicy(
    const std::vector<rib_policy::TRouteFilterStatement>& statements,
    int64_t version);

rib_policy::TRouteFilterPolicy createTRouteFilterPolicyWithKeyType(
    const std::vector<rib_policy::TRouteFilterStatement>& statements,
    int64_t version,
    rib_policy::KeyType keyType);

} // namespace facebook::bgp
