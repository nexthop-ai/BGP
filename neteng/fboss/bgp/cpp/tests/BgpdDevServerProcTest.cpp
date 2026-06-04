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

#include "neteng/fboss/bgp/cpp/tests/BgpdDevServerProc.h"

#include <folly/init/Init.h>

using namespace facebook::bgp;

DEFINE_string(
    bgp_cpp_stand_alone_config,
    "",
    "Path to file containing BGP++ stand alone config unified");

DEFINE_string(
    bgp_cpp_stand_alone_settings,
    "",
    "Path to file containing BGP++ stand alone config settings");

DEFINE_string(
    bgp_cpp_stand_alone_policy,
    "",
    "Path to file containing BGP++ stand alone config policy");

DEFINE_string(bgp_cpp_binary_path, "", "Path to BGP++ binary");

namespace {
constexpr int PASS{0};
constexpr int FAIL{1};
} // namespace

/**
 * Here we are mainly using one example of using the BgpdDevServerProc class.
 * It also tests whether we can successfully start the bgpd stand alone process.
 * The logic is pretty straightforward; if the bgpd is ready to serve, then it
 * passes the testing, and otherwise it fails.
 */
int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);

  BgpdDevServerProc bgpdDevServerProc1(
      FLAGS_bgp_cpp_stand_alone_config, "", FLAGS_bgp_cpp_binary_path);
  if (!bgpdDevServerProc1.run()) {
    return FAIL;
  }
  bgpdDevServerProc1.stop();

  BgpdDevServerProc bgpdDevServerProc2(
      FLAGS_bgp_cpp_stand_alone_config,
      FLAGS_bgp_cpp_stand_alone_policy,
      FLAGS_bgp_cpp_binary_path);
  if (!bgpdDevServerProc2.run()) {
    return FAIL;
  }
  bgpdDevServerProc2.stop();

  BgpdDevServerProc bgpdDevServerProc3(
      FLAGS_bgp_cpp_stand_alone_settings,
      FLAGS_bgp_cpp_stand_alone_policy,
      FLAGS_bgp_cpp_binary_path);
  if (!bgpdDevServerProc3.run()) {
    return FAIL;
  }
  bgpdDevServerProc3.stop();

  return PASS;
}
