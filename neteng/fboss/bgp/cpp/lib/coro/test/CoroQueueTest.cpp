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

#include "neteng/fboss/bgp/cpp/lib/coro/CoroQueue.h"
#include <variant>

#include <folly/Overload.h>
#include <folly/executors/ManualExecutor.h>
#include <folly/logging/xlog.h>
#include <folly/portability/GTest.h>

using namespace folly;
using namespace facebook::nettools::bgplib;

template <typename T>
coro::Task<void> consumer(CoroQueue<T>& q, int count) {
  XLOG(DBG1, "Consumer: queue is ready");
  for (auto i = 0; i < count; i++) {
    auto ret = co_await q.wait_and_pop();
    if (ret.hasError()) {
      break;
    }
  }
}

TEST(Queue, SingleProduceSingleConsumer) {
  CoroQueue<int> queue;
  ManualExecutor executor;
  constexpr int kSize = 100;

  auto producer = [](CoroQueue<int>& queue) -> folly::coro::Task<void> {
    for (int i = 0; i < kSize; i++) {
      co_await queue.push(i);
      XLOGF(DBG1, "pushed: {}", i);
    }
    co_await queue.close();
  };

  EXPECT_EQ(0, queue.getRawQueueUnsafe().size());

  auto futConsumer = co_withExecutor(&executor, consumer(queue, kSize)).start();
  auto futProducer = co_withExecutor(&executor, producer(queue)).start();

  executor.drain();

  EXPECT_TRUE(futConsumer.isReady());
  EXPECT_TRUE(futProducer.isReady());
  EXPECT_EQ(0, queue.getRawQueueUnsafe().size());
}

TEST(Queue, LockStepProducerConsumer) {
  CoroQueue<int> left_to_right;
  CoroQueue<int> right_to_left;
  ManualExecutor executor;
  constexpr int kSize = 100;

  auto leftAgent =
      [](CoroQueue<int>& left_to_right,
         CoroQueue<int>& right_to_left) -> folly::coro::Task<void> {
    for (int i = 0; i < kSize; i++) {
      co_await left_to_right.push(i);
      XLOGF(DBG1, "left, pushed: {}", i);
      auto val = co_await right_to_left.wait_and_pop();
      XLOGF(DBG1, "left, popped: {}", val.value());
    }
    co_await left_to_right.close();
  };

  auto rightAgent =
      [](CoroQueue<int>& left_to_right,
         CoroQueue<int>& right_to_left) -> folly::coro::Task<void> {
    for (int i = 0; i < kSize; i++) {
      co_await right_to_left.push(i);
      XLOGF(DBG1, "right, pushed: {}", i);
      auto val = co_await left_to_right.wait_and_pop();
      XLOGF(DBG1, "right, popped: {}", val.value());
    }
    co_await right_to_left.close();
  };

  EXPECT_EQ(0, left_to_right.getRawQueueUnsafe().size());
  EXPECT_EQ(0, right_to_left.getRawQueueUnsafe().size());

  auto futConsumer =
      co_withExecutor(&executor, leftAgent(left_to_right, right_to_left))
          .start();
  auto futProducer =
      co_withExecutor(&executor, rightAgent(left_to_right, right_to_left))
          .start();

  executor.drain();

  EXPECT_TRUE(futConsumer.isReady());
  EXPECT_TRUE(futProducer.isReady());
  EXPECT_EQ(0, left_to_right.getRawQueueUnsafe().size());
  EXPECT_EQ(0, right_to_left.getRawQueueUnsafe().size());
}

TEST(Queue, MultiProduceMultiConsumer) {
  CoroQueue<int> queue;
  ManualExecutor executor;
  constexpr int kSize = 100;
  constexpr int kNumProducers = 16;

  auto producer = [](CoroQueue<int>& queue) -> folly::coro::Task<void> {
    for (int i = 0; i < kSize; i++) {
      co_await queue.push(i);
      XLOGF(DBG1, "pushed: {}", i);
    }
    co_await queue.close();
  };

  EXPECT_EQ(0, queue.getRawQueueUnsafe().size());

  std::vector<folly::SemiFuture<folly::Unit>> futConsumers;
  std::vector<folly::SemiFuture<folly::Unit>> futProducers;

  for (int i = 0; i < kNumProducers; i++) {
    futConsumers.push_back(
        co_withExecutor(&executor, consumer(queue, kSize)).start());
    futProducers.push_back(co_withExecutor(&executor, producer(queue)).start());
  }

  executor.drain();

  for (int i = 0; i < kNumProducers; i++) {
    EXPECT_TRUE(futConsumers[i].isReady()) << " consumer " << i;
    EXPECT_TRUE(futProducers[i].isReady()) << " producer " << i;
    ;
  }

  EXPECT_EQ(0, queue.getRawQueueUnsafe().size());
}

template <typename Q, typename F>
coro::Task<void> templatized_producer(Q& q, F f, int count) {
  for (auto i = 0; i < count; i++) {
    co_await q.push(f(i));
  }
}

template <typename Q>
coro::Task<void> variant_consumer(Q& q) {
  while (true) {
    auto maybeVal = co_await q.wait_and_pop();
    if (maybeVal.hasError()) {
      XLOG(DBG1, "End of stream");
      break;
    }
    auto val = maybeVal.value();
    folly::variant_match(
        val,
        [](const int& val) { XLOGF(DBG1, "received int: {}", val); },
        [](const std::string& val) { XLOGF(DBG1, "received str: {}", val); },
        [](const auto& val __attribute__((unused))) {
          ADD_FAILURE() << "Unexpected type in the variant";
        });
  }
}

TEST(Queue, TwoQueueMerge) {
  CoroQueue<std::variant<int, std::string>> queue;
  ManualExecutor executor;
  constexpr int kSize = 100;

  auto producer = [&queue]() -> coro::Task<void> {
    co_await templatized_producer(queue, [](int i) -> int { return i; }, kSize);
    co_await templatized_producer(
        queue,
        [](int i) -> std::string { return fmt::format("{}", i); },
        kSize);
    co_await queue.close();
  };

  auto futProducer = co_withExecutor(&executor, producer()).start();
  auto futConsumer =
      co_withExecutor(&executor, variant_consumer(queue)).start();

  executor.drain();

  EXPECT_TRUE(futProducer.isReady());
  EXPECT_TRUE(futConsumer.isReady());
}
