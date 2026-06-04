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

#include <folly/CppAttributes.h>
#include <folly/IPAddress.h>
#include <folly/Range.h>
#include <gflags/gflags.h>

#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/common/Types.h"

/*
 * gflag gate for RFC 1997 well-known community egress suppression.
 * Default OFF: the historical behavior of advertising routes regardless
 * of well-known community is preserved until operators flip the flag.
 * Defined in WellKnownCommunityFilter.cpp.
 */
DECLARE_bool(enable_well_known_community_filter);

namespace facebook::bgp {

/*
 * RFC 1997 well-known communities. Values are (asn, value) pairs where
 * asn is always 0xFFFF (reserved). See RFC 1997 section 4.
 */
namespace WellKnownCommunity {
constexpr uint16_t kReservedAsn = 0xFFFF;
constexpr uint16_t kNoExport = 0xFF01;
constexpr uint16_t kNoAdvertise = 0xFF02;
constexpr uint16_t kNoExportSubconfed = 0xFF03;
} // namespace WellKnownCommunity

/*
 * Identifies which well-known community caused a suppression decision.
 * Used as the return value of shouldSuppressByWellKnownCommunity so the
 * caller can log and increment the matching ODS counter.
 */
enum class RFC1997Community : uint8_t {
  NoAdvertise,
  NoExport,
  NoExportSubconfed,
};

const char* toString(RFC1997Community c);

/*
 * RFC 1997 egress filter.
 *
 * Returns the community that caused suppression, or std::nullopt if the
 * route should be advertised to the receiving peer. Suppression rules:
 *
 *   NO_ADVERTISE        -> suppress to ANY peer (IBGP, EBGP, ConfedEBGP)
 *   NO_EXPORT           -> suppress to EBGP only
 *                          (allowed to IBGP and ConfedEBGP per RFC 1997)
 *   NO_EXPORT_SUBCONFED -> suppress to EBGP and ConfedEBGP
 *                          (allowed to IBGP regardless of sub-AS membership.
 *                           Multi-sub-AS IBGP topologies are not deployed
 *                           in this codebase today; if they ever are, this
 *                           filter must also reject IBGP peers whose
 *                           remote sub-AS differs from the local sub-AS
 *                           per RFC 1997 section 3.)
 *
 * The same predicate is used from both the peer-level AdjRib path
 * (canAnnounceEntry) and the group-level AdjRibOutGroup path
 * (canAnnounceForGroup) so the behavior is identical regardless of whether
 * update-grouping is enabled.
 *
 * NO_ADVERTISE is checked first so that paths carrying it always suppress
 * even when other well-known communities are present.
 *
 * Pre-policy attributes: this predicate inspects the RIB's pre-egress-policy
 * path attributes. An egress route-map that strips or adds a well-known
 * community will NOT influence the suppression decision. This matches the
 * "ingress-checks-first" interpretation of RFC 1997 section 4 and lines up
 * with FRR/BIRD. If post-policy semantics are ever desired, the predicate
 * must move into processRibAnnouncedEntry after policy evaluation, on the
 * post-policy attribute set.
 */
std::optional<RFC1997Community> shouldSuppressByWellKnownCommunity(
    const std::shared_ptr<const BgpPath>& path,
    BgpSessionType sessionType) noexcept;

/*
 * Increment the matching bgpd.well_known_community.* ODS counter.
 * Centralized here so callers in AdjRibOut and AdjRibGroup share the same
 * dispatch logic.
 */
void incrementSuppressionStat(RFC1997Community community);

/*
 * Apply the RFC 1997 well-known community filter to an outbound
 * announcement. Returns true if the route MUST be suppressed.
 *
 * On suppression this also:
 *   - logs a per-prefix DBG3 line (rate-limited to once per second)
 *   - logs a per-context INFO line (rate-limited to once per minute)
 *   - increments the matching bgpd.well_known_community.* counter
 *
 * Centralized so the per-peer (AdjRib::canAnnounceEntry) and group-level
 * (AdjRibOutGroup::canAnnounceForGroup) call sites share identical
 * decision + side-effect logic. The caller is expected to short-circuit
 * on FLAGS_enable_well_known_community_filter so the predicate is only
 * paid for when the feature is on.
 *
 * contextLabel is the caller's human-readable identifier (peer name for
 * per-peer, group descriptor for group). It appears verbatim as the
 * subject in log messages so operators can attribute a suppression
 * decision back to the call site.
 */
bool applyWellKnownCommunityFilter(
    const std::shared_ptr<const BgpPath>& attrs,
    BgpSessionType sessionType,
    folly::StringPiece contextLabel,
    const folly::CIDRNetwork& prefix) noexcept;

} // namespace facebook::bgp
