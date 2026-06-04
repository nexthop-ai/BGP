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

#include <folly/logging/xlog.h>
#include <gflags/gflags.h>

#include "neteng/fboss/bgp/cpp/common/platform/dc/PlatformConstant.h"
#include "neteng/fboss/bgp/cpp/rib/RibBase.h"
#include "neteng/fboss/bgp/cpp/rib/RibPolicyLogger.h"

DECLARE_bool(publish_rib_to_fsdb);

namespace facebook::bgp {

class FsdbSyncer;

class RibDC : public RibBase {
 public:
  RibDC(
      const std::unordered_map<folly::CIDRNetwork, thrift::BgpNetwork>&
          localRoutes,
      const BgpGlobalConfig& globalConfig,
      const std::optional<bgp_policy::BgpPolicies>& policyConfig,
      nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage>& ribInQ,
      MonitoredMPMCQueue<RibOutMessage>& ribOutQ,
      const std::string& platform,
      FsdbSyncer* fsdbSyncer,
      std::shared_ptr<NexthopCache> nextHopCache = nullptr,
      uint16_t fibAgentPort = kPlatformFibAgentPort,
      uint32_t fibAgentRecvTimeout = kPlatformFibAgentRecvTimeout);
  ~RibDC() override = default;

 protected:
  void createFib() override;
  void enqueueRibUpdateToFsdb() override;
  void postRouteFilterPolicyReplaced() override;
  void processNexthopResolutionUpdate(
      const NexthopResolutionUpdate& nexthopResolutionUpdate) noexcept override;
  void replaceRibPolicy(
      std::unique_ptr<RibPolicy> newRibPolicy,
      bool isBootstrap = false) override;

  void overwriteRouteAttributes(
      const std::unordered_set<folly::CIDRNetwork>& prefixes,
      bool fullRibWalk = false) override;

  folly::coro::Task<void> processRibPolicyMsgLoop() noexcept override;
  void handlePathSelectionPolicySetMsg(
      const PathSelectionPolicySetMsg& msg) noexcept override;
  void handlePathSelectionPolicyClearMsg() noexcept override;

  /**
   * Result of single-pass cache migration that combines policy comparison
   * and cache migration to avoid triple iteration over statements.
   */
  struct CacheMigrationResult {
    bool hasUpdate{false};
    bool needsReEvaluation{false};
    std::vector<folly::CIDRNetwork> affectedPrefixes;
  };

  /**
   * Replace route attribute policy (CTE). Each time the route attribute
   * policy is replaced, when there is delta and not in read-only mode,
   * trigger fib programming.
   *
   * @return hasUpdate — whether the newPolicy contains update (including
   *         the expiration time change).
   */
  virtual bool replaceRouteAttributePolicy(
      std::unique_ptr<RouteAttributePolicy> newPolicy);

  /**
   * Single-pass cache migration that combines policy comparison and cache
   * migration. Compares statements ONCE and migrates cache entries, returning
   * all information needed by replaceRouteAttributePolicy().
   *
   * This eliminates the triple statement comparison that previously occurred:
   *   1. operator!= (compares statements)
   *   2. needsReEvaluation() (iterates statements again)
   *   3. migrateRouteAttributePolicyCache() (iterates statements third time)
   */
  CacheMigrationResult migrateRouteAttributePolicyCache(
      RouteAttributePolicy& oldPolicy,
      RouteAttributePolicy& newPolicy);

  void scheduleRouteAttributePolicyTimer() noexcept;

  /**
   * Replace path selection policy (CPS). Each time the path selection
   * policy is replaced, save to disk and trigger fib programming when
   * there is delta and not in read-only mode.
   *
   * @param newPolicy the new PathSelectionPolicy to apply, or nullptr to clear.
   * @param isBootstrap if true, skip saving to disk and appending to
   *        change history (used during constructor restore).
   * @return hasUpdate — whether the newPolicy contains update.
   */
  virtual bool replacePathSelectionPolicy(
      std::unique_ptr<PathSelectionPolicy> newPolicy,
      bool isBootstrap = false);

 protected:
  // scuba logger for rib policy
  std::shared_ptr<rfe::ScubaData> scubaLogger_{nullptr};
  std::unique_ptr<RibPolicyLogger> ribPolicyLogger_{nullptr};

  void maybeStartFsdbSyncer();

  /* Raw pointer owned by Main.cpp; lifetime managed externally. */
  FsdbSyncer* fsdbSyncer_{nullptr};

 private:
  /* DC-only CTE message handlers called from processRibPolicyMsgLoop. */
  void handleRouteAttributePolicySetMsg(
      const RouteAttributePolicySetMsg& msg) noexcept;
  void handleRouteAttributePolicyClearMsg() noexcept;
  void handleRouteAttributePolicyTimerMsg() noexcept;

  /*
   * Helper: iterate over the nexthop IPs in a NexthopResolutionUpdate,
   * look each up in conditionalLocalRoutes_, and invoke processMessage for
   * every (prefix, PrefixPathIds) pair that matches. Direct invocation (not
   * ribInQ_.push) avoids a deadlock — this is called from processRibInMsgLoop
   * which is the sole consumer of ribInQ_, so a push there would block
   * forever if the queue is full.
   */
  template <typename MessageProcessor>
  void processConditionalRoutesForNexthops(
      const std::vector<folly::IPAddress>& nexthopIps,
      MessageProcessor&& processMessage) noexcept;

  bool fsdbSyncerStarted_{false};

  /*
   * One-shot flag: set true after the first NexthopResolutionUpdate has been
   * processed (and any conditional routes advertised). Used to suppress
   * repeat pushes of the RibOutNexthopResolutionReceived signal to
   * PeerManager — PeerManager only needs the first one to satisfy its
   * NDP-received precondition for sending RibInInitialPathComputation.
   */
  bool firstNdpSignalSent_{false};

  friend class RibFixture;
  friend class RibFsdbFixture;
  friend class MockRib;
};

template <typename MessageProcessor>
void RibDC::processConditionalRoutesForNexthops(
    const std::vector<folly::IPAddress>& nexthopIps,
    MessageProcessor&& processMessage) noexcept {
  XLOGF(
      INFO, "Processing conditional routes for {} nexthops", nexthopIps.size());
  for (const auto& ipaddr : nexthopIps) {
    auto it = conditionalLocalRoutes_.find(ipaddr);
    if (it != conditionalLocalRoutes_.end()) {
      for (const auto& prefix : it->second) {
        PrefixPathIds pfxPathIds{{prefix, kDefaultPathID}};
        processMessage(prefix, std::move(pfxPathIds));
      }
    }
  }
}

} // namespace facebook::bgp
