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

#include <cstdint>

#include "neteng/fboss/bgp/cpp/stats/Stats.h"

namespace facebook::bgp {

/**
 * Single authoritative in-memory aggregate of RIB-wide counts.
 *
 * Owned by RibBase and mutated only on the RIB EventBase, so it needs no
 * locking. Each mutator updates the in-memory field AND mirrors the matching
 * fb303 ODS counter (via RibStats) in one place, so callers no longer sprinkle
 * RibStats increment/decrement calls across the RIB and existing dashboards /
 * HealthValidator keep observing identical fb303 values.
 *
 * This is the single home for RIB count maintenance: future summary dimensions
 * (per-AFI totals, per-prefix-length histogram, best-path source breakdown,
 * ...) are added here rather than as parallel ad-hoc counters.
 */
class RibCounters {
 public:
  /** A prefix was inserted into the Loc-RIB. */
  void onPrefixAdded() {
    ++totalPrefixes_;
    RibStats::incrRibPrefixCount();
  }

  /** A prefix was removed from the Loc-RIB. */
  void onPrefixRemoved() {
    --totalPrefixes_;
    RibStats::decrRibPrefixCount();
  }

  /** The set of locally-originated routes was (re)computed to `count`. */
  void setOriginatedRoutes(uint64_t count) {
    originatedRoutes_ = count;
    RibStats::setOriginatedRoutesSize(count);
  }

  /** A tracked nexthop became unresolvable. */
  void onUnresolvableNexthopAdded() {
    ++unresolvableNexthops_;
    RibStats::incrUnresolvableNexthopsCount();
  }

  /** A previously-unresolvable nexthop became resolvable (or was removed). */
  void onUnresolvableNexthopRemoved() {
    --unresolvableNexthops_;
    RibStats::decrUnresolvableNexthopsCount();
  }

  /**
   * Zero the in-memory counts. Called on the RIB EventBase when the Loc-RIB is
   * bulk-cleared during controlled shutdown. The fb303 mirrors are
   * intentionally left untouched here: they are re-initialized on device bootup
   * (see RibStats::initCounters), matching the existing convention for RIB ODS
   * counters that are not adjusted on the bulk-clear path.
   */
  void reset() {
    totalPrefixes_ = 0;
    originatedRoutes_ = 0;
    unresolvableNexthops_ = 0;
  }

  uint64_t totalPrefixes() const {
    return totalPrefixes_;
  }
  uint64_t originatedRoutes() const {
    return originatedRoutes_;
  }
  uint64_t unresolvableNexthops() const {
    return unresolvableNexthops_;
  }

 private:
  uint64_t totalPrefixes_{0};
  uint64_t originatedRoutes_{0};
  uint64_t unresolvableNexthops_{0};
};

} // namespace facebook::bgp
