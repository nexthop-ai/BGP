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

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/facebook/ScubaLoggerFactory.h"
#include "neteng/fboss/bgp/cpp/fsdb/FsdbSyncer.h"
#include "neteng/fboss/bgp/cpp/rib/FibDev.h"
#include "neteng/fboss/bgp/cpp/rib/FibFboss.h"
#include "neteng/fboss/bgp/cpp/rib/RibDC.h"

namespace facebook::bgp {

RibDC::RibDC(
    const std::unordered_map<folly::CIDRNetwork, thrift::BgpNetwork>&
        localRoutes,
    const BgpGlobalConfig& globalConfig,
    const std::optional<bgp_policy::BgpPolicies>& policyConfig,
    nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage>& ribInQ,
    MonitoredMPMCQueue<RibOutMessage>& ribOutQ,
    const std::string& platform,
    FsdbSyncer* fsdbSyncer,
    std::shared_ptr<NexthopCache> nexthopCache,
    uint16_t fibAgentPort,
    uint32_t fibAgentRecvTimeout)
    : RibBase(
          localRoutes,
          globalConfig,
          policyConfig,
          ribInQ,
          ribOutQ,
          platform,
          nexthopCache,
          fibAgentPort,
          fibAgentRecvTimeout) {
  fsdbSyncer_ = fsdbSyncer;

  routeAttributePolicyTimer_ =
      folly::AsyncTimeout::make(evb_, [this]() noexcept {
        if (!routeAttributePolicy_) {
          return;
        }
        auto mostRecentExpTime =
            routeAttributePolicy_->getMostRecentExpirationTime();
        auto now = std::chrono::seconds(std::time(nullptr)).count();
        if (mostRecentExpTime < now) {
          ribPolicyMsgQ_.push(RouteAttributePolicyTimerMsg{});
        }
        scheduleRouteAttributePolicyTimer();
      });

  scubaLogger_ = createRibPolicyScubaLogger();
  if (scubaLogger_ && globalConfig_.deviceName) {
    ribPolicyLogger_ = std::make_unique<RibPolicyLogger>(
        *globalConfig_.deviceName, scubaLogger_);
  }

  /*
   * Read previous RibPolicy from disk to restore policy and trigger fib
   * programming. This must happen in the subclass constructor since
   * replaceRibPolicy() is pure virtual in RibBase.
   */
  replaceRibPolicy(readRibPolicyState(), /*isBootstrap=*/true);
}

void RibDC::createFib() {
  if (platform_ == kDevPlatform) {
    XLOG(DBG1, "Creating Fib Dev with no actual route programming");
    fib_ = FibDev::createFibDev(fromFibMessageQ_);
  } else {
    XLOG(DBG1, "Creating Fib for FBOSS Agent");
    fib_ = FibFboss::createFibFboss(&evb_, asyncScope_, fromFibMessageQ_);
  }
}

void RibDC::maybeStartFsdbSyncer() {
  if (!fsdbSyncer_ || fsdbSyncerStarted_) {
    return;
  }
  if (!ribEoRReceived_) {
    return;
  }
  XLOG(INFO, "Starting FsdbSyncer in Rib thread");
  fsdbSyncer_->start();
  fsdbSyncerStarted_ = true;
}

void RibDC::enqueueRibUpdateToFsdb() {
  if (!fsdbSyncer_) {
    return;
  }

  if (FLAGS_publish_rib_to_fsdb) {
    std::map<std::string, std::optional<bgp_thrift::TRibEntry>> ribUpdateToFsdb;
    for (const auto& updatedRoute : fibBatchList_) {
      const auto& routePrefix = updatedRoute.getPrefix();
      auto prefix = folly::IPAddress::networkToString(routePrefix);
      auto ribEntry = ribEntries_.find(routePrefix);
      if (ribEntry != ribEntries_.end() && updatedRoute.getBestPath()) {
        ribUpdateToFsdb.emplace(
            std::move(prefix), std::make_optional(createTRibEntry(*ribEntry)));
      } else {
        ribUpdateToFsdb.emplace(std::move(prefix), std::nullopt);
      }
    }

    fsdbSyncer_->updateRibMap(std::move(ribUpdateToFsdb));
  }

  maybeStartFsdbSyncer();
}

void RibDC::processNexthopResolutionUpdate(
    const NexthopResolutionUpdate& nexthopResolutionUpdate) noexcept {
  /*
   * Process conditional-route advertisements/withdrawals first (if any),
   * THEN push the one-shot RibOutNexthopResolutionReceived signal to
   * PeerManager. The post-processing push ordering is what guarantees that
   * conditional routes are in ribEntries_ before PeerManager triggers the
   * initial path computation — preventing the initial syncFib from wiping
   * GR-retained conditional routes in FibAgent on BGP daemon restart.
   */
  if (!conditionalLocalRoutes_.empty()) {
    processConditionalRoutesForNexthops(
        nexthopResolutionUpdate.resolved,
        [this](const folly::CIDRNetwork& prefix, PrefixPathIds&& pfxPathIds) {
          XLOGF(
              INFO,
              "Announcing conditional route {}",
              folly::IPAddress::networkToString(prefix));
          processRibInAnnouncement(
              kV4LocalPeerInfo, localRoutes_.at(prefix).attrs, pfxPathIds);
        });
    processConditionalRoutesForNexthops(
        nexthopResolutionUpdate.unresolved,
        [this](const folly::CIDRNetwork& prefix, PrefixPathIds&& pfxPathIds) {
          XLOGF(
              INFO,
              "Withdrawing conditional route {}",
              folly::IPAddress::networkToString(prefix));
          processRibInWithdrawal(kV4LocalPeerInfo, pfxPathIds);
        });
  }

  if (!firstNdpSignalSent_) {
    firstNdpSignalSent_ = true;
    XLOG(
        INFO,
        "First NexthopResolutionUpdate processed; "
        "signaling PeerManager via RibOutNexthopResolutionReceived");
    ribOutQ_.push(RibOutNexthopResolutionReceived{});
  }
}

/*
 * Instead of updating RibPolicy, only replace it. Each time the RibPolicy is
 * replaced, we also need to update the pointers atRibEntry.
 */
void RibDC::replaceRibPolicy(
    std::unique_ptr<RibPolicy> newRibPolicy,
    bool isBootstrap) {
  std::unique_ptr<RouteAttributePolicy> newRouteAttributePolicy = nullptr;
  bool hasUpdateRA = false;
  std::unique_ptr<PathSelectionPolicy> newPathSelectionPolicy = nullptr;
  bool hasUpdatePS = false;
  std::unique_ptr<RouteFilterPolicy> newRouteFilterPolicy = nullptr;
  bool hasUpdateRF = false;

  if (newRibPolicy) {
    newRouteAttributePolicy = newRibPolicy->hasRouteAttributePolicy()
        ? folly::copy_to_unique_ptr(
              std::move(*newRibPolicy->getRouteAttributePolicy()))
        : nullptr;

    newPathSelectionPolicy = newRibPolicy->hasPathSelectionPolicy()
        ? folly::copy_to_unique_ptr(
              std::move(*newRibPolicy->getPathSelectionPolicy()))
        : nullptr;

    newRouteFilterPolicy = newRibPolicy->hasRouteFilterPolicy()
        ? folly::copy_to_unique_ptr(
              std::move(*newRibPolicy->getRouteFilterPolicy()))
        : nullptr;
  }
  hasUpdateRA = replaceRouteAttributePolicy(std::move(newRouteAttributePolicy));
  hasUpdatePS = replacePathSelectionPolicy(
      std::move(newPathSelectionPolicy), isBootstrap);
  hasUpdateRF =
      replaceRouteFilterPolicy(std::move(newRouteFilterPolicy), isBootstrap);

  if (isBootstrap) {
    XLOG(DBG1, "restored RibPolicy from cache");
  } else if (hasUpdateRA || hasUpdatePS || hasUpdateRF) {
    XLOGF(
        DBG1,
        "Replace RibPolicy with a new one. "
        "hasUpdateRA = {}, hasUpdatePS = {}, hasUpdateRF = {}",
        hasUpdateRA,
        hasUpdatePS,
        hasUpdateRF);
  }
}

void RibDC::postRouteFilterPolicyReplaced() {
  if (fsdbSyncer_) {
    fsdbSyncer_->setRouteFilterPolicy(
        routeFilterPolicy_ ? std::optional(routeFilterPolicy_->toThrift())
                           : std::nullopt);
  }

  if (ribPolicyLogger_) {
    int64_t psPolicyVersion = getPathSelectionPolicyVersion();
    int64_t rfPolicyVersion = getRouteFilterPolicyVersion();
    ribPolicyLogger_->log(psPolicyVersion, rfPolicyVersion);
  }
}

void RibDC::handleRouteAttributePolicySetMsg(
    const RouteAttributePolicySetMsg& msg) noexcept {
  replaceRouteAttributePolicy(
      std::make_unique<RouteAttributePolicy>(msg.policy));
}

void RibDC::handleRouteAttributePolicyClearMsg() noexcept {
  replaceRouteAttributePolicy(nullptr);
}

void RibDC::handleRouteAttributePolicyTimerMsg() noexcept {
  XLOG(
      INFO,
      "Received RouteAttributePolicyTimerMsg, triggering FIB programming...");
  for (auto& [_, ribEntry] : ribEntries_) {
    ribEntry.requirePathSelection();
  }
  schedulePrepareFibProgrammingTimer();
}

void RibDC::handlePathSelectionPolicySetMsg(
    const PathSelectionPolicySetMsg& msg) noexcept {
  replacePathSelectionPolicy(std::make_unique<PathSelectionPolicy>(msg.policy));
}

void RibDC::handlePathSelectionPolicyClearMsg() noexcept {
  replacePathSelectionPolicy(nullptr);
}

/* DC processRibPolicyMsgLoop: handles all policy types including CTE and CPS.
 */
folly::coro::Task<void> RibDC::processRibPolicyMsgLoop() noexcept {
  while (true) {
    co_await folly::coro::co_safe_point;

    auto msg = co_await co_awaitTry(ribPolicyMsgQ_.pop());
    if (!msg.hasValue()) {
      XLOG(
          INFO,
          "[Exit] Coro task cancelled. Terminating processRibPolicyMsgLoop");
      break;
    }

    folly::variant_match(
        *msg,
        [this](const RibPolicyClearMsg& /* req */) {
          handleRibPolicyClearMsg();
        },
        [this](const RouteAttributePolicySetMsg& req) {
          handleRouteAttributePolicySetMsg(req);
        },
        [this](const RouteAttributePolicyClearMsg& /* req */) {
          handleRouteAttributePolicyClearMsg();
        },
        [this](const RouteAttributePolicyTimerMsg& /* req */) {
          handleRouteAttributePolicyTimerMsg();
        },
        [this](const PathSelectionPolicySetMsg& req) {
          handlePathSelectionPolicySetMsg(req);
        },
        [this](const PathSelectionPolicyClearMsg& /* req */) {
          handlePathSelectionPolicyClearMsg();
        },
        [this](const RouteFilterPolicySetMsg& req) {
          handleRouteFilterPolicySetMsg(req);
        },
        [this](const RouteFilterPolicyClearMsg& /* req */) {
          handleRouteFilterPolicyClearMsg();
        });
  }
}

void RibDC::scheduleRouteAttributePolicyTimer() noexcept {
  if (!routeAttributePolicy_) {
    return;
  }

  auto mostRecentActiveExpTime =
      routeAttributePolicy_->getMostRecentActiveExpirationTime();
  if (mostRecentActiveExpTime < INT_MAX) {
    auto now = std::chrono::seconds(std::time(nullptr)).count();
    auto countdown = std::chrono::seconds(mostRecentActiveExpTime - now + 1);
    if (routeAttributePolicyTimer_) {
      routeAttributePolicyTimer_->cancelTimeout();
    }
    routeAttributePolicyTimer_->scheduleTimeout(countdown);
  }
}

RibDC::CacheMigrationResult RibDC::migrateRouteAttributePolicyCache(
    RouteAttributePolicy& oldPolicy,
    RouteAttributePolicy& newPolicy) {
  CacheMigrationResult result;
  const auto& oldStmts = oldPolicy.getStatements();
  const auto& newStmts = newPolicy.getStatements();

  /*
   * Single pass over statements to identify:
   * 1. hasUpdate (any difference in statements)
   * 2. needsReEvaluation (content changes or expiration)
   * 3. statementsWithNewContent and statementsRemoved for cache migration
   */
  folly::F14FastSet<std::string> statementsWithNewContent;
  folly::F14FastSet<std::string> statementsWithNewMatcher;
  folly::F14FastSet<std::string> statementsRemoved;
  bool hasNewStatements = false;

  // Check old statements against new (find changed/removed)
  for (const auto& [name, oldStmt] : oldStmts) {
    auto it = newStmts.find(name);
    if (it == newStmts.end()) {
      // Statement removed
      statementsRemoved.insert(name);
      result.hasUpdate = true;
      result.needsReEvaluation = true;
    } else {
      auto reEvalResult = oldStmt.needsReEvaluation(it->second);
      result.hasUpdate |= reEvalResult.changed;
      if (reEvalResult.needsReEval) {
        statementsWithNewContent.insert(name);
        result.needsReEvaluation = true;
        if (reEvalResult.matcherChanged) {
          statementsWithNewMatcher.insert(name);
        }
      }
    }
  }

  // Check for new statements added
  if (newStmts.size() + statementsRemoved.size() > oldStmts.size()) {
    hasNewStatements = true;
    result.hasUpdate = true;
    result.needsReEvaluation = true;
  }

  // If no update at all, old policy keeps its cache and remains active
  if (!result.hasUpdate) {
    XLOGF(
        INFO, "[CTE] Cache migration: policies identical, no migration needed");
    RibStats::STATS_raPolicyCacheMigrationIdentical.add(1);
    return result;
  }

  // If hasUpdate but no re-evaluation needed (expiration-only change)
  if (!result.needsReEvaluation) {
    newPolicy.moveCache(oldPolicy);
    XLOGF(
        INFO,
        "[CTE] Cache migration: only expiration changed, full cache move");
    RibStats::STATS_raPolicyCacheMigrationExpirationOnly.add(1);
    return result;
  }

  // Selective migration: copy unaffected entries, collect affected prefixes
  size_t preserved = 0;

  for (const auto& [prefix, matchResult] : oldPolicy.getCache()) {
    if (matchResult.has_value()) {
      // Positive cache entry - check if matched statement changed/removed
      const auto& stmtName = matchResult->getStatementName();

      if (statementsWithNewContent.contains(stmtName)) {
        result.affectedPrefixes.push_back(prefix);
        if (statementsWithNewMatcher.contains(stmtName)) {
          /*
           * Matcher changed - invalidate cache entry so re-evaluation goes
           * through the cache-miss path which re-checks the matcher.
           */
        } else {
          /*
           * Action-only change - prefix->statement mapping is still valid.
           * Preserve cache entry; re-evaluation will apply the new action.
           */
          newPolicy.setCacheEntry(prefix, matchResult);
          ++preserved;
        }
      } else if (statementsRemoved.contains(stmtName)) {
        /*
         * Statement no longer exists - invalidate cache entry.
         * Prefix may either match a newly added statement (if any), or be
         * purged of its attribute overwrites from the deleted statement.
         */
        result.affectedPrefixes.push_back(prefix);
      } else {
        // Cache entry still valid - copy it
        newPolicy.setCacheEntry(prefix, matchResult);
        ++preserved;
      }
    } else {
      // Negative cache entry (prefix matched no statement)
      if (hasNewStatements) {
        /*
         * New statements might match this prefix - need to re-evaluate.
         * Invalidate cache and mark as affected.
         */
        result.affectedPrefixes.push_back(prefix);
      } else {
        // No new statements - negative cache is still valid
        newPolicy.setCacheEntry(prefix, std::nullopt);
        ++preserved;
      }
    }
  }

  XLOGF(
      INFO,
      "[CTE] Cache migration: preserved={}, needsReEvaluation={}",
      preserved,
      result.affectedPrefixes.size());

  RibStats::STATS_raPolicyCacheMigrationSelective.add(1);
  RibStats::STATS_raPolicyCachePreserved.add(preserved);
  RibStats::STATS_raPolicyCacheInvalidated.add(result.affectedPrefixes.size());

  return result;
}

/* We only replace instead of updating route attribute policy.
   Each time the route attribute policy is replaced, when there is delta and
   not in read-only mode, trigger fib programming. */
bool RibDC::replaceRouteAttributePolicy(
    std::unique_ptr<RouteAttributePolicy> newPolicy) {
  RibStats::STATS_raPolicyRcvd.add(1);

  bool hasUpdate = false;
  bool needsReEvaluation = false;
  std::vector<folly::CIDRNetwork> prefixesNeedingReEvaluation;

  if (routeAttributePolicy_) {
    if (!newPolicy) {
      // Policy cleared - need full re-evaluation
      hasUpdate = true;
      needsReEvaluation = true;
    } else {
      // Both old and new policy exist - single-pass comparison and migration
      auto migrationStart = std::chrono::steady_clock::now();
      auto migrationResult =
          migrateRouteAttributePolicyCache(*routeAttributePolicy_, *newPolicy);
      RibStats::STATS_raPolicyCacheMigrationTimeMs.addValue(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - migrationStart)
              .count());
      hasUpdate = migrationResult.hasUpdate;
      needsReEvaluation = migrationResult.needsReEvaluation;
      prefixesNeedingReEvaluation = std::move(migrationResult.affectedPrefixes);
    }
  } else {
    // No existing policy - hasUpdate if newPolicy is not nullptr
    hasUpdate = (newPolicy != nullptr);
    needsReEvaluation = hasUpdate;
  }

  /* We should replace routeAttributePolicy_ not only when its content changes
     but also when its expiration time changes. */
  if (hasUpdate) {
    XLOG(DBG1, "[CTE] Updating RouteAttributePolicy.");

    routeAttributePolicy_ = std::move(newPolicy);

    // Save upon receipt of RouteAttributePolicy
    saveRibPolicyState();

    if (fsdbSyncer_) {
      fsdbSyncer_->setRouteAttributePolicy(
          routeAttributePolicy_
              ? std::optional(routeAttributePolicy_->toThrift())
              : std::nullopt);
    }
    scheduleRouteAttributePolicyTimer();
    RibStats::STATS_raPolicyUpdate.add(1);
  }

  /*
   * Only trigger a prepareFibProgramming with the following conditions
   * fulfilled:
   *  1. needsReEvaluation: True => policy update beyond refreshing;
   *  2. ribEoRReceived: True => already fulfilled the initial FULL_SYNC;
   *
   * Note: in case ribEoRReceived: False, the FULL_SYNC will be automatically
   * triggered after receiving the start signal inside
   * `processRibInInitialPathComputation`.
   */
  if (needsReEvaluation && ribEoRReceived_) {
    XLOG(INFO)
        << "[CTE] needsReEvaluation and ribEoRReceived, triggering path selection";

    /*
     * Selective re-evaluation: instead of iterating ALL ribEntries_, only
     * process collected prefixes. This is O(affected prefixes) instead of
     * O(all ribEntries_).
     */
    if (!prefixesNeedingReEvaluation.empty()) {
      // Selective re-evaluation: only affected prefixes
      for (const auto& prefix : prefixesNeedingReEvaluation) {
        auto ribIt = ribEntries_.find(prefix);
        if (ribIt != ribEntries_.end()) {
          ribIt->second.requirePathSelection();
        }
      }
      XLOGF(
          INFO,
          "[CTE] Selective re-evaluation: {} prefixes",
          prefixesNeedingReEvaluation.size());
      RibStats::STATS_raPolicyReEvalPrefixes.add(
          prefixesNeedingReEvaluation.size());
    } else {
      /*
       * Fallback: full re-evaluation when affectedPrefixes is empty.
       * This can happen when a statement is removed/changed but no cached
       * prefix matched it (e.g., cache not fully populated, BGP just
       * restarted, or policy was cleared).
       */
      for (auto& [_, ribEntry] : ribEntries_) {
        ribEntry.requirePathSelection();
      }
      XLOGF(
          INFO,
          "[CTE] Full re-evaluation fallback: {} prefixes, "
          "affectedPrefixes empty, cache may not have been fully populated",
          ribEntries_.size());
      RibStats::STATS_raPolicyReEvalPrefixes.add(ribEntries_.size());
    }

    schedulePrepareFibProgrammingTimer();
  }

  return hasUpdate;
}

/* We only replace instead of updating path selection policy.
   Each time the path selection policy is replaced, we also need to save
   path selection policy to disk. After that, when there is delta and not
   in read-only mode, trigger fib programming. */
bool RibDC::replacePathSelectionPolicy(
    std::unique_ptr<PathSelectionPolicy> newPolicy,
    bool isBootstrap) {
  RibStats::STATS_psPolicyRcvd.add(1);

  // hasUpdate is true when newPolicy is different from pathSelectionPolicy_
  bool hasUpdate;

  if (pathSelectionPolicy_) {
    /* When pathSelectionPolicy_ has cached one policy, hasUpdate if
       1. newPolicy == nullptr
       OR
       2. cached policy has delta with new one AND newPolicy has larger or
          equal version */
    hasUpdate = (newPolicy == nullptr) ||
        ((*pathSelectionPolicy_ != *newPolicy) &&
         pathSelectionPolicy_->getVersion() <= newPolicy->getVersion());
  } else {
    // pathSelectionPolicy_ does not cache anything, hasUpdate if newPolicy is
    // not nullptr
    hasUpdate = (newPolicy != nullptr);
  }

  if (hasUpdate) {
    XLOG(DBG1, "[CPS] Updating PathSelectionPolicy.");

    pathSelectionPolicy_ = std::move(newPolicy);

    // Only log and append to history for real policy updates, not bootstrap
    if (!isBootstrap) {
      // Save upon receipt of PathSelectionPolicy
      saveRibPolicyState();
      XLOGF(
          INFO,
          "[CPS] PathSelectionPolicy version: {}",
          getPathSelectionPolicyVersion());

      appendRibPolicyChangeHistory("CPS", getPathSelectionPolicyVersion());
    }

    if (fsdbSyncer_) {
      fsdbSyncer_->setPathSelectionPolicy(
          pathSelectionPolicy_ ? std::optional(pathSelectionPolicy_->toThrift())
                               : std::nullopt);
    }
    RibStats::STATS_psPolicyUpdate.add(1);

    if (ribPolicyLogger_) {
      int64_t psPolicyVersion = getPathSelectionPolicyVersion();
      int64_t rfPolicyVersion = getRouteFilterPolicyVersion();
      ribPolicyLogger_->log(psPolicyVersion, rfPolicyVersion);
    }
  }

  /*
   * Only trigger a FULL_SYNC with the following conditions fulfilled:
   *  1. hasUpdate: True => policy update;
   *  2. ribEoRReceived: True => already fulfilled the initial FULL_SYNC;
   *
   * Note: in case ribEoRReceived: False, the FULL_SYNC will be automatically
   * triggered after receiving the start signal inside
   * `processRibInInitialPathComputation`.
   */
  if (hasUpdate && ribEoRReceived_) {
    XLOG(INFO, "[CPS] hasUpdate and ribEoRReceived, triggering FULL_SYNC");
    // recompute all the paths, similar to fullSync but will send out the
    // update announcement to the peers
    for (auto& [prefix, ribEntry] : ribEntries_) {
      ribEntry.requirePathSelection();
    }
    schedulePrepareFibProgrammingTimer();
  }

  return hasUpdate;
}

/*
 * Apply RouteAttributePolicy to overwrite route attributes of computed RIB
 * entries.
 */
void RibDC::overwriteRouteAttributes(
    const std::unordered_set<folly::CIDRNetwork>& prefixes,
    bool fullRibWalk) {
  auto overwriteStartTime = std::chrono::steady_clock::now();

  // trigger rib policy calculation
  RouteAttributePolicy::RibChange ribChange;

  /*
   * Apply rib policy lbw to rib entries:
   *  - If no rib policy statement matched a ribEntry (including ribPolicy not
   *    available or matching statement is expired), we reset the rib policy
   *    ucmp weight (if available);
   *  - Otherwise, we let rib policy overwrite the route attributes
   */
  for (auto& [prefix, ribEntry] : ribEntries_) {
    if (!prefixes.contains(prefix)) {
      continue;
    }
    auto startTime = std::chrono::steady_clock::now();

    bool matched = false;
    if (routeAttributePolicy_) {
      matched =
          routeAttributePolicy_->overwriteRouteAttributes(ribEntry, ribChange);
    }
    if (!matched) {
      // if rib has non-empty lbw, reset it to 0 and add rib to update list
      if (ribEntry.getRibPolicyUcmpWeight().has_value()) {
        ribEntry.setRibPolicyUcmpWeight(0);
        ribChange.updatedRoutes.emplace(prefix);
      }
    }

    RibStats::STATS_ribRouteAttributeOverwriteTimeMs.addValue(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime)
            .count());
  }

  // update fib batch list
  for (const auto& prefix : ribChange.updatedRoutes) {
    auto ribIt = ribEntries_.find(prefix);
    CHECK(ribIt != ribEntries_.end());
    auto& ribEntry = ribIt->second;
    if (!ribEntry.isOnFibBatchList()) {
      fibBatchList_.push_back(ribEntry);
    }
  }

  if (fullRibWalk) {
    auto routeAttributeOverwriteTimeMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - overwriteStartTime);

    XLOGF(
        INFO,
        "Route attribute overwrite in a FullRibWalk for {} ribEntries took {} ms",
        ribEntries_.size(),
        routeAttributeOverwriteTimeMs.count());
    RibStats::STATS_ribFullSyncRouteAttributeOverwriteTimeMs.addValue(
        routeAttributeOverwriteTimeMs.count());
  }
}

} // namespace facebook::bgp
