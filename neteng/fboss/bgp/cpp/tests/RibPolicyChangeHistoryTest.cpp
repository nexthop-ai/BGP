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

#include <folly/FileUtil.h>
#include <folly/String.h>
#include <folly/testing/TestUtil.h>
#include <gtest/gtest.h>

#include "neteng/fboss/bgp/cpp/rib/RibBase.h"

#include <string>
#include <vector>

DECLARE_string(rp_change_history_file);

namespace {

constexpr size_t kMaxLines = 50;

// Helper: return non-empty lines from the file.
std::vector<std::string> readLines(const std::string& filePath) {
  std::string content;
  folly::readFile(filePath.c_str(), content);
  std::vector<folly::StringPiece> pieces;
  folly::split('\n', content, pieces);
  std::vector<std::string> result;
  for (auto& p : pieces) {
    if (!p.empty()) {
      result.push_back(p.str());
    }
  }
  return result;
}

} // namespace

class RibPolicyChangeHistoryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    tmpFile_ = std::make_unique<folly::test::TemporaryFile>("rp_hist_test");
    filePath_ = tmpFile_->path().string();
    // Start with an empty file.
    folly::writeFileAtomic(filePath_, std::string(""));
  }

  std::unique_ptr<folly::test::TemporaryFile> tmpFile_;
  std::string filePath_;
};

// 1. Basic write test — write one CPS entry, verify file content
TEST_F(RibPolicyChangeHistoryTest, BasicWrite) {
  gflags::FlagSaver fs;
  FLAGS_rp_change_history_file = filePath_;

  facebook::bgp::RibBase::appendRibPolicyChangeHistory("CPS", 1);

  auto lines = readLines(filePath_);
  ASSERT_EQ(lines.size(), 1);
  EXPECT_TRUE(lines[0].find(" CPS 1") != std::string::npos);
}

// 2. Append test — write CPS + CRF entries, verify both present
TEST_F(RibPolicyChangeHistoryTest, AppendMultipleTypes) {
  gflags::FlagSaver fs;
  FLAGS_rp_change_history_file = filePath_;

  facebook::bgp::RibBase::appendRibPolicyChangeHistory("CPS", 10);
  facebook::bgp::RibBase::appendRibPolicyChangeHistory("CRF", 20);

  auto lines = readLines(filePath_);
  ASSERT_EQ(lines.size(), 2);
  EXPECT_TRUE(lines[0].find(" CPS 10") != std::string::npos);
  EXPECT_TRUE(lines[1].find(" CRF 20") != std::string::npos);
}

// 3. Global cap test — write 55 entries, verify only last 50 remain
TEST_F(RibPolicyChangeHistoryTest, GlobalCapAt50) {
  gflags::FlagSaver fs;
  FLAGS_rp_change_history_file = filePath_;

  for (int i = 1; i <= 55; ++i) {
    facebook::bgp::RibBase::appendRibPolicyChangeHistory("CPS", i);
  }

  auto lines = readLines(filePath_);
  EXPECT_EQ(lines.size(), kMaxLines);
  // The oldest entries (version 1-5) should have been trimmed.
  EXPECT_TRUE(lines.back().find(" CPS 55") != std::string::npos);
  EXPECT_TRUE(lines.front().find(" CPS 6") != std::string::npos);
}

// 4. Mixed types cap test — write mixed entries exceeding cap
TEST_F(RibPolicyChangeHistoryTest, MixedTypesCap) {
  gflags::FlagSaver fs;
  FLAGS_rp_change_history_file = filePath_;

  for (int i = 1; i <= 30; ++i) {
    facebook::bgp::RibBase::appendRibPolicyChangeHistory("CPS", i);
    facebook::bgp::RibBase::appendRibPolicyChangeHistory("CRF", i + 100);
  }

  // 60 entries total, should be capped at 50
  auto lines = readLines(filePath_);
  EXPECT_EQ(lines.size(), kMaxLines);
  // Last entry should be CRF 130
  EXPECT_TRUE(lines.back().find(" CRF 130") != std::string::npos);
}

// 5. Version -1 test — verify version -1 is written when policy is cleared
TEST_F(RibPolicyChangeHistoryTest, VersionNegativeOne) {
  gflags::FlagSaver fs;
  FLAGS_rp_change_history_file = filePath_;

  facebook::bgp::RibBase::appendRibPolicyChangeHistory("CPS", -1);

  auto lines = readLines(filePath_);
  ASSERT_EQ(lines.size(), 1);
  EXPECT_TRUE(lines[0].find(" CPS -1") != std::string::npos);
}

// 6. Error handling test — write to invalid path, verify no crash (noexcept)
TEST_F(RibPolicyChangeHistoryTest, ErrorHandlingInvalidPath) {
  gflags::FlagSaver fs;
  // Point to an invalid path and verify no crash (noexcept method).
  FLAGS_rp_change_history_file = "/nonexistent/dir/file.txt";
  // This should not crash — the function catches exceptions internally.
  facebook::bgp::RibBase::appendRibPolicyChangeHistory("CPS", 1);
}

// 7. Ordering preserved test — entries appear in insertion order
TEST_F(RibPolicyChangeHistoryTest, OrderingPreserved) {
  gflags::FlagSaver fs;
  FLAGS_rp_change_history_file = filePath_;

  facebook::bgp::RibBase::appendRibPolicyChangeHistory("CPS", 1);
  facebook::bgp::RibBase::appendRibPolicyChangeHistory("CRF", 2);
  facebook::bgp::RibBase::appendRibPolicyChangeHistory("CPS", 3);

  auto lines = readLines(filePath_);
  ASSERT_EQ(lines.size(), 3);
  EXPECT_TRUE(lines[0].find(" CPS 1") != std::string::npos);
  EXPECT_TRUE(lines[1].find(" CRF 2") != std::string::npos);
  EXPECT_TRUE(lines[2].find(" CPS 3") != std::string::npos);
}
