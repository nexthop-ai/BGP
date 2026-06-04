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

#include "neteng/fboss/bgp/cpp/watchdog/Watchdog.h"
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Sleep.h>
#include <folly/logging/xlog.h>
#include "neteng/fboss/bgp/cpp/watchdog/MemProfiler.h"
#include "neteng/fboss/bgp/cpp/watchdog/QueryTree.h"

namespace facebook::bgp {

Watchdog::Watchdog(
    std::shared_ptr<const Config> config,
    SystemResourceLimits resourceLimits)
    : BgpModuleBase(kModuleWatchdog),
      config_(std::move(config)),
      resourceLimits_(std::move(resourceLimits)),
      startTime_(std::chrono::steady_clock::now()) {}

void Watchdog::run() noexcept {
  // periodically check queue sizes.
  asyncScope_.add(co_withExecutor(&evb_, monitorQueueSizeLoop()));

  // periodically monitor system metrics.
  asyncScope_.add(co_withExecutor(&evb_, monitorSystemMetricsLoop()));

  // periodically snapshot heartbeat counters for stall detection.
  asyncScope_.add(co_withExecutor(&evb_, snapshotHeartbeatsLoop()));

  auto memConfig = config_->getMemoryProfilingConfig();
  if (memConfig && *memConfig->enable_memory_profiling()) {
    // enable heap profiling
    setHeapProfilingMode(true);

    // kick off coro task to do periodic dump
    asyncScope_.add(co_withExecutor(
        &evb_, dumpHeapProfileLoop(*memConfig->heap_dump_interval_s())));
  } else {
    setHeapProfilingMode(false);
  }

  // pump up the eventbase to run all coro tasks.
  evb_.loopForever();
}

void Watchdog::stop() noexcept {
  // cancel all coroutines
  folly::coro::blockingWait(asyncScope_.cancelAndJoinAsync());

  // terminate evb to deterministically shutdown evb
  evb_.terminateLoopSoon();
}

folly::coro::Task<void> Watchdog::monitorQueueSizeLoop() {
  XLOG(INFO, "Starting queue size monitoring coro task");

  // initialize the counter to 0
  fb303::ThreadCachedServiceData::get()->setCounter(
      BgpStats::kWatchdogNumQueueSizeCheck, 0);

  while (true) {
    // when cancelAndJoinAsync is called, guaranteed to exit
    co_await folly::coro::co_safe_point;

    // sleep a constant time and yield before next time checking
    co_await folly::coro::sleep(kWatchdogQueueSizeCheckInterval);

    // increment the counter of run
    fb303::ThreadCachedServiceData::get()->incrementCounter(
        BgpStats::kWatchdogNumQueueSizeCheck, 1);

    // monitor queue size with post-processing when path is not empty
    QueueSizeMapT queueSizeMap{};
    if (monitoredPaths_.size() > 0) {
      queueSizeMap =
          getQueueSizes(std::make_unique<MonitoredPathT>(monitoredPaths_));
    }

    // process queue size map to trigger notification
    processQueueSizeMap(queueSizeMap);
  }

  XLOG(INFO, "[Exit] Successfully stopped queue size monitoring task");
}

void Watchdog::monitorModule(
    const std::string& moduleName,
    MonitoredModule& module) noexcept {
  if (!module.isMonitored() &&
      monitoredModules_.find(moduleName) == monitoredModules_.end()) {
    monitoredModules_.emplace(moduleName, std::ref(module));
    module.markMonitored();
  } else {
    XLOGF(INFO, "Module {} is already monitored.", moduleName);
  }
}

/**
 * Get the queue sizes for the modules by querying the watchdog.
 *
 * @param paths The list of query paths to query. The path can contain
 * the module name or module name followed by queue name, e.g.
 * ["module1", "module2.queue1", "module3.queue2.queue3"]
 * @return The map of queue sizes where the key is the queue path and
 * the value is the size of the queue.
 */
QueueSizeMapT Watchdog::getQueueSizes(
    const std::unique_ptr<MonitoredPathT>& paths) noexcept {
  QueueSizeMapT queueSizes;

  // Build query tree
  QueryTree queryTree;
  for (const auto& path : *paths) {
    queryTree.addPath(path);
  }
  if (paths->empty()) {
    queryTree.root.markLeaf();
  }

  for (const auto& [moduleName, module] : monitoredModules_) {
    auto modulePreifx = moduleName + ".";

    auto queryNode = &queryTree.root;
    if (!queryNode->isLeaf) {
      if (!queryNode->children.contains(moduleName)) {
        // skip if module is not in query tree
        continue;
      }
      queryNode = queryNode->children.at(moduleName).get();
    }

    auto moduleQueueSizes = module.get().getQueueSizes(queryNode);
    for (const auto& [queueName, queueSize] : moduleQueueSizes) {
      queueSizes.emplace(modulePreifx + queueName, queueSize);
    }
  }
  return queueSizes;
}

void Watchdog::processQueueSizeMap(const QueueSizeMapT& queueSizeMap) {
  for (const auto& [queueName, val] : queueSizeMap) {
    if (queueName.find(kQueueNameRibIn) != std::string::npos) {
      /*
       * ribInQueue_ - shared queue threshold violation
       */
      if (val >= sharedQueueSizePauseThreshold_) {
        processRibInQueueBuildUp(queueName, val);
      } else if (val < sharedQueueSizeResumeThreshold_) {
        processRibInQueueMitigation(queueName, val);
      }
      fb303::ThreadCachedServiceData::get()->setCounter(
          BgpStats::kRibInQueueSize, val);
    } else if (queueName.find(kQueueNameRibOut) != std::string::npos) {
      /*
       * ribOutQueue_ - shared queue threshold violation
       */
      if (val >= sharedQueueSizePauseThreshold_) {
        processRibOutQueueBuildUp(queueName, val);
      } else if (val < sharedQueueSizeResumeThreshold_) {
        processRibOutQueueMitigation(queueName, val);
      }
      fb303::ThreadCachedServiceData::get()->setCounter(
          BgpStats::kRibOutQueueSize, val);
    } else {
      /*
       * Per-peer queue threshold violation (ingess or egress)
       */
      if (val >= perPeerQueueSizePauseThreshold_) {
        maybeProcessIngressQueueBuildUp(queueName, val);
        maybeProcessEgressQueueBuildUp(queueName, val);
      } else if (val < perPeerQueueSizeResumeThreshold_) {
        maybeProcessIngressQueueMitigation(queueName, val);
        maybeProcessEgressQueueMitigation(queueName, val);
      }
    }
  }
}

void Watchdog::processRibInQueueBuildUp(
    const std::string& queueName,
    uint64_t val) {
  sendNotificationToSessionMgrWrapper(
      queueName,
      val,
      std::nullopt, /* for all peers */
      OperationStatus::PAUSE);
  isRibInQueueBuildUp_ = true;
}

void Watchdog::processRibInQueueMitigation(
    const std::string& queueName,
    uint64_t val) {
  if (isRibInQueueBuildUp_) {
    sendNotificationToSessionMgrWrapper(
        queueName,
        val,
        std::nullopt, /* for all peers */
        OperationStatus::RESUME);
  }
  isRibInQueueBuildUp_ = false;
}

void Watchdog::processRibOutQueueBuildUp(
    const std::string& queueName,
    uint64_t val) {
  sendNotificationToRib(queueName, val, OperationStatus::PAUSE);
  isRibOutQueueBuildUp_ = true;
}

void Watchdog::processRibOutQueueMitigation(
    const std::string& queueName,
    uint64_t val) {
  if (isRibOutQueueBuildUp_) {
    sendNotificationToRib(queueName, val, OperationStatus::RESUME);
  }
  isRibOutQueueBuildUp_ = false;
}

void Watchdog::maybeProcessIngressQueueBuildUp(
    const std::string& queueName,
    uint64_t val) {
  auto maybePeerIdIngress = matchQueueName(queueName, kIngressQueueSuffix);
  if (maybePeerIdIngress == std::nullopt) {
    // skip notification since this is not an ingress queue
    return;
  }

  if (!isPeerQueueBuildUp_.contains(queueName)) {
    isPeerQueueBuildUp_.emplace(queueName, true);
  }

  // mark queue in build-up state
  isPeerQueueBuildUp_.at(queueName) = true;

  // detect queue build-up. Send notification.
  sendNotificationToSessionMgrWrapper(
      queueName,
      val,
      std::move(maybePeerIdIngress), /* for a specific peer */
      OperationStatus::PAUSE);
}

void Watchdog::maybeProcessEgressQueueBuildUp(
    const std::string& queueName,
    uint64_t val) {
  auto maybePeerIdEgress = matchQueueName(queueName, kEgressQueueSuffix);
  if (maybePeerIdEgress == std::nullopt) {
    // skip notification since this is not an egress queue
    return;
  }

  if (!isPeerQueueBuildUp_.contains(queueName)) {
    isPeerQueueBuildUp_.emplace(queueName, true);
  }

  // mark queue in build-up state
  isPeerQueueBuildUp_.at(queueName) = true;

  XLOGF(
      WARN,
      "WARNING! Detected {} queue size {} over threshold {}",
      queueName,
      val,
      perPeerQueueSizePauseThreshold_);

  // TODO: using egress queue back-pressure to handle.
}

void Watchdog::maybeProcessIngressQueueMitigation(
    const std::string& queueName,
    uint64_t val) {
  auto maybePeerIdIngress = matchQueueName(queueName, kIngressQueueSuffix);
  if (maybePeerIdIngress == std::nullopt) {
    // skip notification since this is not an ingress queue
    return;
  }
  if (!isPeerQueueBuildUp_.contains(queueName)) {
    // skip queue name processing since it is not in build-up state
    return;
  }

  if (isPeerQueueBuildUp_.at(queueName)) {
    // mark queue not in build-up state
    isPeerQueueBuildUp_.at(queueName) = false;

    // first time detect queue drop below threshold. Send notification.
    sendNotificationToSessionMgrWrapper(
        queueName,
        val,
        std::move(maybePeerIdIngress), /* for a specific peer */
        OperationStatus::RESUME);
  }
}

void Watchdog::maybeProcessEgressQueueMitigation(
    const std::string& queueName,
    uint64_t val) {
  auto maybePeerIdEgress = matchQueueName(queueName, kEgressQueueSuffix);
  if (maybePeerIdEgress == std::nullopt) {
    // skip notification since this is not an egress queue
    return;
  }

  if (!isPeerQueueBuildUp_.contains(queueName)) {
    // skip queue name processing since it is not in build-up state
    return;
  }

  if (isPeerQueueBuildUp_.at(queueName)) {
    // mark queue not in build-up state
    isPeerQueueBuildUp_.at(queueName) = false;

    XLOGF(
        WARN,
        "Detected {} queue size {} below threshold {}",
        queueName,
        val,
        perPeerQueueSizeResumeThreshold_);

    // TODO: using egress queue back-pressure to handle.
  }
}

void Watchdog::sendNotificationToSessionMgrWrapper(
    const std::string& queueName,
    int64_t val,
    std::optional<nettools::bgplib::BgpPeerId>&& maybePeerId,
    OperationStatus opStatus) {
  XLOGF(
      WARN,
      "WARNING! Detected {} queue size {} {} threshold {}",
      queueName,
      val,
      opStatus == OperationStatus::PAUSE ? "over" : "below",
      opStatus == OperationStatus::PAUSE
          ? (maybePeerId.has_value() ? perPeerQueueSizePauseThreshold_
                                     : sharedQueueSizePauseThreshold_)
          : (maybePeerId.has_value() ? perPeerQueueSizeResumeThreshold_
                                     : sharedQueueSizeResumeThreshold_));

  // first time detect queue build-up. Send notification.
  sendNotificationToSessionMgr(std::move(maybePeerId), opStatus);
}

void Watchdog::sendNotificationToSessionMgr(
    std::optional<nettools::bgplib::BgpPeerId>&& maybePeerId,
    OperationStatus opStatus) {
  if (!monitoredModules_.contains(kModulePeerManager)) {
    // skip notification since kModulePeerManager module is not monitored
    return;
  }

  auto& peerMgrModule = monitoredModules_.at(kModulePeerManager).get();
  if (!peerMgrModule.monitoredItems_.rlock()->contains(kModuleSessionManager)) {
    // skip notification since kModuleSessionManager module is not monitored
    return;
  }

  // Assuming the variant holds a MonitoredModule reference
  auto& variant =
      peerMgrModule.monitoredItems_.rlock()->at(kModuleSessionManager);
  if (auto modulePtr =
          std::get_if<std::reference_wrapper<MonitoredModule>>(&variant)) {
    auto& notificationQ = modulePtr->get().getNotificationQueue();
    WatchdogEventMessage msg(std::move(maybePeerId), opStatus);
    notificationQ.push(std::move(msg));
  }
}

void Watchdog::sendNotificationToRib(
    const std::string& queueName,
    int64_t val,
    OperationStatus opStatus) {
  XLOGF(
      WARN,
      "WARNING! Detected {} queue size {} {} threshold {}",
      queueName,
      val,
      opStatus == OperationStatus::PAUSE ? "over" : "below",
      opStatus == OperationStatus::PAUSE ? sharedQueueSizePauseThreshold_
                                         : sharedQueueSizeResumeThreshold_);

  if (!monitoredModules_.contains(kModuleRib)) {
    // skip notification since kModuleRib module is not monitored
    return;
  }

  auto& ribModule = monitoredModules_.at(kModuleRib).get();
  auto& notificationQ = ribModule.getNotificationQueue();
  WatchdogEventMessage msg(std::nullopt, opStatus);
  notificationQ.push(std::move(msg));

  XLOGF(
      INFO,
      "Successfully sent {} signal to Rib module",
      opStatus == OperationStatus::PAUSE ? "PAUSE" : "RESUME");
}

std::optional<nettools::bgplib::BgpPeerId> Watchdog::matchQueueName(
    const std::string& queueName,
    const std::string& matcher) {
  const std::string perPeerQueueSuffix =
      fmt::format("{}.{}", kModulePeerManager, kModuleSessionManager);

  if (queueName.size() <= perPeerQueueSuffix.size() ||
      queueName.find(perPeerQueueSuffix) == std::string::npos) {
    return std::nullopt;
  }

  auto strippedStr = queueName.substr(perPeerQueueSuffix.size() + 1);
  uint64_t endPos = strippedStr.find(matcher);
  if (endPos == std::string::npos) {
    return std::nullopt;
  }

  // retrieve the substring of matched peer
  auto peerAddrAndPortStr = strippedStr.substr(0, endPos);
  auto pos = peerAddrAndPortStr.find('-');
  if (pos == std::string::npos) {
    return std::nullopt;
  }

  auto peerAddr = peerAddrAndPortStr.substr(0, pos);
  auto remotePort = peerAddrAndPortStr.substr(pos + 1);
  nettools::bgplib::BgpPeerId peerId(
      folly::IPAddress(peerAddr), std::stoul(remotePort));
  return peerId;
}

folly::coro::Task<void> Watchdog::monitorSystemMetricsLoop() {
  XLOG(INFO, "Starting system metrics monitoring coro task");

  while (true) {
    // when cancelAndJoinAsync is called, guaranteed to exit
    co_await folly::coro::co_safe_point;

    // sleep before next metrics update
    co_await folly::coro::sleep(kWatchdogSystemMetricsInterval);

    // Update system metrics
    updateSystemMetrics();
  }

  XLOG(INFO, "[Exit] Successfully stopped system metrics monitoring task");
}

void Watchdog::updateSystemMetrics() {
  // Update uptime counter
  const std::chrono::steady_clock::time_point now =
      std::chrono::steady_clock::now();
  const int64_t uptimeSeconds =
      std::chrono::duration_cast<std::chrono::seconds>(now - startTime_)
          .count();

  fb303::ThreadCachedServiceData::get()->setCounter(
      "bgpd.process.uptime.seconds", uptimeSeconds);

  // Update memory RSS counter
  const std::optional<size_t> rssMem = systemMetrics_.getRSSMemBytes();
  if (rssMem.has_value()) {
    fb303::ThreadCachedServiceData::get()->setCounter(
        "bgpd.process.memory.rss.bytes", static_cast<int64_t>(*rssMem));
  }

  // Update CPU percentage counter
  const std::optional<double> cpuPct = systemMetrics_.getCPUpercentage();
  if (cpuPct.has_value()) {
    const int64_t cpuPctInt = static_cast<int64_t>(*cpuPct);
    fb303::ThreadCachedServiceData::get()->setCounter(
        "bgpd.process.cpu.percent", cpuPctInt);
  }

  // Log periodic summary at INFO level (less frequent than individual metrics)
  std::chrono::steady_clock::time_point lastSummaryTime =
      std::chrono::steady_clock::now();
  const std::chrono::minutes kSummaryInterval = std::chrono::minutes(5);

  if (now - lastSummaryTime >= kSummaryInterval) {
    XLOGF(
        INFO,
        "BGP++ System Metrics - Uptime: {}s, RSS: {} bytes, CPU: {}%",
        uptimeSeconds,
        rssMem.value_or(0),
        cpuPct.value_or(0.0));
    lastSummaryTime = now;
  }
}

folly::coro::Task<void> Watchdog::snapshotHeartbeatsLoop() {
  XLOG(INFO, "Starting heartbeat snapshot coro task");

  static const std::vector<std::string> kHeartbeatModules = {
      kModuleRib,
      kModulePeerManager,
      kModuleSessionManager,
      kModuleNeighborWatcher,
      kModuleNexthopHandler,
      kModuleNetlinkWrapper,
      kModuleWatchdog,
  };

  while (true) {
    co_await folly::coro::co_safe_point;
    co_await folly::coro::sleepReturnEarlyOnCancel(
        kWatchdogHeartbeatSnapshotInterval);

    try {
      auto now = std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count();

      folly::F14FastMap<std::string, HeartbeatSnapshot> localSnapshots;
      for (const auto& moduleName : kHeartbeatModules) {
        auto counterKey = fmt::format("bgpd.heartbeat.{}", moduleName);
        auto optVal = fb303::ThreadCachedServiceData::get()
                          ->getServiceData()
                          ->getCounterIfExists(counterKey);
        if (optVal.has_value()) {
          localSnapshots[moduleName] = HeartbeatSnapshot{now, *optVal};
        }
      }

      auto snapshots = heartbeatSnapshots_.wlock();
      for (auto& [moduleName, snap] : localSnapshots) {
        auto& deq = (*snapshots)[moduleName];
        deq.push_back(std::move(snap));
        while (deq.size() > kMaxHeartbeatSnapshots) {
          deq.pop_front();
        }
      }
    } catch (const std::exception& ex) {
      XLOGF(ERR, "Heartbeat snapshot failed: {}", ex.what());
    }
  }

  XLOG(INFO, "[Exit] Successfully stopped heartbeat snapshot task");
}

folly::F14FastMap<std::string, std::deque<HeartbeatSnapshot>>
Watchdog::getHeartbeatSnapshots() const {
  return *heartbeatSnapshots_.rlock();
}

folly::coro::Task<void> Watchdog::dumpHeapProfileLoop(
    const int32_t intervalInSecond) {
  XLOGF(
      INFO,
      "Starting heap dump profiling coro task with {}s interval",
      intervalInSecond);

  while (true) {
    // when cancelAndJoinAsync is called, guaranteed to exit
    co_await folly::coro::co_safe_point;

    co_await folly::coro::sleep(std::chrono::seconds(intervalInSecond));

    // Dump heap profile periodically
    getHeapDump("bgpcpp");
  }

  XLOG(INFO, "[Exit] Successfully stopped heap dump profiling task");
}

} // namespace facebook::bgp
