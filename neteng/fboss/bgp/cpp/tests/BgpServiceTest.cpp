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

#include <fb303/FollyLoggingHandler.h>
#include <fb303/ServiceData.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/GtestHelpers.h>
#include <folly/logging/LogMessage.h>
#include <folly/logging/LoggerDB.h>
#include <folly/logging/test/TestLogHandler.h>
#include <folly/test/JsonTestUtil.h>

#include "neteng/fboss/bgp/cpp/BgpServiceDC.h"
#include "neteng/fboss/bgp/cpp/BgpServiceUtil.h"
#include "neteng/fboss/bgp/cpp/config/ConfigManager.h"
#include "neteng/fboss/bgp/cpp/tests/MockPeerManager.h"
#include "neteng/fboss/bgp/cpp/tests/MockSessionManager.h"
#include "neteng/fboss/bgp/cpp/tests/RibPolicyUtils.h"
#include "neteng/fboss/bgp/cpp/tests/RibUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/if/gen-cpp2/bgp_thrift_types.h"

using namespace ::testing;
using namespace facebook::neteng::fboss::bgp_attr;
using namespace facebook::neteng::fboss::bgp::thrift;

namespace facebook::bgp {
static const std::string kExitNullPtrLogPrefix = "ExitOrNullPtr";

class BgpServiceTestFixture : public ::testing::Test {
 public:
  void SetUp() override {
    // rib and peerManager are not tested in the tests
    // so it is fine to keep invalid references in services_ for now

    // config
    config_ = createConfig();
    auto configManager = std::make_shared<ConfigManager>(config_);

    // rib
    rib_ = std::make_unique<MockRib>(
        std::unordered_map<folly::CIDRNetwork, thrift::BgpNetwork>{},
        globalConfig_,
        std::nullopt /* policyConfig */,
        ribInQ_,
        ribOutQ_,
        "dev" /* platform */,
        nullptr /* fsdbSyncer*/);

    // peerManager
    peerManager_ = std::make_unique<MockPeerManager>(
        configManager, ribInQ_, ribOutQ_, neighborEventQ_);

    sessionMgr_ = std::make_shared<MockSessionManager>(globalConfig_, false);

    peerManager_->setSessionManager(sessionMgr_);

    // watchdog
    watchdog_ = std::make_unique<Watchdog>(config_);

    // bgpService
    service_ = std::make_unique<BgpServiceDC>(
        *peerManager_,
        configManager,
        *rib_,
        nullptr /* neighbor watcher */,
        *watchdog_,
        false /* thrift protection */);
  }

  void TearDown() override {
    service_.reset();
  }

 protected:
  std::unique_ptr<MockRib> rib_;
  std::unique_ptr<MockPeerManager> peerManager_;
  std::unique_ptr<BgpServiceDC> service_;
  std::shared_ptr<Config> config_;
  std::unique_ptr<Watchdog> watchdog_;
  std::shared_ptr<MockSessionManager> sessionMgr_;

 private:
  /*
   * [Config]
   *
   * Define a virtual method to be inherited and overridden by child test
   * fixtures for customize config setup.
   */
  virtual std::shared_ptr<Config> createConfig() {
    thrift::BgpConfig thriftConfig;
    thriftConfig.router_id() = kLocalAddr1.str();
    thriftConfig.local_as() = kAsn1;
    thriftConfig.hold_time() = kHoldTime.count();
    thriftConfig.graceful_restart_convergence_seconds() =
        kGrRestartTime.count();
    thriftConfig.listen_addr() = kLocalAddr1.str();
    thriftConfig.eor_time_s() = 45;

    return std::make_shared<Config>(std::move(thriftConfig));
  }

  BgpGlobalConfig globalConfig_{
      kAsn1, // localAsn
      kLocalAddr1, // routerId
      kPeerAddr3, // clusterId
      kHoldTime, // holdTime
      std::nullopt, // listenAddr
      kGrRestartTime, // grRestartTime
      {}, // networksV4
      {} // networksV6
  };

  // Rib
  nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage> ribInQ_{
      nettools::bgplib::kMaxIngressQueueSize};
  MonitoredMPMCQueue<RibOutMessage> ribOutQ_;

  // Peer Manager
  std::optional<MonitoredMPMCQueue<NeighborWatcherMessage>> neighborEventQ_;
};

class BgpServiceNullPtrTestFixture : public BgpServiceTestFixture {
 public:
  void SetUp() override {
    BgpServiceTestFixture::SetUp();
    logHandler_ = std::make_shared<folly::TestLogHandler>();
    folly::LoggerDB::get().getCategory("")->addHandler(logHandler_);
    folly::LoggerDB::get().setLevel("", folly::LogLevel::INFO);
  }

  std::shared_ptr<folly::TestLogHandler> logHandler_;
  // Placeholder variables for reference arguments
  TResult res_;
  std::vector<rib_policy::TPathSelector> resPthSelector_;
  std::vector<TRibEntry> resRibEntry_;
  TBgpAfi afi_{0};
};

class BgpServiceBaseNullPtrTestFixture : public BgpServiceTestFixture {
 public:
  void SetUp() override {
    BgpServiceTestFixture::SetUp();
    logHandler_ = std::make_shared<folly::TestLogHandler>();
    folly::LoggerDB::get().getCategory("")->addHandler(logHandler_);
    folly::LoggerDB::get().setLevel("", folly::LogLevel::INFO);
  }

  std::shared_ptr<folly::TestLogHandler> logHandler_;
  // Placeholder variables for reference arguments
  std::vector<TBgpSession> resBgpSession_;
  std::map<TIpPrefix, TBgpPath> resBgpPath_;
  std::map<TIpPrefix, std::vector<TBgpPath>> resBgpPaths_;
  std::map<TIpPrefix, TBgpNetwork> resBgpNetworkMap_;
  std::vector<TBgpNetwork> resBgpNetwork_;
};

class BgpServiceWithNodeDrainedConfigTestFixture
    : public BgpServiceTestFixture {
 public:
  std::shared_ptr<Config> createConfig() override {
    thrift::BgpConfig bgpConfig;
    bgpConfig.router_id() = kLocalAddr1.str();
    bgpConfig.local_as() = kAsn1;
    bgpConfig.hold_time() = kHoldTime.count();
    bgpConfig.graceful_restart_convergence_seconds() = kGrRestartTime.count();
    bgpConfig.listen_addr() = kLocalAddr1.str();
    bgpConfig.eor_time_s() = 45;
    bgpConfig.drain_state() = bgp_policy::DrainState::DRAINED;

    return std::make_shared<Config>(std::move(bgpConfig));
  }
};

class BgpServiceWithInterfaceDrainedConfigTestFixture
    : public BgpServiceTestFixture {
 public:
  std::shared_ptr<Config> createConfig() override {
    thrift::BgpConfig bgpConfig;
    bgpConfig.router_id() = kLocalAddr1.str();
    bgpConfig.local_as() = kAsn1;
    bgpConfig.hold_time() = kHoldTime.count();
    bgpConfig.graceful_restart_convergence_seconds() = kGrRestartTime.count();
    bgpConfig.listen_addr() = kLocalAddr1.str();
    bgpConfig.eor_time_s() = 45;
    bgpConfig.drain_state() = bgp_policy::DrainState::UNDRAINED;
    bgpConfig.drained_interfaces() = {"ethernet1/1"};

    return std::make_shared<Config>(std::move(bgpConfig));
  }
};

/*
 * Test setLogLevel API, inspired by https://fburl.com/code/t7sy7hd8
 */
TEST_F(BgpServiceTestFixture, SetLogLevelTest) {
  // include the handler
  facebook::fb303::registerFollyLoggingOptionHandlers();

  service_->setLogLevel(std::make_unique<std::string>(".=DBG1"));

  FOLLY_EXPECT_JSON_EQ(
      R"JSON({
  "categories" : {
    "" : {
      "handlers" : [
        "default"
      ],
      "inherit" : false,
      "level" : "DBG1",
      "propagate": "NONE"
    }
  },
  "handlers" : {
    "default" : {
      "options" : {
        "async" : "true",
        "stream" : "stderr"
      },
      "type" : "stream"
    }
  }
})JSON",
      fb303::ThreadCachedServiceData::get()->getOption("logging"));

  service_->setLogLevel(
      std::make_unique<std::string>(".=INFO;default:async=true"));

  FOLLY_EXPECT_JSON_EQ(
      R"JSON({
  "categories" : {
    "" : {
      "handlers" : [
        "default"
      ],
      "inherit" : false,
      "level" : "INFO",
      "propagate": "NONE"
    }
  },
  "handlers" : {
    "default" : {
      "options" : {
        "async" : "true",
        "stream" : "stderr"
      },
      "type" : "stream"
    }
  }
})JSON",
      fb303::ThreadCachedServiceData::get()->getOption("logging"));
}

TEST_F(BgpServiceTestFixture, InitializationApiTest) {
  const auto initializingTime{1};
  const auto initializedTime{100};

  /*
   * Step1: zero out counters
   */
  facebook::fb303::ThreadCachedServiceData::getShared()->clearCounter(
      fmt::format(
          kInitEventCounterFormat,
          apache::thrift::util::enumNameSafe(
              BgpInitializationEvent::INITIALIZING)));

  facebook::fb303::ThreadCachedServiceData::getShared()->clearCounter(
      fmt::format(
          kInitEventCounterFormat,
          apache::thrift::util::enumNameSafe(
              BgpInitializationEvent::INITIALIZED)));

  // verify NO converge signal
  EXPECT_FALSE(service_->initializationConverged());

  /*
   * Step2: test initializationConverged() API
   */
  facebook::fb303::ThreadCachedServiceData::getShared()->setCounter(
      fmt::format(
          kInitEventCounterFormat,
          apache::thrift::util::enumNameSafe(
              BgpInitializationEvent::INITIALIZED)),
      initializedTime /* time elapsed */);

  // verify initializing state
  EXPECT_TRUE(service_->initializationConverged());

  /*
   * Step3: test getInitializationEvents() API
   */
  BgpInitializationMap res{};
  service_->getInitializationEvents(res);
  EXPECT_EQ(1, res.size());
  EXPECT_EQ(initializedTime, res.at(BgpInitializationEvent::INITIALIZED));

  // set initialized time
  facebook::fb303::ThreadCachedServiceData::getShared()->setCounter(
      fmt::format(
          kInitEventCounterFormat,
          apache::thrift::util::enumNameSafe(
              BgpInitializationEvent::INITIALIZING)),
      initializingTime /* time elapsed */);

  service_->getInitializationEvents(res);
  EXPECT_EQ(2, res.size());
  EXPECT_EQ(initializingTime, res.at(BgpInitializationEvent::INITIALIZING));
}

// Test getNodeDrainState with UNDRAIN state
TEST_F(BgpServiceTestFixture, GetUndrainedNodeTest) {
  neteng::fboss::bgp::thrift::TBgpDrainState state;
  service_->getDrainState(state);
  EXPECT_EQ(*state.drain_state(), bgp_policy::DrainState::UNDRAINED);
  ASSERT_TRUE(state.drained_interfaces()->empty());
}

// Test getNodeDrainState with DRAIN state
TEST_F(BgpServiceWithNodeDrainedConfigTestFixture, GetDrainedNodeTest) {
  neteng::fboss::bgp::thrift::TBgpDrainState state;
  service_->getDrainState(state);
  EXPECT_EQ(*state.drain_state(), bgp_policy::DrainState::DRAINED);
  ASSERT_TRUE(state.drained_interfaces()->empty());
}

// Test getNodeDrainState with interface drained
TEST_F(
    BgpServiceWithInterfaceDrainedConfigTestFixture,
    GetDrainedInterfaceTest) {
  neteng::fboss::bgp::thrift::TBgpDrainState state;
  service_->getDrainState(state);
  EXPECT_EQ(*state.drain_state(), bgp_policy::DrainState::UNDRAINED);
  EXPECT_EQ(state.drained_interfaces()->size(), 1);
}

// test validateConfig
TEST_F(BgpServiceNullPtrTestFixture, ValidateConfigNullPtrTest) {
  service_->validateConfig(res_, nullptr);
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test validateConfigAndPolicy
TEST_F(BgpServiceNullPtrTestFixture, ValidateConfigAndPolicyNullPtrTest) {
  service_->validateConfigAndPolicy(res_, nullptr, nullptr);
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_setRouteAttributePolicy
CO_TEST_F(BgpServiceNullPtrTestFixture, SetRouteAttributePolicyNullPtrTest) {
  co_await service_->co_setRouteAttributePolicy(nullptr);
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_setPathSelectionPolicy
CO_TEST_F(BgpServiceNullPtrTestFixture, SetPathSelectionPolicyNullPtrTest) {
  co_await service_->co_setPathSelectionPolicy(nullptr);
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_getActivePathSelectionCriteria
TEST_F(
    BgpServiceNullPtrTestFixture,
    GetActivePathSelectionCriteriaNullPtrTest) {
  folly::coro::blockingWait(
      service_->co_getActivePathSelectionCriteria(nullptr));
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_setRouteFilterPolicy
CO_TEST_F(BgpServiceNullPtrTestFixture, SetRouteFilterPolicyNullPtrTest) {
  co_await service_->co_setRouteFilterPolicy(nullptr);
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// Helper function to create a peer group with AFI settings
thrift::PeerGroup createTestPeerGroup(
    bool disableIpv4 = false,
    bool disableIpv6 = false) {
  thrift::PeerGroup peerGroup;
  if (disableIpv4) {
    peerGroup.disable_ipv4_afi() = true;
  }
  if (disableIpv6) {
    peerGroup.disable_ipv6_afi() = true;
  }
  return peerGroup;
}

class BgpServicePeerGroupValidationTestFixture : public BgpServiceTestFixture {
 public:
  void SetUp() override {
    BgpServiceTestFixture::SetUp();
    logHandler_ = std::make_shared<folly::TestLogHandler>();
    folly::LoggerDB::get().getCategory("")->addHandler(logHandler_);
    folly::LoggerDB::get().setLevel("", folly::LogLevel::INFO);
  }

  std::shared_ptr<Config> createConfig() override {
    thrift::BgpConfig thriftConfig;
    thriftConfig.router_id() = kLocalAddr1.str();
    thriftConfig.local_as() = kAsn1;
    thriftConfig.hold_time() = kHoldTime.count();
    thriftConfig.graceful_restart_convergence_seconds() =
        kGrRestartTime.count();
    thriftConfig.listen_addr() = kLocalAddr1.str();
    thriftConfig.eor_time_s() = 45;

    // Add test peer groups
    std::vector<thrift::PeerGroup> testPeerGroups;

    thrift::PeerGroup peerGroup1 = createTestPeerGroup(false, true);
    peerGroup1.name() = "test-peer-group-ipv4-enabled";
    testPeerGroups.push_back(peerGroup1);

    thrift::PeerGroup peerGroup2 = createTestPeerGroup(true, false);
    peerGroup2.name() = "test-peer-group-ipv6-enabled";
    testPeerGroups.push_back(peerGroup2);

    thrift::PeerGroup peerGroup3 = createTestPeerGroup(false, false);
    peerGroup3.name() = "test-peer-group-both-enabled";
    testPeerGroups.push_back(peerGroup3);

    thrift::PeerGroup peerGroup4 = createTestPeerGroup(true, true);
    peerGroup4.name() = "test-peer-group-both-disabled";
    testPeerGroups.push_back(peerGroup4);

    thriftConfig.peer_groups() = testPeerGroups;

    return std::make_shared<Config>(std::move(thriftConfig));
  }

  std::shared_ptr<folly::TestLogHandler> logHandler_;
  TResult res_;
};

// Test setRouteFilterPolicy with peer group that doesn't exist
TEST_F(
    BgpServicePeerGroupValidationTestFixture,
    SetRouteFilterPolicyPeerGroupNotFoundTest) {
  rib_policy::TRouteFilterPolicy tPolicy;
  tPolicy.key_type() = rib_policy::KeyType::PEER_GROUP_NAME;
  tPolicy.statements()->emplace(
      "nonexistent-peer-group", createTRouteFilterStatement({}, false, true));

  auto res = folly::coro::blockingWait(service_->co_setRouteFilterPolicy(
      std::make_unique<rib_policy::TRouteFilterPolicy>(std::move(tPolicy))));

  EXPECT_FALSE(*res->success());
  EXPECT_THAT(*res->err(), HasSubstr("PEER_GROUP_NOT_FOUND"));
  EXPECT_THAT(
      logHandler_->getMessageValues(),
      Contains(ContainsRegex("BgpServicePeerGroupValidation")));
}

// Test setRouteFilterPolicy with IPv4 policy but peer group has IPv4 disabled
TEST_F(
    BgpServicePeerGroupValidationTestFixture,
    SetRouteFilterPolicyIpv4MismatchTest) {
  rib_policy::TRouteFilterPolicy tPolicy;
  tPolicy.key_type() = rib_policy::KeyType::PEER_GROUP_NAME;
  tPolicy.statements()->emplace(
      "test-peer-group-ipv6-enabled",
      createTRouteFilterStatementWithIngressAndEgressFilters(
          {kV4Prefix1},
          {kV4Prefix1},
          false,
          false,
          facebook::bgp::routing_policy::IPVersion::V4,
          std::nullopt));

  auto res = folly::coro::blockingWait(service_->co_setRouteFilterPolicy(
      std::make_unique<rib_policy::TRouteFilterPolicy>(std::move(tPolicy))));

  EXPECT_FALSE(*res->success());
  EXPECT_THAT(*res->err(), HasSubstr("IPV4_AFI_MISMATCH"));
  EXPECT_THAT(
      logHandler_->getMessageValues(),
      Contains(ContainsRegex("BgpServicePeerGroupValidation")));
}

// Test setRouteFilterPolicy with IPv6 policy but peer group has IPv6 disabled
TEST_F(
    BgpServicePeerGroupValidationTestFixture,
    SetRouteFilterPolicyIpv6MismatchTest) {
  rib_policy::TRouteFilterPolicy tPolicy;
  tPolicy.key_type() = rib_policy::KeyType::PEER_GROUP_NAME;
  tPolicy.statements()->emplace(
      "test-peer-group-ipv4-enabled",
      createTRouteFilterStatementWithIngressAndEgressFilters(
          {kV4Prefix1},
          {kV4Prefix1},
          false,
          false,
          std::nullopt,
          facebook::bgp::routing_policy::IPVersion::V6));

  auto res = folly::coro::blockingWait(service_->co_setRouteFilterPolicy(
      std::make_unique<rib_policy::TRouteFilterPolicy>(std::move(tPolicy))));

  EXPECT_FALSE(*res->success());
  EXPECT_THAT(*res->err(), HasSubstr("IPV6_AFI_MISMATCH"));
  EXPECT_THAT(
      logHandler_->getMessageValues(),
      Contains(ContainsRegex("BgpServicePeerGroupValidation")));
}

// test co_getRibPrefix
TEST_F(BgpServiceNullPtrTestFixture, GetRibPrefixNullPtrTest) {
  folly::coro::blockingWait(service_->co_getRibPrefix(nullptr));
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_getRibSubprefixes
TEST_F(BgpServiceNullPtrTestFixture, GetRibSubprefixesNullPtrTest) {
  folly::coro::blockingWait(service_->co_getRibSubprefixes(nullptr));
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_addNetwork
TEST_F(BgpServiceNullPtrTestFixture, AddNetworkNullPtrTest) {
  service_->addNetwork(nullptr, nullptr);
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test addNetworks
TEST_F(BgpServiceNullPtrTestFixture, AddNetworksNullPtrTest) {
  service_->addNetworks(nullptr);
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test delNetwork
TEST_F(BgpServiceNullPtrTestFixture, DelNetworkNullPtrTest) {
  service_->delNetwork(nullptr);
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test delNetworks
TEST_F(BgpServiceNullPtrTestFixture, DelNetworksNullPtrTest) {
  service_->delNetworks(nullptr);
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_getRibEntriesForCommunity
TEST_F(BgpServiceNullPtrTestFixture, GetRibEntriesForCommunityNullPtrTest) {
  folly::coro::blockingWait(
      service_->co_getRibEntriesForCommunity(afi_, nullptr));
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_getRibEntriesForCommunities
TEST_F(BgpServiceNullPtrTestFixture, GetRibEntriesForCommunitiesNullPtrTest) {
  folly::coro::blockingWait(
      service_->co_getRibEntriesForCommunities(afi_, nullptr));
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_getNexthopInfoForNexthop
TEST_F(BgpServiceNullPtrTestFixture, GetNexthopInfoForNexthopNullPtrTest) {
  folly::coro::blockingWait(service_->co_getNexthopInfoForNexthop(nullptr));
  EXPECT_THAT(
      logHandler_->getMessageValues(),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_getBgpNeighbors with nullptr
TEST_F(BgpServiceBaseNullPtrTestFixture, GetBgpNeighborsNullPtrTest) {
  auto result =
      folly::coro::blockingWait(service_->co_getBgpNeighbors(nullptr));
  EXPECT_TRUE(result->empty());
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_getBgpNeighborsFromSession with nullptr
TEST_F(
    BgpServiceBaseNullPtrTestFixture,
    GetBgpNeighborsFromSessionNullPtrTest) {
  auto result = folly::coro::blockingWait(
      service_->co_getBgpNeighborsFromSession(nullptr, nullptr));
  EXPECT_TRUE(result->empty());
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_getPrefilterReceivedNetworks
TEST_F(
    BgpServiceBaseNullPtrTestFixture,
    GetPrefilterReceivedNetworksNullPtrTest) {
  folly::coro::blockingWait(service_->co_getPrefilterReceivedNetworks(nullptr));
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_getPrefilterReceivedNetworks2
TEST_F(
    BgpServiceBaseNullPtrTestFixture,
    GetPrefilterReceivedNetworks2NullPtrTest) {
  folly::coro::blockingWait(
      service_->co_getPrefilterReceivedNetworks2(nullptr));
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_getPrefilterReceivedNetworksFromSession
TEST_F(
    BgpServiceBaseNullPtrTestFixture,
    GetPrefilterReceivedNetworksFromSessionNullPtrTest) {
  folly::coro::blockingWait(
      service_->co_getPrefilterReceivedNetworksFromSession(nullptr, nullptr));
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_getPrefilterReceivedNetworksFromSession2
TEST_F(
    BgpServiceBaseNullPtrTestFixture,
    GetPrefilterReceivedNetworksFromSession2NullPtrTest) {
  folly::coro::blockingWait(
      service_->co_getPrefilterReceivedNetworksFromSession2(nullptr, nullptr));
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_getPostfilterReceivedNetworks
TEST_F(
    BgpServiceBaseNullPtrTestFixture,
    GetPostfilterReceivedNetworksNullPtrTest) {
  folly::coro::blockingWait(
      service_->co_getPostfilterReceivedNetworks(nullptr));
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_getPostfilterReceivedNetworks2
TEST_F(
    BgpServiceBaseNullPtrTestFixture,
    GetPostfilterReceivedNetworks2NullPtrTest) {
  folly::coro::blockingWait(
      service_->co_getPostfilterReceivedNetworks2(nullptr));
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_getPostfilterReceivedNetworksFromSession
TEST_F(
    BgpServiceBaseNullPtrTestFixture,
    GetPostfilterReceivedNetworksFromSessionNullPtrTest) {
  folly::coro::blockingWait(
      service_->co_getPostfilterReceivedNetworksFromSession(nullptr, nullptr));
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_getPostfilterReceivedNetworksFromSession2
TEST_F(
    BgpServiceBaseNullPtrTestFixture,
    GetPostfilterReceivedNetworksFromSession2NullPtrTest) {
  folly::coro::blockingWait(
      service_->co_getPostfilterReceivedNetworksFromSession2(nullptr, nullptr));
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_getPrefilterAdvertisedNetworks
TEST_F(
    BgpServiceBaseNullPtrTestFixture,
    GetPrefilterAdvertisedNetworksNullPtrTest) {
  folly::coro::blockingWait(
      service_->co_getPrefilterAdvertisedNetworks(nullptr));
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_getPrefilterAdvertisedNetworks2
TEST_F(
    BgpServiceBaseNullPtrTestFixture,
    GetPrefilterAdvertisedNetworks2NullPtrTest) {
  folly::coro::blockingWait(
      service_->co_getPrefilterAdvertisedNetworks2(nullptr));
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_getPostfilterAdvertisedNetworks
TEST_F(
    BgpServiceBaseNullPtrTestFixture,
    GetPostfilterAdvertisedNetworksNullPtrTest) {
  folly::coro::blockingWait(
      service_->co_getPostfilterAdvertisedNetworks(nullptr));
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_getPostfilterAdvertisedNetworks2
TEST_F(
    BgpServiceBaseNullPtrTestFixture,
    GetPostfilterAdvertisedNetworks2NullPtrTest) {
  folly::coro::blockingWait(
      service_->co_getPostfilterAdvertisedNetworks2(nullptr));
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_getDryRunPostfilterReceivedNetworks
TEST_F(
    BgpServiceBaseNullPtrTestFixture,
    GetDryRunPostfilterReceivedNetworksNullPtrTest) {
  folly::coro::blockingWait(
      service_->co_getDryRunPostfilterReceivedNetworks(nullptr, nullptr));
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_getDryRunPostfilterAdvertisedNetworks
TEST_F(
    BgpServiceBaseNullPtrTestFixture,
    GetDryRunPostfilterAdvertisedNetworksNullPtrTest) {
  folly::coro::blockingWait(
      service_->co_getDryRunPostfilterAdvertisedNetworks(nullptr, nullptr));
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_getSubscriberNetworkInfo
TEST_F(BgpServiceBaseNullPtrTestFixture, GetSubscriberNetworkInfoNullPtrTest) {
  folly::coro::blockingWait(service_->co_getSubscriberNetworkInfo(1, nullptr));
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_shutdownSession
CO_TEST_F(BgpServiceBaseNullPtrTestFixture, ShutdownSessionNullPtrTest) {
  co_await service_->co_shutdownSession(nullptr);
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_restartSession
CO_TEST_F(BgpServiceBaseNullPtrTestFixture, RestartSessionNullPtrTest) {
  co_await service_->co_restartSession(nullptr);
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_startSession
CO_TEST_F(BgpServiceBaseNullPtrTestFixture, StartSessionNullPtrTest) {
  co_await service_->co_startSession(nullptr);
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_getAdvertisedNetworksFiltered
TEST_F(
    BgpServiceBaseNullPtrTestFixture,
    GetAdvertisedNetworksFilteredNullPtrTest) {
  folly::coro::blockingWait(
      service_->co_getAdvertisedNetworksFiltered(nullptr, nullptr));
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_getReceivedNetworks
TEST_F(BgpServiceBaseNullPtrTestFixture, GetReceivedNetworksNullPtrTest) {
  folly::coro::blockingWait(service_->co_getReceivedNetworks(nullptr));
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// test co_getAdvertisedNetworks
TEST_F(BgpServiceBaseNullPtrTestFixture, GetAdvertisedNetworksNullPtrTest) {
  folly::coro::blockingWait(service_->co_getAdvertisedNetworks(nullptr));
  EXPECT_THAT(
      std::move(logHandler_->getMessageValues()),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

TEST_F(BgpServiceTestFixture, GetMonitoredQueueSizesTest) {
  QueueSizeMapT ret;
  std::vector<std::string> paths = {};

  // Nothing is monitored yet
  {
    service_->getMonitoredQueueSizes(
        ret, std::make_unique<std::vector<std::string>>(paths));

    EXPECT_EQ(ret.size(), 0);
  }

  // Add a monitored queue
  {
    MonitoredModule module;
    MonitoredQueue<std::deque<int>> queue;
    module.monitorQueue("queue", queue, MonitorableQueueTrace::Direction::IN);
    watchdog_->monitorModule("module", module);

    service_->getMonitoredQueueSizes(
        ret, std::make_unique<std::vector<std::string>>(paths));

    EXPECT_EQ(ret.size(), 1);
  }
}

/**
 * Test thrift API call is allowed within allowed execution window of
 * active requests where requests are called one after the other
 */
TEST_F(BgpServiceTestFixture, ThriftSequentialAllowedTest) {
  BgpStats::initCounters();
  service_->setThriftProtection(true);

  /*
   * Thrift call 1
   */
  auto tmpThread1 = std::thread([&]() {
    EXPECT_TRUE(service_->continueExecution(true));
    service_->decrRequestsInExecution();
  });

  /*
   * Thrift call 2
   */
  EXPECT_TRUE(service_->continueExecution(true));
  service_->decrRequestsInExecution();

  /*
   * Thrift call 3
   */
  auto tmpThread2 = std::thread([&]() {
    EXPECT_TRUE(service_->continueExecution(true));
    service_->decrRequestsInExecution();
  });

  tmpThread1.join();
  tmpThread2.join();

  fb303::ThreadCachedServiceData::get()->publishStats();
  EXPECT_EQ(
      fb303::ThreadCachedServiceData::get()->getCounter(
          BgpStats::kThriftReject),
      0);
  EXPECT_EQ(
      fb303::ThreadCachedServiceData::get()->getCounter(
          BgpStats::kThriftSuspend),
      0);
}

/**
 * Test thrift API call is allowed within allowed execution window of
 * active requests where requests are overlapped
 */
TEST_F(BgpServiceTestFixture, ThriftParallelAllowedTest) {
  BgpStats::initCounters();
  folly::EventBase testEvb;
  service_->setThriftProtection(true);

  /*
   * Thrift call 1
   */
  service_->continueExecution(true);

  /*
   * Thrift call 2 without completing 1st call
   */
  auto tmpThread = std::thread([&]() {
    EXPECT_TRUE(service_->continueExecution(true));
    service_->decrRequestsInExecution();
  });

  /*
   * Sleep few ms to allow Thrift call 2 to kick in
   */
  testEvb.scheduleAt(
      [&]() noexcept { service_->decrRequestsInExecution(); },
      std::chrono::steady_clock::now() + std::chrono::milliseconds(2));

  testEvb.loop();
  tmpThread.join();

  /*
   * Thrift call 3 after completing call 1 and 2
   */
  EXPECT_TRUE(service_->continueExecution(true));

  fb303::ThreadCachedServiceData::get()->publishStats();
  EXPECT_EQ(
      fb303::ThreadCachedServiceData::get()->getCounter(
          BgpStats::kThriftReject),
      0);
  EXPECT_EQ(
      fb303::ThreadCachedServiceData::get()->getCounter(
          BgpStats::kThriftSuspend),
      0);
}

/**
 * Test thrift API call is allowed when called to not enforce dampening
 */
TEST_F(BgpServiceTestFixture, ThriftParallelNoDampeningTest) {
  BgpStats::initCounters();
  folly::EventBase testEvb;
  service_->setThriftProtection(true);

  /*
   * Thrift call 1
   */
  service_->continueExecution(true);

  /*
   * Thrift call 2 without completing 1st call
   */
  std::thread tmpThread;
  testEvb.scheduleAt(
      [&]() noexcept {
        tmpThread = std::thread([&]() {
          EXPECT_TRUE(service_->continueExecution(false));
          service_->decrRequestsInExecution();
        });
      },
      std::chrono::steady_clock::now() +
          std::chrono::milliseconds(service_->getAllowedBufferWindow()));

  /*
   * Sleep few ms to allow Thrift call 2 to kick in
   */
  testEvb.scheduleAt(
      [&]() noexcept { service_->decrRequestsInExecution(); },
      std::chrono::steady_clock::now() +
          std::chrono::milliseconds(service_->getAllowedBufferWindow()));

  testEvb.loop();
  tmpThread.join();

  /*
   * Thrift call 3 after completing call 1 and 2
   */
  testEvb.scheduleAt(
      [&]() { EXPECT_TRUE(service_->continueExecution(true)); },
      std::chrono::steady_clock::now() +
          std::chrono::milliseconds(service_->getIdleWindow_() + 1));
  testEvb.loop();

  fb303::ThreadCachedServiceData::get()->publishStats();
  EXPECT_EQ(
      fb303::ThreadCachedServiceData::get()->getCounter(
          BgpStats::kThriftReject),
      0);
  EXPECT_EQ(
      fb303::ThreadCachedServiceData::get()->getCounter(
          BgpStats::kThriftSuspend),
      0);
}

/**
 * Test thrift API call is suspended beyond allowed execution window of
 * active requests
 */
TEST_F(BgpServiceTestFixture, ThriftParallelDampeningFalseTest) {
  BgpStats::initCounters();
  folly::EventBase testEvb;

  service_->continueExecution(true);

  std::thread tmpThread;
  testEvb.scheduleAt(
      [&]() {
        tmpThread = std::thread(
            [&]() { EXPECT_TRUE(service_->continueExecution(true)); });
      },
      std::chrono::steady_clock::now() +
          std::chrono::milliseconds(service_->getAllowedBufferWindow()));

  testEvb.scheduleAt(
      [&]() { service_->decrRequestsInExecution(); },
      std::chrono::steady_clock::now() +
          std::chrono::milliseconds(service_->getAllowedBufferWindow()));
  testEvb.loop();
  tmpThread.join();

  fb303::ThreadCachedServiceData::get()->publishStats();
  EXPECT_EQ(
      fb303::ThreadCachedServiceData::get()->getCounter(
          BgpStats::kThriftReject),
      0);
  EXPECT_EQ(
      fb303::ThreadCachedServiceData::get()->getCounter(
          BgpStats::kThriftSuspend),
      0);
}

/**
 * Test thrift API call is suspended beyond allowed execution window of
 * active requests
 */
TEST_F(BgpServiceTestFixture, ThriftParallelDampeningTest) {
  BgpStats::initCounters();
  service_->setThriftProtection(true);

  folly::EventBase testEvb;
  service_->continueExecution(true);
  std::thread tmpThread;
  testEvb.scheduleAt(
      [&]() {
        tmpThread = std::thread(
            [&]() { EXPECT_TRUE(service_->continueExecution(true)); });
      },
      std::chrono::steady_clock::now() +
          std::chrono::milliseconds(service_->getAllowedBufferWindow()));

  testEvb.scheduleAt(
      [&]() { service_->decrRequestsInExecution(); },
      std::chrono::steady_clock::now() +
          std::chrono::milliseconds(service_->getAllowedBufferWindow()));

  testEvb.loop();
  tmpThread.join();

  fb303::ThreadCachedServiceData::get()->publishStats();
  EXPECT_EQ(
      fb303::ThreadCachedServiceData::get()->getCounter(
          BgpStats::kThriftReject),
      0);
  EXPECT_GT(
      fb303::ThreadCachedServiceData::get()->getCounter(
          BgpStats::kThriftSuspend),
      0);
}

/**
 * Test thrift API call is suspended beyond allowed execution window of
 * active requests, and eventually rejected as a result of suspended for
 * too long
 */
TEST_F(BgpServiceTestFixture, ThriftParallelRejectionTest) {
  BgpStats::initCounters();
  service_->setThriftProtection(true);
  service_->setRejectRequestWindow(1000);
  folly::EventBase testEvb;

  service_->continueExecution(true);

  std::thread tmpThread;
  testEvb.scheduleAt(
      [&]() {
        tmpThread = std::thread(
            [&]() { EXPECT_FALSE(service_->continueExecution(true)); });
      },
      std::chrono::steady_clock::now() +
          std::chrono::milliseconds(service_->getAllowedBufferWindow()));

  testEvb.scheduleAt(
      [&]() { service_->decrRequestsInExecution(); },
      std::chrono::steady_clock::now() +
          std::chrono::milliseconds(
              service_->getAllowedBufferWindow() +
              service_->getRejectRequestWindow()));

  testEvb.loop();
  tmpThread.join();

  fb303::ThreadCachedServiceData::get()->publishStats();
  EXPECT_GT(
      fb303::ThreadCachedServiceData::get()->getCounter(
          BgpStats::kThriftReject),
      0);
  EXPECT_GT(
      fb303::ThreadCachedServiceData::get()->getCounter(
          BgpStats::kThriftSuspend),
      0);
}

/**
 * Test that when setRouteFilterPolicy fails, the request is
 * still properly decremented using thrift protection
 */
TEST_F(BgpServiceTestFixture, ThriftProtectionFailureScenarioTest) {
  BgpStats::initCounters();
  service_->setThriftProtection(true);
  auto ribThread = rib_->runInThread();
  auto peerMgrThread = peerManager_->runInThread();
  auto sessionMgrThread = sessionMgr_->runInThread();

  rib_policy::TRouteFilterPolicy tPolicy;
  tPolicy.statements()->emplace(
      "failThriftProtection", createTRouteFilterStatement({}));
  folly::coro::blockingWait(service_->co_setRouteFilterPolicy(
      std::make_unique<rib_policy::TRouteFilterPolicy>(std::move(tPolicy))));

  rib_->stop();
  peerManager_->stop();
  sessionMgr_->stop();
  ribThread.join();
  peerMgrThread.join();
  sessionMgrThread.join();
  EXPECT_EQ(service_->numRequestsInExecution(), 0);
}

// Test co_getEntryStats endpoint
TEST_F(BgpServiceTestFixture, GetEntryStatsTest) {
  auto ribThread = rib_->runInThread();

  auto stats = folly::coro::blockingWait(service_->co_getEntryStats());
  EXPECT_EQ(*stats->total_ucast_routes(), 0);
  EXPECT_EQ(*stats->total_rib_paths(), 0);
  EXPECT_EQ(*stats->total_adj_ribs(), 0);
  EXPECT_EQ(*stats->total_originated_routes(), 0);
  EXPECT_EQ(*stats->total_shadow_rib_entries(), 0);
  EXPECT_EQ(*stats->total_netlink_wrapper_interfaces(), 0);
  rib_->stop();
  ribThread.join();
}

// Test co_getPolicyStats dispatches to PeerManager evb and returns empty stats
TEST_F(BgpServiceTestFixture, GetPolicyStatsTest) {
  auto peerMgrThread = peerManager_->runInThread();

  auto stats = folly::coro::blockingWait(service_->co_getPolicyStats());
  EXPECT_TRUE(stats->policy_statement_stats()->empty());

  peerManager_->stop();
  peerMgrThread.join();
}

// Test getNexthopInfoForNexthop with invalid nexthop
TEST_F(BgpServiceTestFixture, GetNexthopInfoForNexthopInvalidTest) {
  auto ribThread = rib_->runInThread();

  auto nexthopInfo =
      folly::coro::blockingWait(service_->co_getNexthopInfoForNexthop(
          std::make_unique<std::string>("10.0.0.1")));

  EXPECT_FALSE(
      apache::thrift::is_non_optional_field_set_manually_or_by_serializer(
          nexthopInfo->next_hop()));

  rib_->stop();
  ribThread.join();
}

} // namespace facebook::bgp
