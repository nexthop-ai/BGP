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

#include <array>
#include <cstdio>
#include <cstring>
#include <exception>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <folly/Benchmark.h>
#include <folly/init/Init.h>
#include <folly/memory/MallctlHelper.h>
#include <folly/memory/Malloc.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include "neteng/fboss/bgp/cpp/tests/gen-cpp2/canonical_pool_benchmark_types.h"

namespace facebook::bgp::benchmark {
namespace {

constexpr size_t kPayloadBytes = 32;
constexpr size_t kLookupsPerIteration = 1024;

struct PoolShape {
  const char* name;
  size_t prefixes;
  size_t pathsPerPrefix;
  size_t nexthops;
  std::array<size_t, 6> cardinalities;
};

constexpr std::array<PoolShape, 2> kShapes = {{
    {"standardFpf", 34320, 120, 120, {17160, 17160, 1, 0, 0, 120}},
    {"xxlFpf", 241920, 1, 960, {960, 960, 1, 0, 0, 960}},
}};

PoolValue makeValue(size_t id) {
  PoolValue value;
  std::string payload(kPayloadBytes, '\0');
  std::memcpy(payload.data(), &id, sizeof(id));
  value.payload() = std::move(payload);
  return value;
}

ListPools makeListPools(const PoolShape& shape) {
  ListPools pools;
  std::array<std::vector<PoolValue>*, 6> fields = {
      &*pools.wholePaths(),
      &*pools.asPaths(),
      &*pools.communities(),
      &*pools.extCommunities(),
      &*pools.clusterLists(),
      &*pools.peers(),
  };
  for (size_t fieldIdx = 0; fieldIdx < fields.size(); ++fieldIdx) {
    auto* field = fields[fieldIdx];
    const auto cardinality = shape.cardinalities[fieldIdx];
    field->reserve(cardinality);
    for (size_t id = 0; id < cardinality; ++id) {
      field->push_back(makeValue(id));
    }
  }
  return pools;
}

MapPools makeMapPools(const PoolShape& shape) {
  MapPools pools;
  std::array<std::unordered_map<int64_t, PoolValue>*, 6> fields = {
      &*pools.wholePaths(),
      &*pools.asPaths(),
      &*pools.communities(),
      &*pools.extCommunities(),
      &*pools.clusterLists(),
      &*pools.peers(),
  };
  for (size_t fieldIdx = 0; fieldIdx < fields.size(); ++fieldIdx) {
    auto* field = fields[fieldIdx];
    const auto cardinality = shape.cardinalities[fieldIdx];
    field->reserve(cardinality);
    for (size_t id = 0; id < cardinality; ++id) {
      field->emplace(static_cast<int64_t>(id), makeValue(id));
    }
  }
  return pools;
}

template <typename Pools>
std::string serialize(const Pools& pools) {
  std::string out;
  apache::thrift::CompactSerializer::serialize(pools, &out);
  return out;
}

uint64_t currentThreadBytes() {
  uint64_t allocated = 0;
  uint64_t deallocated = 0;
  folly::mallctlRead("thread.allocated", &allocated);
  folly::mallctlRead("thread.deallocated", &deallocated);
  return allocated >= deallocated ? allocated - deallocated : 0;
}

template <typename BuildFn>
size_t retainedBytes(BuildFn&& build) {
  size_t result = 0;
  std::exception_ptr error;
  std::thread thread([&]() {
    try {
      const auto before = currentThreadBytes();
      auto pools = build();
      const auto after = currentThreadBytes();
      folly::doNotOptimizeAway(pools);
      result = static_cast<size_t>(after >= before ? after - before : 0);
    } catch (...) {
      error = std::current_exception();
    }
  });
  thread.join();
  if (error) {
    std::rethrow_exception(error);
  }
  return result;
}

struct Fixture {
  explicit Fixture(const PoolShape& shape)
      : lists(makeListPools(shape)), maps(makeMapPools(shape)) {}
  ListPools lists;
  MapPools maps;
};

std::map<size_t, Fixture>& fixtures() {
  static std::map<size_t, Fixture> values;
  return values;
}

void BM_ConstructLists(uint32_t iters, size_t shapeIdx) {
  while (iters--) {
    auto pools = makeListPools(kShapes.at(shapeIdx));
    folly::doNotOptimizeAway(pools);
  }
}

void BM_ConstructMaps(uint32_t iters, size_t shapeIdx) {
  while (iters--) {
    auto pools = makeMapPools(kShapes.at(shapeIdx));
    folly::doNotOptimizeAway(pools);
  }
}

void BM_SerializeLists(uint32_t iters, size_t shapeIdx) {
  const auto& pools = fixtures().at(shapeIdx).lists;
  while (iters--) {
    auto out = serialize(pools);
    folly::doNotOptimizeAway(out);
  }
}

void BM_SerializeMaps(uint32_t iters, size_t shapeIdx) {
  const auto& pools = fixtures().at(shapeIdx).maps;
  while (iters--) {
    auto out = serialize(pools);
    folly::doNotOptimizeAway(out);
  }
}

void BM_RandomLookupLists(uint32_t iters, size_t shapeIdx) {
  const auto& pool = *fixtures().at(shapeIdx).lists.wholePaths();
  const auto cardinality = pool.size();
  while (iters--) {
    size_t id = 17 % cardinality;
    size_t checksum = 0;
    for (size_t i = 0; i < kLookupsPerIteration; ++i) {
      id = (id * 48271 + 1) % cardinality;
      checksum += pool[id].payload()->size();
    }
    folly::doNotOptimizeAway(checksum);
  }
}

void BM_RandomLookupMaps(uint32_t iters, size_t shapeIdx) {
  const auto& pool = *fixtures().at(shapeIdx).maps.wholePaths();
  const auto cardinality = pool.size();
  while (iters--) {
    size_t id = 17 % cardinality;
    size_t checksum = 0;
    for (size_t i = 0; i < kLookupsPerIteration; ++i) {
      id = (id * 48271 + 1) % cardinality;
      checksum += pool.at(static_cast<int64_t>(id)).payload()->size();
    }
    folly::doNotOptimizeAway(checksum);
  }
}

void BM_SequentialReadLists(uint32_t iters, size_t shapeIdx) {
  const auto& pool = *fixtures().at(shapeIdx).lists.wholePaths();
  while (iters--) {
    size_t checksum = 0;
    for (size_t id = 0; id < pool.size(); ++id) {
      checksum += id + pool[id].payload()->size();
    }
    folly::doNotOptimizeAway(checksum);
  }
}

void BM_SequentialReadMaps(uint32_t iters, size_t shapeIdx) {
  const auto& pool = *fixtures().at(shapeIdx).maps.wholePaths();
  while (iters--) {
    size_t checksum = 0;
    for (const auto& [id, value] : pool) {
      checksum += static_cast<size_t>(id) + value.payload()->size();
    }
    folly::doNotOptimizeAway(checksum);
  }
}

#define REGISTER_PAIR(name, label, shapeIdx)                \
  BENCHMARK_NAMED_PARAM(BM_##name##Lists, label, shapeIdx); \
  BENCHMARK_RELATIVE_NAMED_PARAM(BM_##name##Maps, label, shapeIdx)

REGISTER_PAIR(Construct, StandardFpf, 0);
REGISTER_PAIR(Construct, XxlFpf, 1);
BENCHMARK_DRAW_LINE();
REGISTER_PAIR(Serialize, StandardFpf, 0);
REGISTER_PAIR(Serialize, XxlFpf, 1);
BENCHMARK_DRAW_LINE();
REGISTER_PAIR(RandomLookup, StandardFpf, 0);
REGISTER_PAIR(RandomLookup, XxlFpf, 1);
BENCHMARK_DRAW_LINE();
REGISTER_PAIR(SequentialRead, StandardFpf, 0);
REGISTER_PAIR(SequentialRead, XxlFpf, 1);

#undef REGISTER_PAIR

void reportSizes(size_t shapeIdx) {
  const auto& shape = kShapes.at(shapeIdx);
  const auto& fixture = fixtures().at(shapeIdx);
  const auto listWire = serialize(fixture.lists).size();
  const auto mapWire = serialize(fixture.maps).size();
  const auto listMemory = retainedBytes([&]() { return makeListPools(shape); });
  const auto mapMemory = retainedBytes([&]() { return makeMapPools(shape); });
  std::fprintf(
      stderr,
      "[pool-representation] shape=%s prefixes=%zu pathsPerPrefix=%zu "
      "totalPaths=%zu nexthops=%zu "
      "whole=%zu asPath=%zu communities=%zu extCommunities=%zu "
      "clusterLists=%zu peers=%zu payload=%zu "
      "listWire=%zu mapWire=%zu wireRatio=%.3fx "
      "listMemory=%zu mapMemory=%zu memoryRatio=%.3fx\n",
      shape.name,
      shape.prefixes,
      shape.pathsPerPrefix,
      shape.prefixes * shape.pathsPerPrefix,
      shape.nexthops,
      shape.cardinalities[0],
      shape.cardinalities[1],
      shape.cardinalities[2],
      shape.cardinalities[3],
      shape.cardinalities[4],
      shape.cardinalities[5],
      kPayloadBytes,
      listWire,
      mapWire,
      static_cast<double>(mapWire) / listWire,
      listMemory,
      mapMemory,
      static_cast<double>(mapMemory) / listMemory);
}

} // namespace
} // namespace facebook::bgp::benchmark

int main(int argc, char** argv) {
  const folly::Init init(&argc, &argv);
  for (size_t shapeIdx = 0; shapeIdx < facebook::bgp::benchmark::kShapes.size();
       ++shapeIdx) {
    facebook::bgp::benchmark::fixtures().try_emplace(
        shapeIdx, facebook::bgp::benchmark::kShapes.at(shapeIdx));
    facebook::bgp::benchmark::reportSizes(shapeIdx);
  }
  folly::runBenchmarks();
  return 0;
}
