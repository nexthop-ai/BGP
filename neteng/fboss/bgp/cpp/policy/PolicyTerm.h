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

#include "configerator/structs/neteng/bgp_policy/thrift/gen-cpp2/bgp_policy_types.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyAction.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyAttributesMask.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyMatch.h"
#include "neteng/fboss/bgp/cpp/policy/base/PolicyTermBase.h"
#include "neteng/fboss/bgp/if/gen-cpp2/BgpStructs_types.h"

namespace facebook::bgp {

class PolicyTerm : public routing::PolicyTermBase<
                       BgpPath,
                       std::shared_ptr<BgpPolicyActionData>,
                       BgpPolicyMatchData> {
 public:
  explicit PolicyTerm(
      const bgp_policy::BgpPolicyTerm& term,
      PolicyAttributesMask& mask,
      const BgpGlobalConfig* config = nullptr);
  virtual ~PolicyTerm() = default;

  virtual void copyAttr(std::shared_ptr<BgpPath>& attr) override;

  std::optional<std::string> hasInvalidAttrsPostAction(
      const std::shared_ptr<BgpPath>& attr,
      const routing::PolicyEntryBase<
          BgpPath,
          std::shared_ptr<BgpPolicyActionData>,
          BgpPolicyMatchData>& entry,
      const std::unordered_set<folly::CIDRNetwork>& prefixesSet,
      const std::string& policyName) const noexcept override;
};

} // namespace facebook::bgp
