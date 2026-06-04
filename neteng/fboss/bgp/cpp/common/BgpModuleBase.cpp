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

#include <fmt/format.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Sleep.h>
#include <folly/logging/xlog.h>
#include <folly/system/ThreadName.h>

#include <fb303/ThreadCachedServiceData.h>
#include "neteng/fboss/bgp/cpp/common/BgpModuleBase.h"

namespace facebook::bgp {

/**
 * @brief Schedule the heartbeat loop on evb_
 */
void BgpModuleBase::scheduleHeartbeatLoop() noexcept {
  asyncScope_.add(co_withExecutor(&evb_, heartbeatLoop()));
}

std::thread BgpModuleBase::runInThread() {
  auto thread = std::thread([this]() {
    XLOGF(INFO, "Starting {} thread...", this->moduleName_);

    const auto threadName = fmt::format("bgpcpp-{}", this->moduleName_);
    folly::setThreadName(threadName);

    // periodically pump heartbeat
    this->scheduleHeartbeatLoop();
    this->run();
    XLOGF(INFO, "[Exit] {} thread got stopped", this->moduleName_);
  });

  // Ensure that evb has started running before returning
  evb_.waitUntilRunning();

  return thread;
}

/**
 * @brief Coroutine to periodically update heartbeat counter
 */
folly::coro::Task<void> BgpModuleBase::heartbeatLoop() noexcept {
  auto heartbeatKey = fmt::format("bgpd.heartbeat.{}", moduleName_);

  // initialize heartbeat counter to 0
  fb303::ThreadCachedServiceData::get()->setCounter(heartbeatKey, 0);

  while (true) {
    // when cancelAndJoinAsync is called, guaranteed to exit
    co_await folly::coro::co_safe_point;

    co_await folly::coro::sleep(std::chrono::seconds(1));

    fb303::ThreadCachedServiceData::get()->incrementCounter(heartbeatKey, 1);
  }
}

} // namespace facebook::bgp
