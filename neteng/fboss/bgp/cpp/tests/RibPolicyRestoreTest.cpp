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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#define RibBase_TEST_FRIENDS                                                   \
  FRIEND_TEST(RibPolicyRestoreTestFixture, RibPolicyRestoreTest);              \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, RibPolicyRestoreTestBadFileTermination);    \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, RibPolicyRestoreTestEmptyTermPolicy);       \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, RibPolicyRestoreTestMalformedContent);      \
  FRIEND_TEST(RibPolicyRestoreTestFixture, RibPolicyRestoreTestEmptyFile);     \
  FRIEND_TEST(RibPolicyRestoreTestFixture, RibPolicyRestoreTestMissingFile);   \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, RibPolicyRestoreTestBackwardCompatibility); \
  FRIEND_TEST(RibPolicyRestoreTestFixture, RibPolicyStoreExistsTest);          \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadTRibPolicyStoreTestBadFileTermination); \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadTRibPolicyStoreTestSuccessfulRead);     \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadTRibPolicyStoreTestMissingFile);        \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestEmptyPolicy);     \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestBadRibPolicy);    \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestEmptyFile);       \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestMissingFile);     \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture,                                             \
      ReadSaveRibPolicyStateTestBadTerminationString);                         \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestBadFileContent);  \
  FRIEND_TEST(RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTest);        \
  FRIEND_TEST(RibPolicyRestoreTestFixture, SaveTRibPolicyStorePrettyPrintsJson);

#define RouteAttributePolicy_TEST_FRIENDS                                      \
  FRIEND_TEST(RibPolicyRestoreTestFixture, RibPolicyRestoreTest);              \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, RibPolicyRestoreTestBadFileTermination);    \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, RibPolicyRestoreTestEmptyTermPolicy);       \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, RibPolicyRestoreTestMalformedContent);      \
  FRIEND_TEST(RibPolicyRestoreTestFixture, RibPolicyRestoreTestEmptyFile);     \
  FRIEND_TEST(RibPolicyRestoreTestFixture, RibPolicyRestoreTestMissingFile);   \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, RibPolicyRestoreTestBackwardCompatibility); \
  FRIEND_TEST(RibPolicyRestoreTestFixture, RibPolicyStoreExistsTest);          \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadTRibPolicyStoreTestBadFileTermination); \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadTRibPolicyStoreTestSuccessfulRead);     \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadTRibPolicyStoreTestMissingFile);        \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestEmptyPolicy);     \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestBadRibPolicy);    \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestEmptyFile);       \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestMissingFile);     \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture,                                             \
      ReadSaveRibPolicyStateTestBadTerminationString);                         \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestBadFileContent);  \
  FRIEND_TEST(RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTest);        \
  FRIEND_TEST(RibPolicyRestoreTestFixture, SaveTRibPolicyStorePrettyPrintsJson);

#include <boost/filesystem.hpp>

#include <folly/FileUtil.h>
#include <folly/IPAddress.h>
#include <folly/json/json.h>

#include "neteng/fboss/bgp/cpp/tests/RibPolicyUtils.h"
#include "neteng/fboss/bgp/cpp/tests/RibUtils.h"

using namespace facebook::bgp;
using namespace facebook::bgp::rib_policy;
using namespace facebook::neteng::fboss::bgp::thrift;
using namespace std::chrono;

namespace facebook::bgp {

class RibPolicyRestoreTestFixture : public RibFixture {
  // Delete routing policy state file (if it exists) before and after each test
  // to ensure idempotency.
  void SetUp() override {
    RibFixture::SetUp();
    boost::filesystem::remove(FLAGS_rp_state_file);
  }
  void TearDown() override {
    RibFixture::TearDown();
    boost::filesystem::remove(FLAGS_rp_state_file);
  }
};

/*
 * Test read rib policy state file with below cases:
 * - read file with stale timestamp
 * - read file with unmatch version
 * - read file with wrong term line
 * - read file with invalid rib-policy content
 * - read file with random content
 * - read empty file
 * - read non-existent file
 * - backward combatibility test for read/savePathSelectionPolicyState
 */
TEST_F(RibPolicyRestoreTestFixture, RibPolicyRestoreTest) {
  const auto prefix1 = folly::IPAddress::createNetwork("1::/64");
  auto tRibPolicy = createTRibPolicyLbw({prefix1}, kLbw10G);
  RibPolicy expectedPolicy{tRibPolicy};

  // save for testing
  {
    TRibPolicyStore tRibPolicyStore;
    tRibPolicyStore.fileTermination() = kRibPolicyFileTermination;
    tRibPolicyStore.policy() = tRibPolicy;
    rib_->saveTRibPolicyStore(tRibPolicyStore);
    EXPECT_TRUE(boost::filesystem::exists(FLAGS_rp_state_file));
  }

  // call read rib policy from file and compare
  {
    auto ribPolicy = rib_->readRibPolicyState();
    EXPECT_NE(nullptr, ribPolicy);
    EXPECT_EQ(expectedPolicy, *ribPolicy);
  }

  // the retrieved policy should be resumed from its saved time
  {
    auto storedTime = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count() -
        100;
    TRibPolicyStore oldPolicy;
    // make sure the timestamp is stale
    oldPolicy.storedTime() = storedTime;
    oldPolicy.fileTermination() = kRibPolicyFileTermination;

    // save a policy that valids for 20 seconds
    auto policy20s = tRibPolicy;
    auto& tSubPolicy = *policy20s.route_attribute_policy();
    auto& tStmt = tSubPolicy.statements()->find("stmt1")->second;
    tStmt.expiration_time_s() =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count() -
        80;

    policy20s.path_selection_policy() =
        createTPathSelectionPolicyWithPathSelector(
            {kV4Prefix1}, TPathSelector());

    oldPolicy.policy() = policy20s;

    // write to the file
    folly::writeFileAtomic(
        FLAGS_rp_state_file,
        apache::thrift::SimpleJSONSerializer::serialize<std::string>(
            oldPolicy));

    // verify reading rib policy has expired
    auto policy = rib_->readRibPolicyState();
    EXPECT_EQ(
        *tStmt.expiration_time_s(),
        policy->getRouteAttributePolicy()
            ->statements_.at("stmt1")
            .getExpirationTime());
    EXPECT_FALSE(
        policy->getRouteAttributePolicy()->statements_.at("stmt1").isActive());

    // Ensure that we do have the path selection policy loaded
    EXPECT_TRUE(policy->hasPathSelectionPolicy());
  }
}

// make saved file with bad termination string and read
TEST_F(RibPolicyRestoreTestFixture, RibPolicyRestoreTestBadFileTermination) {
  TRibPolicyStore badTermPolicy;
  auto badTerm = "bad ending line";

  // make sure the timestamp is stale
  badTermPolicy.storedTime() =
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  badTermPolicy.fileTermination() = badTerm;
  badTermPolicy.policy() = createTRibPolicyLbw({kV4Prefix1}, kLbw10G);

  // write to the file
  folly::writeFileAtomic(
      FLAGS_rp_state_file,
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(
          badTermPolicy));

  // verify reading rib policy will return nullptr
  auto policy = rib_->readRibPolicyState();
  EXPECT_EQ(nullptr, policy);
}

// make saved file with empty statement in rib policy and read
TEST_F(RibPolicyRestoreTestFixture, RibPolicyRestoreTestEmptyTermPolicy) {
  TRibPolicyStore badTermPolicy;
  TRibPolicy emptyPolicy = createTRibPolicyLbw({kV4Prefix1}, kLbw10G);
  emptyPolicy.route_attribute_policy().reset();

  // make sure the timestamp is stale
  badTermPolicy.storedTime() =
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  badTermPolicy.fileTermination() = kRibPolicyFileTermination;
  badTermPolicy.policy() = emptyPolicy;

  // write to the file
  folly::writeFileAtomic(
      FLAGS_rp_state_file,
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(
          badTermPolicy));

  // verify reading rib policy will return nullptr
  auto policy = rib_->readRibPolicyState();
  EXPECT_EQ(nullptr, policy);
}

// make saved file with random content and read
TEST_F(RibPolicyRestoreTestFixture, RibPolicyRestoreTestMalformedContent) {
  // write to the file
  folly::writeFileAtomic(
      FLAGS_rp_state_file, "anything other than rib policy state");

  // verify reading rib policy will return nullptr
  auto policy = rib_->readRibPolicyState();
  EXPECT_EQ(nullptr, policy);
}

// make empty saved file and read
TEST_F(RibPolicyRestoreTestFixture, RibPolicyRestoreTestEmptyFile) {
  // write to the file
  folly::writeFileAtomic(FLAGS_rp_state_file, "");

  // verify reading rib policy will return nullptr
  auto policy = rib_->readRibPolicyState();
  EXPECT_EQ(nullptr, policy);
}

// file does not exist and read
TEST_F(RibPolicyRestoreTestFixture, RibPolicyRestoreTestMissingFile) {
  // remove file
  boost::filesystem::remove(FLAGS_rp_state_file);

  // verify reading rib policy will return nullptr
  auto policy = rib_->readRibPolicyState();
  EXPECT_EQ(nullptr, policy);
}

// backward combatibility test for read/savePathSelectionPolicyState
// test the case when bgpd save RibPolicy by saveRibPolicyState, and we
// retrieve PathSelectionPolicy by readPathSelectionPolicyState
TEST_F(RibPolicyRestoreTestFixture, RibPolicyRestoreTestBackwardCompatibility) {
  {
    // Create the tPathSelectionPolicy and tRibPolicy for testing
    auto tMatcher = createCommunityMatch(200, 666, bgp_policy::Origin::EGP);
    auto tPathSelector = createTPathSlectorWithOneMatcher(tMatcher);

    auto tRibPolicy = createTRibPolicyLbw({kV4Prefix1}, kLbw10G);
    TPathSelectionPolicy tPathSelectionPolicy =
        createTPathSelectionPolicyWithPathSelector({kV4Prefix1}, tPathSelector);
    // update the RibPolicy
    tRibPolicy.path_selection_policy() = tPathSelectionPolicy;
    PathSelectionPolicy expectedPathSelectionPolicy{tPathSelectionPolicy};

    // save the tRibPolicy as saveRibPolicyState
    TRibPolicyStore tRibPolicyStore;
    tRibPolicyStore.fileTermination() = kRibPolicyFileTermination;
    tRibPolicyStore.policy() = tRibPolicy;
    rib_->saveTRibPolicyStore(tRibPolicyStore);
    EXPECT_TRUE(boost::filesystem::exists(FLAGS_rp_state_file));

    // read the path selection policy from file
    auto policy = rib_->readRibPolicyState();
    EXPECT_NE(nullptr, policy);
    EXPECT_EQ(expectedPathSelectionPolicy, *policy->getPathSelectionPolicy());
  }
  // clean up file during stress run
  boost::filesystem::remove(FLAGS_rp_state_file);
}

TEST_F(RibPolicyRestoreTestFixture, RibPolicyStoreExistsTest) {
  // write a dummy file for testing
  TPathSelectionPolicy tEmptyPolicy;
  tEmptyPolicy.statements()->emplace("stmt1", TPathSelectionStatement{});
  TRibPolicyStore tDummyPolicyStore;
  tDummyPolicyStore.policy()->path_selection_policy() = tEmptyPolicy;

  // write to the file
  folly::writeFileAtomic(
      FLAGS_rp_state_file,
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(
          tDummyPolicyStore));
  EXPECT_TRUE(rib_->ribPolicyStoreExists());

  // remove file
  boost::filesystem::remove(FLAGS_rp_state_file);
  EXPECT_FALSE(rib_->ribPolicyStoreExists());
}

// bad fileTermination would fail
TEST_F(RibPolicyRestoreTestFixture, ReadTRibPolicyStoreTestBadFileTermination) {
  TRibPolicyStore tBadPolicyStore;
  tBadPolicyStore.fileTermination() = "bad ending line";
  // write to the file
  folly::writeFileAtomic(
      FLAGS_rp_state_file,
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(
          tBadPolicyStore));

  auto [readSuccess, tRibPolicyStore] = rib_->readTRibPolicyStore();
  EXPECT_FALSE(readSuccess);
}

// Correct fileTermination and successful read
TEST_F(RibPolicyRestoreTestFixture, ReadTRibPolicyStoreTestSuccessfulRead) {
  // create path selector for a non-trivial read
  auto tMatcher = createCommunityMatch(200, 666, bgp_policy::Origin::EGP);
  auto tPathSelector = createTPathSlectorWithOneMatcher(tMatcher);

  TRibPolicyStore tCorrectPolicyStore;

  tCorrectPolicyStore.fileTermination() = kRibPolicyFileTermination;
  tCorrectPolicyStore.policy()->path_selection_policy() =
      createTPathSelectionPolicyWithPathSelector({kV4Prefix1}, tPathSelector);
  // write to the file
  folly::writeFileAtomic(
      FLAGS_rp_state_file,
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(
          tCorrectPolicyStore));
  auto [readSuccess, tRibPolicyStore] = rib_->readTRibPolicyStore();
  EXPECT_TRUE(readSuccess);
  // the stored time might be different, but the policies should be the same
  EXPECT_EQ(tCorrectPolicyStore.policy(), tRibPolicyStore.policy());
}

TEST_F(RibPolicyRestoreTestFixture, ReadTRibPolicyStoreTestMissingFile) {
  auto [readSuccess, tRibPolicyStore] = rib_->readTRibPolicyStore();
  EXPECT_FALSE(readSuccess);
}

// empty rib policy state would not result in file saved
TEST_F(RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestEmptyPolicy) {
  EXPECT_EQ(nullptr, rib_->pathSelectionPolicy_);
  EXPECT_EQ(nullptr, rib_->routeAttributePolicy_);
  EXPECT_EQ(nullptr, rib_->routeFilterPolicy_);
  rib_->saveRibPolicyState();
  EXPECT_FALSE(boost::filesystem::exists(FLAGS_rp_state_file));
}

// read a bad rib policy
TEST_F(RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestBadRibPolicy) {
  TRibPolicyStore badPolicyStore;

  TPathSelectionPolicy tEmptyPolicy;
  tEmptyPolicy.statements()->emplace("stmt1", TPathSelectionStatement{});

  badPolicyStore.policy()->path_selection_policy() = tEmptyPolicy;

  // write to the file
  folly::writeFileAtomic(
      FLAGS_rp_state_file,
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(
          badPolicyStore));

  // verify reading rib policy will return nullptr
  EXPECT_EQ(nullptr, rib_->readRibPolicyState());
}

// make saved file with bad termination string and read
TEST_F(
    RibPolicyRestoreTestFixture,
    ReadSaveRibPolicyStateTestBadTerminationString) {
  auto tMatcher = createCommunityMatch(200, 666, bgp_policy::Origin::EGP);
  auto tPathSelector = createTPathSlectorWithOneMatcher(tMatcher);
  auto tPathSelectionPolicy =
      createTPathSelectionPolicyWithPathSelector({kV4Prefix1}, tPathSelector);

  TRibPolicyStore badPolicyStore;
  auto badTerm = "bad ending line";

  // make sure the timestamp is stale
  badPolicyStore.fileTermination() = badTerm;
  badPolicyStore.policy()->path_selection_policy() = tPathSelectionPolicy;

  // write to the file
  folly::writeFileAtomic(
      FLAGS_rp_state_file,
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(
          badPolicyStore));

  // verify reading rib policy will return nullptr
  EXPECT_EQ(nullptr, rib_->readRibPolicyState());
}

// make saved file with random content and read
TEST_F(RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestBadFileContent) {
  // write to the file
  folly::writeFileAtomic(
      FLAGS_rp_state_file, "anything other than rib policy state");

  // verify reading rib policy will return nullptr
  EXPECT_EQ(nullptr, rib_->readRibPolicyState());
}

// make empty saved file and read
TEST_F(RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestEmptyFile) {
  // write to the file
  folly::writeFileAtomic(FLAGS_rp_state_file, "");

  // verify reading rib policy will return nullptr
  EXPECT_EQ(nullptr, rib_->readRibPolicyState());
}

// file does not exist and read
TEST_F(RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestMissingFile) {
  // remove file
  boost::filesystem::remove(FLAGS_rp_state_file);

  // verify reading rib policy will return nullptr
  EXPECT_EQ(nullptr, rib_->readRibPolicyState());
}

TEST_F(RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTest) {
  // Create the tPathSelectionPolicy for testing
  auto tMatcher = createCommunityMatch(200, 666, bgp_policy::Origin::EGP);
  auto tPathSelector = createTPathSlectorWithOneMatcher(tMatcher);

  auto tPathSelectionPolicy =
      createTPathSelectionPolicyWithPathSelector({kV4Prefix1}, tPathSelector);
  PathSelectionPolicy expectedPsPolicy{tPathSelectionPolicy};

  // Create the tRouteAttributePolicy for testing
  auto prefix1 = folly::IPAddress::createNetwork("::/0");
  auto tRouteAttributePolicy = createTRouteAttributePolicyLbw(
      {prefix1},
      kLbw10G,
      "stmt1",
      seconds(std::time(nullptr)).count() + 60 /* 60s */);
  RouteAttributePolicy expectedRaPolicy{tRouteAttributePolicy};

  // Create RouteFilterPolicy for testing
  auto tRouteFilterPolicy =
      createTRouteFilterPolicy({createTRouteFilterStatement({prefix1})}, 12345);
  RouteFilterPolicy expectedRfPolicy{tRouteFilterPolicy};

  // make sure no file was generated at first place
  EXPECT_FALSE(boost::filesystem::exists(FLAGS_rp_state_file));

  // call set path selection policy and then save rib policy to file
  {
    auto ribResult = sendPathSelectionPolicySet(tPathSelectionPolicy);
    EXPECT_TRUE(*ribResult.success());

    // wait till policy output queue got item
    rib_->waitForPathSelectionPolicyUpdate();

    // save the policy to file
    rib_->saveRibPolicyState();
    EXPECT_TRUE(boost::filesystem::exists(FLAGS_rp_state_file));
  }

  // read path selection policy from file and compare
  {
    auto policy = rib_->readRibPolicyState();
    EXPECT_NE(nullptr, policy);
    EXPECT_EQ(expectedPsPolicy, *policy->getPathSelectionPolicy());
    // no ra policy
    EXPECT_FALSE(policy->hasRouteAttributePolicy());
    EXPECT_FALSE(policy->hasRouteFilterPolicy());
  }

  // call set route attribute policy and then save rib policy to file
  {
    auto ribResult = sendRouteAttributePolicySet(tRouteAttributePolicy);
    EXPECT_TRUE(*ribResult.success());

    // wait till policy output queue got item
    rib_->waitForRouteAttributePolicyUpdate();

    // save the policy to file
    rib_->saveRibPolicyState();
    EXPECT_TRUE(boost::filesystem::exists(FLAGS_rp_state_file));
  }

  // read route attribute policy from file and compare
  {
    auto policy = rib_->readRibPolicyState();
    EXPECT_NE(nullptr, policy);
    EXPECT_EQ(expectedPsPolicy, *policy->getPathSelectionPolicy());
    EXPECT_EQ(expectedRaPolicy, *policy->getRouteAttributePolicy());
    EXPECT_FALSE(policy->hasRouteFilterPolicy());
  }

  // call set route filter policy and then save rib policy to file
  {
    sendRouteFilterPolicySet(tRouteFilterPolicy);

    // wait till policy output queue got item
    rib_->waitForRouteFilterPolicyUpdate();

    // save the policy to file
    rib_->saveRibPolicyState();
    EXPECT_TRUE(boost::filesystem::exists(FLAGS_rp_state_file));
  }

  // read route filter policy from file and compare
  {
    auto policy = rib_->readRibPolicyState();
    EXPECT_NE(nullptr, policy);
    EXPECT_EQ(expectedPsPolicy, *policy->getPathSelectionPolicy());
    EXPECT_EQ(expectedRaPolicy, *policy->getRouteAttributePolicy());
    EXPECT_EQ(expectedRfPolicy, *policy->getRouteFilterPolicy());
  }

  // clear path selection policy, verify other policies are untouched
  {
    rib_->clearPathSelectionPolicy();
    // wait till policy output queue got item
    rib_->waitForPathSelectionPolicyClear();

    rib_->saveRibPolicyState();

    auto policy = rib_->readRibPolicyState();
    EXPECT_NE(nullptr, policy);
    EXPECT_FALSE(policy->hasPathSelectionPolicy());
    EXPECT_EQ(expectedRaPolicy, *policy->getRouteAttributePolicy());
    EXPECT_EQ(expectedRfPolicy, *policy->getRouteFilterPolicy());
  }

  // clear route attribute policy, verify other policies are untouched
  {
    rib_->clearRouteAttributePolicy();
    // wait till policy output queue got item
    rib_->waitForRouteAttributePolicyClear();

    rib_->saveRibPolicyState();

    auto policy = rib_->readRibPolicyState();
    EXPECT_NE(nullptr, policy);
    EXPECT_FALSE(policy->hasPathSelectionPolicy());
    EXPECT_FALSE(policy->hasRouteAttributePolicy());
    EXPECT_EQ(expectedRfPolicy, *policy->getRouteFilterPolicy());
  }
  // clear route filter policy, verify rib policy file is gone
  {
    rib_->clearRouteFilterPolicy();
    // wait till policy output queue got item
    rib_->waitForRouteFilterPolicyClear();

    rib_->saveRibPolicyState();

    auto policy = rib_->readRibPolicyState();
    EXPECT_EQ(nullptr, policy);
  }
}
TEST_F(RibPolicyRestoreTestFixture, SaveTRibPolicyStorePrettyPrintsJson) {
  const auto prefix1 = folly::IPAddress::createNetwork("1::/64");
  auto tRibPolicy = createTRibPolicyLbw({prefix1}, kLbw10G);

  TRibPolicyStore tRibPolicyStore;
  tRibPolicyStore.fileTermination() = kRibPolicyFileTermination;
  tRibPolicyStore.policy() = tRibPolicy;
  rib_->saveTRibPolicyStore(tRibPolicyStore);

  // Read raw file content and verify it is pretty-printed (contains newlines)
  std::string fileContent;
  ASSERT_TRUE(folly::readFile(FLAGS_rp_state_file.c_str(), fileContent));
  EXPECT_NE(fileContent.find('\n'), std::string::npos);

  // Verify it is valid JSON that parses successfully
  auto parsed = folly::parseJson(fileContent);
  EXPECT_TRUE(parsed.isObject());

  // Verify round-trip: deserialized content matches original
  auto [readSuccess, readBack] = rib_->readTRibPolicyStore();
  EXPECT_TRUE(readSuccess);
  EXPECT_EQ(*tRibPolicyStore.fileTermination(), *readBack.fileTermination());
}
} // namespace facebook::bgp
