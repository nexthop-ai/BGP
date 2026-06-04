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

#include <deque>

#include <folly/Synchronized.h>
#include <folly/container/F14Map.h>
#include <openr/monitor/SystemMetrics.h>

#include "neteng/fboss/bgp/cpp/common/BgpModuleBase.h"
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/config/Config.h"
#include "neteng/fboss/bgp/cpp/stats/Stats.h"
#include "neteng/fboss/bgp/cpp/watchdog/MonitoredModule.h"

namespace facebook::bgp {

using MonitoredPathT = std::vector<std::string>;
using QueueSizeMapT = folly::F14FastMap<std::string, int32_t>;

/**
 * System resource limits derived from gflags in Main.cpp.
 * Passed to Watchdog at construction, accessible to HealthValidator.
 */
struct SystemResourceLimits {
  int64_t rssLimitBytes{0};
};

struct HeartbeatSnapshot {
  int64_t timestampSeconds{0};
  int64_t heartbeatValue{0};
};

class Watchdog : public BgpModuleBase {
 public:
  explicit Watchdog(
      std::shared_ptr<const Config> config,
      SystemResourceLimits resourceLimits = {});

  const SystemResourceLimits& getSystemResourceLimits() const {
    return resourceLimits_;
  }

  folly::F14FastMap<std::string, std::deque<HeartbeatSnapshot>>
  getHeartbeatSnapshots() const;

  virtual ~Watchdog() override {}

  void run() noexcept override;
  void stop() noexcept override;

  void monitorModule(
      const std::string& moduleName,
      MonitoredModule& module) noexcept;

  /**
   * Get the queue sizes for the modules by querying the watchdog.
   *
   * @param paths The list of query paths to query. The path can contain
   * the module name or module name followed by queue name, e.g.
   * ["module1", "module2.queue1", "module3.queue2.queue3"]
   * @return The map of queue sizes where the key is the queue path and
   * the value is the size of the queue.
   */
  QueueSizeMapT getQueueSizes(
      const std::unique_ptr<MonitoredPathT>& paths) noexcept;

 private:
  folly::coro::Task<void> monitorQueueSizeLoop();
  folly::coro::Task<void> monitorSystemMetricsLoop();
  folly::coro::Task<void> snapshotHeartbeatsLoop();
  folly::coro::Task<void> dumpHeapProfileLoop(const int32_t intervalInSecond);
  void updateSystemMetrics();

  void processQueueSizeMap(const QueueSizeMapT& queueSizeMap);

  /*
   * This is the util function to send notification to sessionManager.
   *
   * @param: queueName - queue name for logging purpose.
   * @param: val - queue size for logging purpose.
   * @param: maybePeerId - the std::optional wrapper of BgpPeerId.
   *                       std::nullopt will represent to all peers.
   * @param: opStatus - operational status (PAUSE/RESUME)
   * @return: none
   */
  void sendNotificationToSessionMgrWrapper(
      const std::string& queueName,
      int64_t val,
      std::optional<nettools::bgplib::BgpPeerId>&& maybePeerId,
      OperationStatus opStatus);
  void sendNotificationToSessionMgr(
      std::optional<nettools::bgplib::BgpPeerId>&& maybePeerId,
      OperationStatus opStatus);

  /*
   * This is the util function to send notification to Rib.
   *
   * @param: queueName - queue name for logging purpose.
   * @param: val - queue size for logging purpose.
   * @param: opStatus - operational status (PAUSE/RESUME)
   */
  void sendNotificationToRib(
      const std::string& queuName,
      int64_t val,
      OperationStatus opStatus);

  /*
   * This is the util function to parse queueName based on the matcher string.
   *
   * @param: queueName - the std::string representation of queue to be parsed
   * @param: matcher - the pattern substring to be matched against queueNAme.
   *
   * @return: std::nullopt - no valid matching/parsing result.
   *          nettools::bgplib::BgpPeerId - a valid BgpPeerId
   */
  static std::optional<nettools::bgplib::BgpPeerId> matchQueueName(
      const std::string& queueName,
      const std::string& matcher);

  /*
   * Set of util function to process ingress and egress per-peer queue
   *  - 1) build-up
   *  - 2) mitigation
   */
  void processRibInQueueBuildUp(const std::string& queueName, uint64_t val);
  void processRibInQueueMitigation(const std::string& queueName, uint64_t val);
  void processRibOutQueueBuildUp(const std::string& queueName, uint64_t val);
  void processRibOutQueueMitigation(const std::string& queueName, uint64_t val);
  void maybeProcessIngressQueueBuildUp(
      const std::string& queueName,
      uint64_t val);
  void maybeProcessIngressQueueMitigation(
      const std::string& queueName,
      uint64_t val);
  void maybeProcessEgressQueueBuildUp(
      const std::string& queueName,
      uint64_t val);
  void maybeProcessEgressQueueMitigation(
      const std::string& queueName,
      uint64_t val);

  // make the function private for UT purpose
  void setMonitoredPaths(MonitoredPathT&& paths) {
    monitoredPaths_ = std::move(paths);
  }

  // make the function private for UT purpose
  void setQueueSizeThreshold(
      uint64_t pauseThreshold,
      uint64_t resumeThreshold) {
    sharedQueueSizePauseThreshold_ = pauseThreshold;
    sharedQueueSizeResumeThreshold_ = resumeThreshold;
    perPeerQueueSizePauseThreshold_ = pauseThreshold;
    perPeerQueueSizeResumeThreshold_ = resumeThreshold;
  }

  // shared_ptr to the BGP++ config
  std::shared_ptr<const Config> config_;

  // System resource limits derived from gflags
  SystemResourceLimits resourceLimits_;

  folly::F14FastMap<std::string, std::reference_wrapper<MonitoredModule>>
      monitoredModules_;

  // System metrics collector
  openr::SystemMetrics systemMetrics_;
  std::chrono::steady_clock::time_point startTime_;

  /* Written by Watchdog's evb thread, read by BgpService's thrift thread via
   * getHeartbeatSnapshots(). Keeps up to kMaxHeartbeatSnapshots per module
   * for stable oldest-baseline drift detection. */
  folly::Synchronized<
      folly::F14FastMap<std::string, std::deque<HeartbeatSnapshot>>>
      heartbeatSnapshots_;

  /*
   * This is the state to remember queue build-up or not. They will be used to
   * determine if a resume message should be sent out.
   */
  bool isRibInQueueBuildUp_{false};
  bool isRibOutQueueBuildUp_{false};
  folly::F14NodeMap<std::string /* queue name */, bool /* in build-up state */>
      isPeerQueueBuildUp_;

  /*
   * This is the list of modules watchdog will montior on.
   * NOTE: Watchdog will skip the monitoring if the paths given is empty.
   */
  static inline MonitoredPathT monitoredPaths_{
      fmt::format("{}.{}", kModuleRib, kQueueNameRibIn),
      fmt::format("{}.{}", kModuleRib, kQueueNameRibOut),
      fmt::format("{}.{}", kModulePeerManager, kModuleSessionManager),
  };

  static inline uint64_t sharedQueueSizePauseThreshold_{
      kWatchdogSharedQueueSizePauseThreshold};
  static inline uint64_t sharedQueueSizeResumeThreshold_{
      kWatchdogSharedQueueSizeResumeThreshold};
  static inline uint64_t perPeerQueueSizePauseThreshold_{
      kWatchdogPerPeerQueueSizePauseThreshold};
  static inline uint64_t perPeerQueueSizeResumeThreshold_{
      kWatchdogPerPeerQueueSizeResumeThreshold};

// per class placeholder for test code injection
// only need to be setup once here
#ifdef Watchdog_TEST_FRIENDS
  Watchdog_TEST_FRIENDS
#endif
      friend class WatchdogTestFixture;
};

} // namespace facebook::bgp
