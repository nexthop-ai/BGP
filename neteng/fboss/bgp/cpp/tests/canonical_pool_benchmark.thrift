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

package "facebook.com/neteng/fboss/bgp/cpp/tests/canonical_pool_benchmark"

include "thrift/annotation/cpp.thrift"

namespace cpp2 facebook.bgp.benchmark

struct PoolValue {
  1: binary payload;
}

struct ListPools {
  1: list<PoolValue> wholePaths;
  2: list<PoolValue> asPaths;
  3: list<PoolValue> communities;
  4: list<PoolValue> extCommunities;
  5: list<PoolValue> clusterLists;
  6: list<PoolValue> peers;
}

struct MapPools {
  @cpp.Type{template = "std::unordered_map"}
  1: map<i64, PoolValue> wholePaths;
  @cpp.Type{template = "std::unordered_map"}
  2: map<i64, PoolValue> asPaths;
  @cpp.Type{template = "std::unordered_map"}
  3: map<i64, PoolValue> communities;
  @cpp.Type{template = "std::unordered_map"}
  4: map<i64, PoolValue> extCommunities;
  @cpp.Type{template = "std::unordered_map"}
  5: map<i64, PoolValue> clusterLists;
  @cpp.Type{template = "std::unordered_map"}
  6: map<i64, PoolValue> peers;
}
