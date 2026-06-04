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

// TODO: deprecate this class once ADD-PATH changes are completed

#include <folly/IPAddress.h>
#include <folly/container/F14Map.h>

namespace facebook::bgp {

class PathIdGenerator {
 public:
  explicit PathIdGenerator(bool sendAddPath) : sendAddPath_(sendAddPath) {}

  uint32_t getPathId(
      const folly::CIDRNetwork& prefix,
      const folly::IPAddress& nextHop);

 private:
  folly::F14NodeMap<
      folly::CIDRNetwork,
      folly::F14NodeMap<folly::IPAddress, uint32_t>>
      PathIdCache_;
  folly::F14NodeMap<folly::CIDRNetwork, uint32_t> pathIdOption_;
  bool sendAddPath_{false};
};
} // namespace facebook::bgp
