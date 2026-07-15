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

#include <folly/container/F14Map.h>
#include <folly/coro/Task.h>
#include <folly/fibers/Semaphore.h>
#include <folly/logging/xlog.h>
#include <folly/synchronization/MicroSpinLock.h>

#include <list>
#include <optional>

namespace facebook::bgp::coro {

/**
 * An unbounded, coalescing (merge) queue for a single consumer.
 *
 * Every item is enqueued under a caller-supplied slot id. Pushing an item whose
 * slot already has a pending (not-yet-popped) item overwrites that item in
 * place, so the queue holds at most one pending item per slot -- always the
 * latest -- while preserving FIFO order relative to items in other slots.
 * Overwriting in place is correct only because a slotted item affects solely
 * its own slot, so its position relative to other slots is immaterial.
 *
 * `pushPurgeAll` is the whole-queue merge: it coalesces every pending item into
 * `val` (e.g. a "clear everything" message that supersedes all queued items).
 * Such an item affects every slot, so its FIFO position IS significant and it
 * cannot overwrite in place -- it drops all predecessors and lands at the tail,
 * ordered after everything it supersedes. Consecutive pushPurgeAll thus
 * collapse too: the second drops the first.
 *
 * Concurrency: designed for a single consumer (one `pop()` loop). Multiple
 * producers are safe -- all mutation is under a short-held spin lock. The
 * wakeup semaphore is an over-approximation of the queue depth: an in-place
 * merge adds no wakeup, and a purge can leave stale wakeups behind, so `pop()`
 * re-checks under the lock and waits again if it wakes to an empty queue.
 *
 * `pushMerge`/`pushPurgeAll` are noexcept: an allocation failure terminates the
 * process rather than propagating, matching bgpd's crash-on-OOM policy (and the
 * MPMCQueue this replaced).
 */
template <typename T>
class MergeQueue {
 public:
  MergeQueue() = default;
  ~MergeQueue() = default;

  // Non-copyable and non-movable: owns a folly::fibers::Semaphore and a
  // MicroSpinLock, neither of which is movable.
  MergeQueue(const MergeQueue&) = delete;
  MergeQueue& operator=(const MergeQueue&) = delete;
  MergeQueue(MergeQueue&&) = delete;
  MergeQueue& operator=(MergeQueue&&) = delete;

  /*
   * Enqueue `val` under `slot` (must be >= 0). If an item for `slot` is already
   * pending, overwrite it in place (no new wakeup); otherwise append and wake
   * the consumer. Returns true if it coalesced into an existing pending item,
   * false if it appended a new one -- lets callers count coalescing.
   */
  template <typename U = T>
  bool pushMerge(U&& val, int slot) noexcept {
    // slot must be non-negative; kNoSlot (-1) is reserved for pushPurgeAll and
    // a collision there would corrupt slotIters_ bookkeeping.
    XDCHECK_GE(slot, 0);
    bool added = false;
    {
      std::lock_guard<folly::MicroSpinLock> guard(lock_);
      auto it = slotIters_.find(slot);
      if (it != slotIters_.end()) {
        // Replace the pending node in place, preserving its FIFO position.
        // insert+erase (rather than assignment) so T need only be
        // move-constructible, not move-assignable (e.g. a variant with a const
        // member).
        auto newIt =
            queue_.insert(it->second, Node{slot, std::forward<U>(val)});
        queue_.erase(it->second);
        it->second = newIt;
      } else {
        queue_.push_back(Node{slot, std::forward<U>(val)});
        slotIters_.emplace(slot, std::prev(queue_.end()));
        added = true;
      }
    }
    if (added) {
      sem_.signal();
    }
    return !added;
  }

  /*
   * Whole-queue merge: coalesce every pending item into `val`, which lands at
   * the tail. Use when `val` supersedes everything already queued. Unlike a
   * slotted merge, a supersede-all item cannot overwrite in place -- its FIFO
   * position matters, so it must be ordered after all the items it drops.
   */
  template <typename U = T>
  void pushPurgeAll(U&& val) noexcept {
    {
      std::lock_guard<folly::MicroSpinLock> guard(lock_);
      queue_.clear();
      slotIters_.clear();
      queue_.push_back(Node{kNoSlot, std::forward<U>(val)});
    }
    sem_.signal();
  }

  // Cancellable async pop; returns items in FIFO order.
  folly::coro::Task<T> pop() {
    while (true) {
      folly::Try<void> result =
          co_await folly::coro::co_awaitTry(sem_.co_wait());
      if (result.hasException()) {
        // Cancelled.
        co_yield folly::coro::co_error(std::move(result).exception());
      }

      std::optional<T> val;
      {
        std::lock_guard<folly::MicroSpinLock> guard(lock_);
        if (!queue_.empty()) {
          Node& front = queue_.front();
          if (front.slot != kNoSlot) {
            auto it = slotIters_.find(front.slot);
            if (it != slotIters_.end() && it->second == queue_.begin()) {
              slotIters_.erase(it);
            }
          }
          val.emplace(std::move(front.val));
          queue_.pop_front();
        }
      }
      if (val.has_value()) {
        co_return std::move(*val);
      }
      // Woke on a stale signal left by a merge/purge; wait again.
    }
  }

  size_t size() const noexcept {
    std::lock_guard<folly::MicroSpinLock> guard(lock_);
    return queue_.size();
  }

  bool empty() const noexcept {
    std::lock_guard<folly::MicroSpinLock> guard(lock_);
    return queue_.empty();
  }

 private:
  // Reserved slot for a pushPurgeAll item; never produced by a pushMerge caller
  // (real slots are >= 0), so a later merge can never address or overwrite a
  // queued supersede-all item.
  static constexpr int kNoSlot = -1;

  struct Node {
    int slot;
    T val;
  };

  mutable folly::MicroSpinLock lock_{};
  std::list<Node> queue_;
  folly::F14FastMap<int, typename std::list<Node>::iterator> slotIters_;
  folly::fibers::Semaphore sem_{0};
};

} // namespace facebook::bgp::coro
