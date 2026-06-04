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

/*
 * Benchmark for RouteAttributePolicy Cache Preservation
 *
 * Measures performance improvement from cache preservation during policy
 * updates. Compares:
 *   - BEFORE: Full re-evaluation of all ribEntries
 *   - AFTER: Selective re-evaluation of affected prefixes only
 */

#include <folly/Benchmark.h>
#include <folly/init/Init.h>

#include <neteng/fboss/bgp/cpp/rib/RibPolicy.h>
#include <neteng/fboss/bgp/cpp/tests/RibPolicyUtils.h>

using namespace facebook::bgp;

namespace {

// Test constants
constexpr int64_t kWeight = 100;
constexpr int64_t kNewWeight = 200;
constexpr size_t kNumStatements = 41; // Matches real CTE policy statement count

// Generate prefixes for testing
std::vector<folly::CIDRNetwork> generatePrefixes(size_t count) {
  std::vector<folly::CIDRNetwork> prefixes;
  prefixes.reserve(count);
  for (uint32_t i = 0; i < static_cast<uint32_t>(count); ++i) {
    prefixes.emplace_back(folly::IPAddress::fromLong(0x0A000000 + i), 24);
  }
  return prefixes;
}

// Create a policy with specified number of statements, each covering a portion
// of prefixes
rib_policy::TRibPolicy createPolicy(
    const std::vector<folly::CIDRNetwork>& prefixes,
    size_t numStatements,
    int64_t weight) {
  auto now = std::chrono::seconds(std::time(nullptr));
  auto timestamp = now.count() + 3600; // 1 hour from now

  size_t prefixesPerStmt = prefixes.size() / numStatements;

  // Create first statement
  std::vector<folly::CIDRNetwork> firstStmtPrefixes(
      prefixes.begin(), prefixes.begin() + prefixesPerStmt);
  auto tPolicy =
      createTRibPolicyLbw(firstStmtPrefixes, weight, "stmt0", timestamp);

  // Add remaining statements
  for (size_t s = 1; s < numStatements; ++s) {
    size_t startIdx = s * prefixesPerStmt;
    size_t endIdx = std::min((s + 1) * prefixesPerStmt, prefixes.size());
    std::vector<folly::CIDRNetwork> stmtPrefixes(
        prefixes.begin() + startIdx, prefixes.begin() + endIdx);

    tPolicy.route_attribute_policy()->statements()->emplace(
        "stmt" + std::to_string(s),
        createTRouteAttributeStatementLbw(stmtPrefixes, weight, timestamp));
  }

  return tPolicy;
}

} // namespace

/**
 * Benchmark: Cache hit for overwriteRouteAttributes
 *
 * Measures time to evaluate routes when cache is warm (hit) vs cold (miss).
 */
void BM_OverwriteRouteAttributes_CacheHit(uint32_t iters, size_t numPrefixes) {
  auto suspender = folly::BenchmarkSuspender();

  auto prefixes = generatePrefixes(numPrefixes);
  auto tPolicy = createPolicy(prefixes, kNumStatements, kWeight);
  RibPolicy policy{tPolicy};

  // Create RIB entries
  std::vector<RibEntry> ribEntries;
  ribEntries.reserve(numPrefixes);
  for (const auto& prefix : prefixes) {
    ribEntries.emplace_back(prefix);
  }

  // Warm up cache with first pass (not timed)
  RouteAttributePolicy::RibChange warmupChange;
  for (auto& entry : ribEntries) {
    policy.getRouteAttributePolicy()->overwriteRouteAttributes(
        entry, warmupChange);
  }

  suspender.dismiss(); // START timing

  while (iters--) {
    RouteAttributePolicy::RibChange ribChange;
    for (auto& entry : ribEntries) {
      policy.getRouteAttributePolicy()->overwriteRouteAttributes(
          entry, ribChange);
    }
  }

  suspender.rehire(); // STOP timing
}

/**
 * Benchmark: Cache miss for overwriteRouteAttributes
 *
 * Measures time to evaluate routes with empty cache (full statement iteration).
 */
void BM_OverwriteRouteAttributes_CacheMiss(uint32_t iters, size_t numPrefixes) {
  auto suspender = folly::BenchmarkSuspender();

  auto prefixes = generatePrefixes(numPrefixes);

  suspender.dismiss(); // START timing

  while (iters--) {
    // Create fresh policy each iteration (empty cache)
    auto tPolicy = createPolicy(prefixes, kNumStatements, kWeight);
    RibPolicy policy{tPolicy};

    // Create RIB entries
    std::vector<RibEntry> ribEntries;
    ribEntries.reserve(numPrefixes);
    for (const auto& prefix : prefixes) {
      ribEntries.emplace_back(prefix);
    }

    RouteAttributePolicy::RibChange ribChange;
    for (auto& entry : ribEntries) {
      policy.getRouteAttributePolicy()->overwriteRouteAttributes(
          entry, ribChange);
    }
  }

  suspender.rehire(); // STOP timing
}

/**
 * Benchmark: Cache migration (moveCache)
 *
 * Measures time to migrate cache from old policy to new policy.
 */
void BM_CacheMigration_MoveCache(uint32_t iters, size_t numPrefixes) {
  auto suspender = folly::BenchmarkSuspender();

  auto prefixes = generatePrefixes(numPrefixes);
  auto tPolicy = createPolicy(prefixes, kNumStatements, kWeight);

  suspender.dismiss(); // START timing

  while (iters--) {
    RibPolicy oldPolicy{tPolicy};

    // Warm up old policy cache
    std::vector<RibEntry> ribEntries;
    ribEntries.reserve(numPrefixes);
    for (const auto& prefix : prefixes) {
      ribEntries.emplace_back(prefix);
    }
    RouteAttributePolicy::RibChange warmupChange;
    for (auto& entry : ribEntries) {
      oldPolicy.getRouteAttributePolicy()->overwriteRouteAttributes(
          entry, warmupChange);
    }

    // Create new policy and move cache
    auto tNewPolicy = createPolicy(prefixes, kNumStatements, kNewWeight);
    RibPolicy newPolicy{tNewPolicy};

    newPolicy.getRouteAttributePolicy()->moveCache(
        *oldPolicy.getRouteAttributePolicy());
  }

  suspender.rehire(); // STOP timing
}

/**
 * Benchmark: Selective re-evaluation (WITH cache preservation)
 *
 * Simulates policy update with cache preservation - only affected prefixes
 * are re-evaluated.
 */
void BM_PolicyUpdate_WithCachePreservation(
    uint32_t iters,
    size_t numPrefixes,
    size_t affectedPercent) {
  auto suspender = folly::BenchmarkSuspender();

  auto prefixes = generatePrefixes(numPrefixes);
  auto tOldPolicy = createPolicy(prefixes, kNumStatements, kWeight);
  RibPolicy oldPolicy{tOldPolicy};

  // Warm up old policy cache
  std::vector<RibEntry> ribEntries;
  ribEntries.reserve(numPrefixes);
  for (const auto& prefix : prefixes) {
    ribEntries.emplace_back(prefix);
  }
  RouteAttributePolicy::RibChange warmupChange;
  for (auto& entry : ribEntries) {
    oldPolicy.getRouteAttributePolicy()->overwriteRouteAttributes(
        entry, warmupChange);
  }

  // Calculate affected prefixes (simulate stmt1 changed)
  size_t affectedCount = (numPrefixes * affectedPercent) / 100;

  suspender.dismiss(); // START timing

  while (iters--) {
    // Create new policy
    auto tNewPolicy = createPolicy(prefixes, kNumStatements, kNewWeight);
    RibPolicy newPolicy{tNewPolicy};

    // Move cache (cache preservation)
    newPolicy.getRouteAttributePolicy()->moveCache(
        *oldPolicy.getRouteAttributePolicy());

    // Re-evaluate only affected prefixes (selective)
    RouteAttributePolicy::RibChange ribChange;
    for (size_t i = 0; i < affectedCount; ++i) {
      newPolicy.getRouteAttributePolicy()->overwriteRouteAttributes(
          ribEntries[i], ribChange);
    }
  }

  suspender.rehire(); // STOP timing
}

/**
 * Benchmark: Full re-evaluation (WITHOUT cache preservation)
 *
 * Simulates policy update without cache preservation - all prefixes
 * are re-evaluated (baseline for comparison).
 */
void BM_PolicyUpdate_WithoutCachePreservation(
    uint32_t iters,
    size_t numPrefixes) {
  auto suspender = folly::BenchmarkSuspender();

  auto prefixes = generatePrefixes(numPrefixes);

  // Create RIB entries
  std::vector<RibEntry> ribEntries;
  ribEntries.reserve(numPrefixes);
  for (const auto& prefix : prefixes) {
    ribEntries.emplace_back(prefix);
  }

  suspender.dismiss(); // START timing

  while (iters--) {
    // Create new policy (empty cache - no preservation)
    auto tNewPolicy = createPolicy(prefixes, kNumStatements, kNewWeight);
    RibPolicy newPolicy{tNewPolicy};

    // Re-evaluate ALL prefixes (full sync - no cache)
    RouteAttributePolicy::RibChange ribChange;
    for (auto& entry : ribEntries) {
      newPolicy.getRouteAttributePolicy()->overwriteRouteAttributes(
          entry, ribChange);
    }
  }

  suspender.rehire(); // STOP timing
}

// ============================================================================
// Benchmark registrations
// ============================================================================

// Cache hit vs miss comparison (shows value of caching)
BENCHMARK_NAMED_PARAM(BM_OverwriteRouteAttributes_CacheHit, 10k, 10000);
BENCHMARK_NAMED_PARAM(BM_OverwriteRouteAttributes_CacheMiss, 10k, 10000);
BENCHMARK_DRAW_LINE();

BENCHMARK_NAMED_PARAM(BM_OverwriteRouteAttributes_CacheHit, 100k, 100000);
BENCHMARK_NAMED_PARAM(BM_OverwriteRouteAttributes_CacheMiss, 100k, 100000);
BENCHMARK_DRAW_LINE();

// Cache migration performance
BENCHMARK_NAMED_PARAM(BM_CacheMigration_MoveCache, 10k, 10000);
BENCHMARK_NAMED_PARAM(BM_CacheMigration_MoveCache, 100k, 100000);
BENCHMARK_NAMED_PARAM(BM_CacheMigration_MoveCache, 500k, 500000);
BENCHMARK_DRAW_LINE();

// ============================================================================
// KEY COMPARISON: With vs Without Cache Preservation
// ============================================================================

// 100K prefixes - compare full re-eval vs selective (8% affected)
BENCHMARK_NAMED_PARAM(
    BM_PolicyUpdate_WithoutCachePreservation,
    100k_BASELINE,
    100000);
BENCHMARK_NAMED_PARAM(
    BM_PolicyUpdate_WithCachePreservation,
    100k_8pct_affected,
    100000,
    8);
BENCHMARK_DRAW_LINE();

// 500K prefixes - compare full re-eval vs selective (8% affected)
BENCHMARK_NAMED_PARAM(
    BM_PolicyUpdate_WithoutCachePreservation,
    500k_BASELINE,
    500000);
BENCHMARK_NAMED_PARAM(
    BM_PolicyUpdate_WithCachePreservation,
    500k_8pct_affected,
    500000,
    8);
BENCHMARK_DRAW_LINE();

// Different affected percentages (shows scaling)
BENCHMARK_NAMED_PARAM(
    BM_PolicyUpdate_WithCachePreservation,
    100k_1pct_affected,
    100000,
    1);
BENCHMARK_NAMED_PARAM(
    BM_PolicyUpdate_WithCachePreservation,
    100k_5pct_affected,
    100000,
    5);
BENCHMARK_NAMED_PARAM(
    BM_PolicyUpdate_WithCachePreservation,
    100k_10pct_affected,
    100000,
    10);
BENCHMARK_NAMED_PARAM(
    BM_PolicyUpdate_WithCachePreservation,
    100k_25pct_affected,
    100000,
    25);
BENCHMARK_NAMED_PARAM(
    BM_PolicyUpdate_WithCachePreservation,
    100k_50pct_affected,
    100000,
    50);
BENCHMARK_NAMED_PARAM(
    BM_PolicyUpdate_WithCachePreservation,
    100k_100pct_affected,
    100000,
    100);

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
