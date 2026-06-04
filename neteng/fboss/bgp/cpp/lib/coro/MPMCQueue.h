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

#include <folly/coro/Mutex.h>
#include <folly/coro/Task.h>
#include <folly/fibers/Semaphore.h>
#include <folly/logging/xlog.h>

namespace facebook::bgp::coro {

/**
 * An unbounded version of Multi Producer Multi Consumer queue
 * All public apis are thread safe
 */
template <typename T>
class MPMCQueue {
 public:
  MPMCQueue() {}

  template <typename U = T>
  void push(U&& val) noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.emplace_back(std::forward<T>(val));
    }
    sem_.signal();
  }

  // cancellable async pop
  folly::coro::Task<T> pop() {
    folly::Try<void> result = co_await folly::coro::co_awaitTry(sem_.co_wait());
    if (result.hasException()) {
      // this happens when it got cancelled
      co_yield folly::coro::co_error(std::move(result).exception());
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto val = std::move(queue_.front());
    queue_.pop_front();
    co_return val;
  }

  bool empty() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
  }

  size_t size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

 private:
  // delete copy constructors
  MPMCQueue(MPMCQueue const&) = delete;
  MPMCQueue& operator=(MPMCQueue const&) = delete;

  mutable std::mutex mutex_;
  std::deque<T> queue_;
  folly::fibers::Semaphore sem_{0};
};

} // namespace facebook::bgp::coro
