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

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/adjrib/AdjRibStructs.h"
#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/common/Utils.h"
#include "neteng/fboss/bgp/cpp/stats/Stats.h"

namespace facebook::bgp {

using nettools::bgplib::DeDuplicatedBgpPath;

extern PostPolicyResultCacheT postPolicyResultCache_;

// Adjacency Rib entry
struct AdjRibEntry {
  // Bitmap flags for AdjRibEntry state
  // Bit 0: isStale - Marked for session down with GR
  // Bits 1-7: Reserved for future use
  uint8_t flags_{0};

  // Flag bit positions
  static constexpr uint8_t kStaleBit = 0;
  static constexpr uint8_t kNexthopSetByPolicyBit = 1;

  bool isStale() const {
    return (flags_ & (1 << kStaleBit)) != 0;
  }

  void setStale(bool stale) {
    if (stale) {
      flags_ |= (1 << kStaleBit);
    } else {
      flags_ &= ~(1 << kStaleBit);
    }
  }

  /**
   * Flag for CLI display only. Indicates the egress policy's SetNexthop
   * action fired for this entry. Used by AdjRibShow to display the
   * correct nexthop instead of blindly applying nexthop-self.
   *
   * NOTE: This flag is intentionally duplicated with
   * BgpPathWithAfi::isNexthopSetByPolicy. Packing list and
   * advertisement use the BgpPathWithAfi key flag (which is captured
   * at insertion time and is immune to backpressure-induced races).
   * This AdjRibEntry flag always reflects the latest policy result
   * and is suitable for CLI display, which should show current state.
   * See BgpPathWithAfi comment in AdjRibStructs.h for the full
   * rationale on why both flags are needed.
   */
  bool isNexthopSetByPolicy() const {
    return (flags_ & (1 << kNexthopSetByPolicyBit)) != 0;
  }

  void setNexthopSetByPolicy(bool value) {
    if (value) {
      flags_ |= (1 << kNexthopSetByPolicyBit);
    } else {
      flags_ &= ~(1 << kNexthopSetByPolicyBit);
    }
  }

  explicit AdjRibEntry(uint32_t pathId) : pathId_(pathId) {}

  void setPreIn(const std::shared_ptr<const BgpPath>& attrs) {
    prePolicyAttrs_ = attrs;
    // We only modify lastUpdateRcvdUsec_ when pre-in attributes have changed
    lastUpdateRcvdUsec_ = getCurrentTimeMicroSec();
  }

  void setPostAttr(const std::shared_ptr<const BgpPath>& attrs) {
    if (attrs) {
      // Dedup via DeDuplicatedBgpPath: identical BgpPath values share one ptr
      DeDuplicatedBgpPath deduped(std::const_pointer_cast<BgpPath>(attrs));
      postPolicyAttrs_ = deduped.getSharedPtr();
    } else {
      postPolicyAttrs_ = nullptr;
    }
  }

  void setPreOut(const std::shared_ptr<const BgpPath>& attrs) {
    if (prePolicyAttrs_) {
      prePolicyAttrs_->decOnAdjPreoutCount();
    }
    prePolicyAttrs_ = attrs;
    if (prePolicyAttrs_) {
      prePolicyAttrs_->incOnAdjPreoutCount();
    }
  }

  void setPostInPolicy(const std::string& policyName) {
    setPostPolicy(policyName);
  }

  void setPostOutPolicy(const std::string& policyName) {
    setPostPolicy(policyName);
  }

  const std::shared_ptr<const BgpPath>& getPreIn() const {
    return prePolicyAttrs_;
  }

  const std::shared_ptr<const BgpPath>& getPostAttr() const {
    return postPolicyAttrs_;
  }

  const std::shared_ptr<const BgpPath>& getPreOut() const {
    return prePolicyAttrs_;
  }

  uint32_t getPathId() {
    return pathId_;
  }

  /*
   * Compare post-policy attributes with another entry.
   * Returns true if both entries exist and have equivalent post-policy state.
   * Used during collapse to decide whether PL correction is needed.
   *
   * Note: Only compares postPolicyAttrs_. Prefix and pathId are already
   * matched by the tree key and path map key respectively before this
   * is called (see collapseLiteEntry/collapsePathEntry).
   */
  bool hasMatchingPostPolicyAttrs(const AdjRibEntry* other) const {
    return other && postPolicyAttrs_ == other->postPolicyAttrs_;
  }

  uint64_t getLastUpdateRcvdTime() const {
    return lastUpdateRcvdUsec_;
  }

  uint64_t getRibVersion() const {
    return ribVersion_;
  }

  void setRibVersion(uint64_t version) {
    ribVersion_ = version;
  }

  const PostPolicyResultT& getPostOutPolicy() const {
    return postPolicyResult_;
  }

  const PostPolicyResultT& getPostInPolicy() const {
    return postPolicyResult_;
  }

 private:
  std::shared_ptr<const BgpPath> prePolicyAttrs_;
  std::shared_ptr<const BgpPath> postPolicyAttrs_;
  PostPolicyResultT postPolicyResult_;

  uint32_t pathId_{kDefaultPathID};

  // last modified time in microseconds since epoch
  int64_t lastUpdateRcvdUsec_{0};

  // RIB version when this entry was last updated
  uint64_t ribVersion_{0};

  void clearPostPolicyResult() {
    // If there are no AdjRibEntry referencing the postPolicyResult_,
    // the base use_count() is 1 from existing in the set.
    // Hence we additionally subtract baseline use_count for pruning.
    if (postPolicyResult_ && (postPolicyResult_.use_count() - 1) == 1) {
      if (postPolicyResultCache_.erase(postPolicyResult_)) {
        RibStats::decrPostPolicyResultCacheCount();
      }
    }
  }

  void setPostPolicyResult(const PostPolicyResultT& term) {
    auto ret = postPolicyResultCache_.insert(term);
    if (ret.second) {
      RibStats::incrPostPolicyResultCacheCount();
    }
    postPolicyResult_ = *ret.first;
  }

  // Set the post policy result on AdjRibEntry after
  // inserting into cache. Prune unused policy terms.
  void setPostPolicy(const std::string& policyName) {
    clearPostPolicyResult();
    setPostPolicyResult(std::make_shared<const std::string>(policyName));
  }
};

} // namespace facebook::bgp
