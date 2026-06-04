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

#include <type_traits>

#include "neteng/fboss/bgp/cpp/lib/coro/MPMCQueue.h"
#include "neteng/fboss/bgp/cpp/lib/coro/MPMCWatermarkQueue.h"
#include "neteng/fboss/bgp/cpp/lib/fibers/Queue.h"
#include "neteng/fboss/bgp/cpp/watchdog/MonitorableTrace.h"

namespace facebook::bgp {
class MonitoredQueueBase : public MonitorableQueueTrace {
 public:
  MonitoredQueueBase() = default;
  virtual ~MonitoredQueueBase() = default;

  virtual int size() noexcept = 0;
};

template <class C>
class MonitoredQueue : public C, public MonitoredQueueBase {
 public:
  // use all explicit constructors of C
  template <typename... Args>
  explicit MonitoredQueue(Args&&... args) : C(std::forward<Args>(args)...) {}

  template <
      typename T = C,
      typename = std::enable_if_t<std::is_assignable<T&, const T&>::value>>
  MonitoredQueue& operator=(const MonitoredQueue& other) noexcept {
    C::operator=(other);
    return *this;
  }

  int size() noexcept override {
    return (int)C::size();
  }
};

template <class C>
using MonitoredMPMCQueue = MonitoredQueue<bgp::coro::MPMCQueue<C>>;

template <class C>
using MonitoredRQueue = MonitoredQueue<nettools::bgplib::RQueue<C>>;

template <class C>
using MonitoredWQueue = MonitoredQueue<nettools::bgplib::WQueue<C>>;

template <class C>
using MonitoredRWQueue = MonitoredQueue<nettools::bgplib::RWQueue<C>>;

template <class C>
using MonitoredMPMCWQueue =
    MonitoredQueue<nettools::bgplib::MPMCWatermarkQueue<C>>;

} // namespace facebook::bgp
