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

#include "neteng/fboss/bgp/cpp/rib/CanonicalRibBuilder.h"

#include <utility>

#include "configerator/structs/neteng/fboss/bgp/if/gen-cpp2/bgp_attr_types.h"
#include "neteng/fboss/bgp/cpp/BgpServiceUtil.h"
#include "neteng/fboss/bgp/cpp/rib/CanonicalConvert.h"

namespace facebook::bgp {

namespace bgp_attr = ::facebook::neteng::fboss::bgp_attr;

int64_t CanonicalRibBuilder::internWholePath(
    const std::shared_ptr<const BgpPath>& path) {
  auto [idx, inserted] = wholePathPool_.internReporting(path);
  /*
   * The list-valued sub-attrs are interned the first time a whole path is seen
   * (their pointers are shared by every duplicate of that path), so only intern
   * them on insert -- a duplicate BgpPath shared across many prefixes at scale
   * would otherwise re-run four redundant pool lookups. Empty sub-attrs have no
   * deduplicated pointer and carry no dict entry.
   */
  if (!inserted) {
    return idx;
  }
  if (const auto& p = path->getAsPath().getSharedPtr()) {
    asPathPool_.intern(p);
  }
  if (const auto& p = path->getCommunities().getSharedPtr()) {
    communitiesPool_.intern(p);
  }
  if (const auto& p = path->getExtCommunities().getSharedPtr()) {
    extCommunitiesPool_.intern(p);
  }
  if (const auto& p = path->getClusterList().getSharedPtr()) {
    clusterListPool_.intern(p);
  }
  return idx;
}

int64_t CanonicalRibBuilder::internPeer(
    const folly::IPAddress& addr,
    int64_t routerId,
    std::string_view description) {
  /*
   * Keyed on (addr, routerId); the description from the first call for a given
   * key wins, since it is only stored on insert. Callers must pass a consistent
   * description per peer -- RibBase reads it from the same RouteInfo::peer for
   * every path of a peer, so this holds today.
   */
  auto [it, inserted] =
      peerIdxByKey_.try_emplace(PeerKey{addr, routerId}, nextPeerId_);
  if (inserted) {
    peers_.emplace(
        nextPeerId_++, toTCanonicalPeer(addr, routerId, description));
  } else {
    const auto& storedPeer = std::as_const(peers_).at(it->second);
    auto storedDescription = storedPeer.peer_description();
    XDCHECK_EQ(storedDescription.has_value(), !description.empty());
    if (storedDescription.has_value()) {
      XDCHECK_EQ(*storedDescription, description)
          << "inconsistent description for canonical peer";
    }
  }
  return it->second;
}

bgp_thrift::TBgpDedupedPath CanonicalRibBuilder::buildDedupedPath(
    const BgpPath& path) const {
  /* next_hop + inline scalars are shared; add this build's sub-attr indices. */
  auto attrs = toTBgpDedupedPathBase(path);

  /*
   * List-valued sub-attrs resolve to their dict index (interned in
   * internWholePath()); an unset/empty sub-attr carries no index.
   */
  if (const auto& p = path.getAsPath().getSharedPtr()) {
    attrs.as_path_idx() = asPathPool_.indexOf(p);
  }
  if (const auto& p = path.getCommunities().getSharedPtr()) {
    attrs.communities_idx() = communitiesPool_.indexOf(p);
  }
  if (const auto& p = path.getExtCommunities().getSharedPtr()) {
    attrs.ext_communities_idx() = extCommunitiesPool_.indexOf(p);
  }
  if (const auto& p = path.getClusterList().getSharedPtr()) {
    attrs.cluster_list_idx() = clusterListPool_.indexOf(p);
  }

  return attrs;
}

void CanonicalRibBuilder::addEntry(
    const folly::CIDRNetwork& prefix,
    int64_t ribVersion,
    const std::vector<CanonicalPathInput>& paths,
    const CanonicalEntryFields& entryFields) {
  /*
   * Guard duplicate prefixes up front, before interning any paths/peers, so a
   * duplicate call is a true no-op that cannot leave orphaned entries in the
   * shared pools in release builds (where the DFATAL below does not abort).
   */
  auto key = folly::IPAddress::networkToString(prefix);
  auto [entryIt, inserted] = ribEntries_.try_emplace(key);
  if (!inserted) {
    XLOGF(
        DFATAL, "addEntry called twice for prefix {}; keeping the first", key);
    return;
  }

  auto& entry = entryIt->second;
  entry.prefix() = createTIpPrefix(prefix);
  entry.rib_version() = ribVersion;

  /* Per-entry operational fields */
  if (entryFields.pathSelectionPending.has_value()) {
    entry.path_selection_pending() = entryFields.pathSelectionPending.value();
  }
  if (entryFields.activeCpsCriteria.has_value()) {
    entry.active_cps_criteria() = entryFields.activeCpsCriteria.value();
  }
  if (entryFields.activeCteUcmpAction.has_value()) {
    entry.active_cte_ucmp_action() = entryFields.activeCteUcmpAction.value();
  }

  auto& groups = entry.paths().value();
  /*
   * Cache the current group's vector: a run of paths in the same group (best
   * then multipaths, the common case) then does one map lookup instead of one
   * per path. std::map node pointers stay valid across later inserts.
   */
  std::string_view cachedGroup;
  std::vector<bgp_thrift::TBgpPathCanonical>* groupPaths = nullptr;
  for (const auto& in : paths) {
    bgp_thrift::TBgpPathCanonical p;
    p.path_idx() = internWholePath(in.path);
    p.peer_idx() = internPeer(in.peerAddr, in.peerRouterId, in.peerDescription);
    applyPerPathInstanceFields(p, in);
    if (groupPaths == nullptr || in.group != cachedGroup) {
      groupPaths = &groups[std::string(in.group)];
      cachedGroup = in.group;
    }
    groupPaths->push_back(std::move(p));
  }
}

bgp_thrift::TCanonicalRibState CanonicalRibBuilder::build() {
  /*
   * One-shot: build() moves the peer + entry pools out, so a second call would
   * silently return a state with a populated dict/deduped_paths but empty
   * peers/rib_entries. Fail loudly on misuse instead.
   */
  XCHECK(!built_) << "CanonicalRibBuilder::build() called more than once";
  built_ = true;
  bgp_thrift::TCanonicalRibState state;

  bgp_thrift::TBgpAttrDict dict;
  dict.as_path_lists() =
      asPathPool_.snapshot<std::vector<bgp_attr::TAsPathSeg>>(toTAsPathSegList);
  dict.community_lists() =
      communitiesPool_.snapshot<std::vector<bgp_attr::TBgpCommunity>>(
          toTCommunityList);
  dict.ext_community_lists() =
      extCommunitiesPool_.snapshot<std::vector<bgp_thrift::TBgpExtCommunity>>(
          toCanonicalExtCommunities);
  dict.cluster_lists() =
      clusterListPool_.snapshot<std::vector<int64_t>>(toTClusterList);
  state.attr_dict() = std::move(dict);

  state.deduped_paths() = wholePathPool_.snapshot<bgp_thrift::TBgpDedupedPath>(
      [this](const BgpPath& path) { return buildDedupedPath(path); });

  state.peers() = std::move(peers_);
  state.rib_entries() = std::move(ribEntries_);
  return state;
}

} // namespace facebook::bgp
