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

#include "neteng/fboss/bgp/cpp/lib/coro/test/MPMCQueueBenchmarkFixture.h"

#include <folly/init/Init.h>

#include <folly/coro/BlockingWait.h>
#include <folly/coro/UnboundedQueue.h>
#include <folly/logging/xlog.h>

using namespace facebook::nettools::bgplib;

using QueueItemType = unsigned int;

folly::coro::Task<void> BM_UnboundedQueue_MPMC_Async(
    uint32_t iters,
    uint32_t nProducers,
    uint32_t nConsumers,
    uint32_t itemsPerProducer) {
  folly::BenchmarkSuspender suspender;

  auto enqueueOp = [](auto& queue,
                      QueueItemType&& item) -> folly::coro::Task<void> {
    queue.enqueue(std::move(item));
    co_return;
  };

  auto dequeueOp = [](auto& queue) -> folly::coro::Task<void> {
    auto ret = co_await queue.dequeue();
    XCHECK(ret >= 0) << "Dequeue Error";
  };

  for (int i = 0; i < iters; ++i) {
    folly::coro::UnboundedQueue<QueueItemType, false, false> queue;
    MPMCQueueBenchmarkFixture<
        folly::coro::UnboundedQueue<QueueItemType, false, false>,
        QueueItemType>
        fixture(nProducers, nConsumers, itemsPerProducer);

    suspender.dismiss();

    co_await fixture.runBenchmark(queue, enqueueOp, dequeueOp);

    suspender.rehire();
  }
}

void BM_UnboundedQueue_MPMC(
    uint32_t iters,
    uint32_t nProducers,
    uint32_t nConsumers,
    uint32_t itemsPerProducer) {
  folly::coro::blockingWait(BM_UnboundedQueue_MPMC_Async(
      iters, nProducers, nConsumers, itemsPerProducer));
}

BENCHMARK_NAMED_PARAM(
    BM_UnboundedQueue_MPMC,
    1_to_1_1000_uqueue_atomic,
    1,
    1,
    1000);
BENCHMARK_DRAW_LINE();
BENCHMARK_NAMED_PARAM(
    BM_UnboundedQueue_MPMC,
    1_to_2_1000_uqueue_atomic,
    1,
    2,
    1000);
BENCHMARK_DRAW_LINE();
BENCHMARK_NAMED_PARAM(
    BM_UnboundedQueue_MPMC,
    2_to_2_1000_uqueue_atomic,
    2,
    2,
    1000);
BENCHMARK_DRAW_LINE();
BENCHMARK_NAMED_PARAM(
    BM_UnboundedQueue_MPMC,
    4_to_4_1000_uqueue_atomic,
    4,
    4,
    1000);
BENCHMARK_DRAW_LINE();
BENCHMARK_NAMED_PARAM(
    BM_UnboundedQueue_MPMC,
    16_to_16_1000_uqueue_atomic,
    16,
    16,
    100);

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
