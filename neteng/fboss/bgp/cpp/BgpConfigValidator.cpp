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

#include <fmt/format.h>
#include <folly/init/Init.h>
#include <folly/logging/xlog.h>
#include <gflags/gflags.h>
#include <filesystem>

#include "neteng/fboss/bgp/cpp/config/Config.h"

DEFINE_string(config, "", "Path to BGP config file (required)");
DEFINE_string(policy, "", "Path to separate BGP policy file (optional)");

int main(int argc, char* argv[]) {
  folly::Init guard(&argc, &argv);

  if (FLAGS_config.empty()) {
    XLOG(ERR, "[ERROR] --config is required");
    return 1;
  }

  if (!std::filesystem::exists(FLAGS_config)) {
    XLOGF(ERR, "[ERROR] Config file does not exist: {}", FLAGS_config);
    return 1;
  }

  if (!FLAGS_policy.empty() && !std::filesystem::exists(FLAGS_policy)) {
    XLOGF(ERR, "[ERROR] Policy file does not exist: {}", FLAGS_policy);
    return 1;
  }

  try {
    auto config = std::make_shared<facebook::bgp::Config>(FLAGS_config);

    // BGP supports policy inside config or policy split from config
    if (!FLAGS_policy.empty()) {
      // Validates JSON syntax via folly::parseJson() first
      config->setPolicyConfigFromFile(FLAGS_policy);
    }

    auto policyManager = facebook::bgp::Config::createPolicyManager(config);

    XLOGF(
        INFO,
        "[PASS] Validation succeeded for config: {}, policy: {}",
        FLAGS_config,
        !FLAGS_policy.empty() ? FLAGS_policy : "N/A");

    return 0;
  } catch (const std::exception& ex) {
    XLOGF(
        ERR,
        "[FAIL] Validation failed for config: {}, policy: {} with exception: {}",
        FLAGS_config,
        !FLAGS_policy.empty() ? FLAGS_policy : "N/A",
        folly::exceptionStr(ex));
  }
  return 1;
}
