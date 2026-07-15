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

#include "neteng/fboss/bgp/cpp/lib/coro/MPMCQueue.h"
#include "neteng/fboss/bgp/cpp/lib/coro/MergeQueue.h"

#include <folly/Benchmark.h>
#include <folly/init/Init.h>

using namespace facebook::bgp::coro;

/*
 * All pushes to one slot: every push after the first merges in place, so the
 * queue never grows beyond a single node. This is the empty->full thrash
 * workload the merge queue is meant to collapse.
 */
BENCHMARK(MergeQueue_SameSlotMerge, n) {
  MergeQueue<int> q;
  for (unsigned i = 0; i < n; ++i) {
    q.pushMerge(static_cast<int>(i), /*slot=*/0);
  }
  folly::doNotOptimizeAway(q.size());
}

/*
 * Baseline: plain FIFO push of the same items with no coalescing. The queue
 * grows to n nodes, and a consumer would have to process all n.
 */
BENCHMARK_RELATIVE(MPMCQueue_PlainPush, n) {
  MPMCQueue<int> q;
  for (unsigned i = 0; i < n; ++i) {
    q.push(static_cast<int>(i));
  }
  folly::doNotOptimizeAway(q.size());
}

BENCHMARK_DRAW_LINE();

/*
 * Distinct slots: every push appends (no merge) -- measures the append path
 * overhead relative to the plain queue.
 */
BENCHMARK(MergeQueue_DistinctSlotsAppend, n) {
  MergeQueue<int> q;
  for (unsigned i = 0; i < n; ++i) {
    q.pushMerge(static_cast<int>(i), /*slot=*/static_cast<int>(i));
  }
  folly::doNotOptimizeAway(q.size());
}

BENCHMARK_RELATIVE(MPMCQueue_DistinctAppend, n) {
  MPMCQueue<int> q;
  for (unsigned i = 0; i < n; ++i) {
    q.push(static_cast<int>(i));
  }
  folly::doNotOptimizeAway(q.size());
}

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
