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

#include <iostream>
#include <string>
#include <vector>

#include <gflags/gflags.h>

#include <folly/init/Init.h>

#include "neteng/fboss/bgp/cpp/sim/BgpSimulatorCli.h"

DEFINE_bool(
    aggregated_config,
    false,
    "Treat each argument as an aggregated config file: a JSON object with a "
    "single outer wrapper key whose value maps switch name -> BgpConfig (as "
    "produced by the emulator `routes save-bgp-configs` command, e.g. "
    "{\"bgp_configs\": {\"switch_name1\": {...}, \"switch_name2\": {...}}}), "
    "instead of a per-switch config file or a directory of them.");

int main(int argc, char* argv[]) {
  folly::Init init(&argc, &argv);

  const std::vector<std::string> args(argv + 1, argv + argc);

  if (FLAGS_aggregated_config) {
    if (args.empty()) {
      std::cerr << "Usage: " << argv[0]
                << " --aggregated_config <aggregated_config_file>..."
                << std::endl;
      return 1;
    }
    return facebook::bgp::runSimulation(args, std::cout, /*aggregated=*/true);
  }

  const auto configPaths = facebook::bgp::collectConfigPaths(args);
  if (configPaths.empty()) {
    std::cerr << "Usage: " << argv[0] << " <config_dir | config_file>..."
              << std::endl;
    return 1;
  }

  return facebook::bgp::runSimulation(configPaths, std::cout);
}
