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

#include <gtest/gtest.h>

#include "neteng/fboss/bgp/cpp/watchdog/QueryTree.h"

using namespace ::testing;

namespace facebook::bgp {

TEST(QueryNodeTest, FindOrCreateChildTest) {
  QueryNode node;

  auto& nodePtr1 = node.findOrCreateChild("test1");

  // different object
  EXPECT_NE(nodePtr1.get(), &node);

  // Try to craete again
  auto& nodePtr2 = node.findOrCreateChild("test1");

  // Returned the created object
  EXPECT_EQ(nodePtr1, nodePtr2);
}

TEST(QueryNodeTest, MarkLeafTest) {
  QueryNode node;
  node.findOrCreateChild("test1");

  EXPECT_EQ(node.children.size(), 1);

  node.markLeaf();

  // stop at the leaf node
  EXPECT_TRUE(node.isLeaf);
  EXPECT_EQ(node.children.size(), 0);
}

void checkChildrenNames(
    const std::vector<std::string>& expectedChildrenNames,
    QueryNode* node) {
  EXPECT_EQ(expectedChildrenNames.size(), node->children.size());
  for (const auto& childName : expectedChildrenNames) {
    EXPECT_TRUE(node->children.contains(childName));
  }
}

TEST(QueryTreeTest, AddPathTest) {
  QueryTree tree;

  tree.addPath("test1.test3.test2");
  tree.addPath("test1.test3");
  tree.addPath("test1.test3.test4");
  tree.addPath("test5.test6");

  checkChildrenNames({"test1", "test5"}, &tree.root);

  QueryNode* node1 = tree.root.children.at("test1").get();
  checkChildrenNames({"test3"}, node1);

  QueryNode* node13 = node1->children.at("test3").get();
  checkChildrenNames({}, node13);
  EXPECT_TRUE(node13->isLeaf);

  QueryNode* node5 = tree.root.children.at("test5").get();
  checkChildrenNames({"test6"}, node5);
}

} // namespace facebook::bgp
