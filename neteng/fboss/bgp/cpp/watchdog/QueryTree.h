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

#include <folly/container/F14Map.h>
#include <memory>
#include <string>

namespace facebook::bgp {

struct QueryNode {
  bool isLeaf{false};
  // name -> child node
  folly::F14FastMap<std::string, std::unique_ptr<QueryNode>> children;

  // Finds or creates a child node with the given name
  std::unique_ptr<QueryNode>& findOrCreateChild(const std::string& childName);
  void markLeaf();
};

/*
 * A tree structure to represent the query path. The tree is built by adding
 *   paths to it. Each path is a string of the form "a.b.c.d". The query "a.b.c"
 *   means to query everything under "a.b.c", including "a.b.c", "a.b.c.d",
 *    "a.b.c.e", etc.
 */
struct QueryTree {
  QueryNode root;

  void addPath(const std::string& path);
};

} // namespace facebook::bgp
