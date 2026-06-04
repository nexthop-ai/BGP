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

#include "neteng/fboss/bgp/cpp/watchdog/MemProfiler.h"

#include <folly/logging/xlog.h>

namespace facebook::bgp {

bool setHeapProfilingMode(bool /*enable*/) {
  XLOG(INFO, "Please add your own implementation to enable heap profiling.");
  return false;
}

std::string getHeapDump(const std::string& /*suffix*/) {
  XLOG(
      INFO,
      "Please add your own implementation to dump the heap memory profile.");
  return "";
}

} // namespace facebook::bgp
