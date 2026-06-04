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

#include <memory>
#include <string>

#include <folly/IPAddress.h>

namespace facebook {
namespace rfe {
class ScubaData;
} // namespace rfe

namespace bgp {

class RouteFilterLogger {
 public:
  explicit RouteFilterLogger(
      const std::string& deviceName,
      const std::string& statementName,
      const std::string& peerName,
      std::shared_ptr<rfe::ScubaData> scubaLogger)
      : deviceName_(deviceName),
        statementName_(statementName),
        peerName_(peerName),
        scubaLogger_(std::move(scubaLogger)) {}
  ~RouteFilterLogger() = default;

  size_t log(
      bool egress,
      const folly::CIDRNetwork& prefix,
      bool allow,
      bool permissive,
      const std::vector<std::string>& communities);

 private:
  const std::string deviceName_;
  const std::string statementName_;
  const std::string peerName_;
  std::shared_ptr<rfe::ScubaData> scubaLogger_{nullptr};
};

} // namespace bgp
} // namespace facebook
