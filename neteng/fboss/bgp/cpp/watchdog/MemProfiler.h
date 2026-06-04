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

#include <string>

namespace facebook::bgp {

/**
 * Set heap profiling mode via jemalloc.
 * Internal build: delegates to openr::setHeapProfilingMode().
 * OSS build: no-op stub.
 *
 * @return true if successful, false otherwise.
 */
bool setHeapProfilingMode(bool enable);

/**
 * Dump heap profile to a file.
 * Internal build: delegates to openr::getHeapDump().
 * OSS build: no-op stub that returns an empty string.
 *
 * @param suffix Suffix for the heap dump filename.
 * @return The filename of the heap dump.
 */
std::string getHeapDump(const std::string& suffix);

} // namespace facebook::bgp
