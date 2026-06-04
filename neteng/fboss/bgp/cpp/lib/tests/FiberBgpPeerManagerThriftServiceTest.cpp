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

#include "neteng/fboss/bgp/cpp/lib/fibers/FiberBgpPeerManager.h"
#include "neteng/fboss/bgp/cpp/lib/tests/FiberBgpPeerManagerTestUtils.h"

namespace facebook::nettools::bgplib {
using namespace facebook::bgp;
using facebook::bgp::thrift::BgpNetwork;

class MockFiberBgpPeerManager : public FiberBgpPeerManager {
 public:
  using FiberBgpPeerManager::FiberBgpPeerManager;

  using retType = folly::Expected<folly::Unit, ErrorCode>;

  MOCK_METHOD(retType, startPeer, (const folly::IPAddress&));
  MOCK_METHOD(retType, stopPeer, (const folly::IPAddress&, bool));
  MOCK_METHOD(retType, shutdownPeer, (const folly::IPAddress&, bool));

  MOCK_METHOD(retType, startDynamicPeer, (const folly::CIDRNetwork&));
  MOCK_METHOD(
      retType,
      stopDynamicPeerWithGracefulRestart,
      (const folly::CIDRNetwork&));
  MOCK_METHOD(retType, shutdownDynamicPeer, (const folly::CIDRNetwork&));
};

class ThriftServiceTestFixture : public ::testing::Test {
 public:
  ThriftServiceTestFixture() : fm_(folly::fibers::getFiberManager(evb_, {})) {}

  void SetUp() override {
    BgpGlobalConfig bgpGlobalConfig(
        100, // localAsn
        kR1Lo1, // routerId 127.1.0.1
        kR1Lo1, // clusterId 127.1.0.1
        kDefaultHoldTime, // holdTime
        std::nullopt, // listenAddr
        kDefaultGrRestartTime, // grRestartTime
        std::unordered_map<folly::CIDRNetwork, BgpNetwork>(), // networksV4
        std::unordered_map<folly::CIDRNetwork, BgpNetwork>() // networksV6
    );
    mockSessionMgr_ = std::make_unique<MockFiberBgpPeerManager>(
        bgpGlobalConfig, fm_, evb_, true, true);

    sessionMgrThread_ = std::thread([&]() { evb_.loopForever(); });

    evb_.waitUntilRunning();
  }

  void TearDown() override {
    evb_.runInEventBaseThreadAndWait([&]() { evb_.terminateLoopSoon(); });
    mockSessionMgr_.reset();
    sessionMgrThread_.join();
  }

  std::unique_ptr<MockFiberBgpPeerManager> mockSessionMgr_;
  folly::EventBase evb_;
  folly::fibers::FiberManager& fm_;

  std::thread sessionMgrThread_;
};

TEST_F(ThriftServiceTestFixture, StartStaticPeerSessionTest) {
  folly::IPAddress peerAddr;

  EXPECT_CALL(*mockSessionMgr_, startPeer(peerAddr)).Times(1);

  mockSessionMgr_->startSession(peerAddr);
}

TEST_F(ThriftServiceTestFixture, RestartStaticPeerSessionTest) {
  folly::IPAddress peerAddr;

  EXPECT_CALL(*mockSessionMgr_, stopPeer(peerAddr, true)).Times(1);

  mockSessionMgr_->restartSession(peerAddr);
}

TEST_F(ThriftServiceTestFixture, ShutdownStaticPeerSessionTest) {
  folly::IPAddress peerAddr;

  EXPECT_CALL(*mockSessionMgr_, shutdownPeer(peerAddr, ::testing::_)).Times(1);

  mockSessionMgr_->shutdownSession(peerAddr);
}

TEST_F(ThriftServiceTestFixture, StartDynamicPeerSessionTest) {
  folly::CIDRNetwork peerPrefix;

  EXPECT_CALL(*mockSessionMgr_, startDynamicPeer(peerPrefix)).Times(1);

  mockSessionMgr_->startSession(peerPrefix);
}

TEST_F(ThriftServiceTestFixture, RestartDynamicPeerSessionTest) {
  folly::CIDRNetwork peerPrefix;

  EXPECT_CALL(*mockSessionMgr_, stopDynamicPeerWithGracefulRestart(peerPrefix))
      .Times(1);

  mockSessionMgr_->restartSession(peerPrefix);
}

TEST_F(ThriftServiceTestFixture, ShutdownDynamicPeerSessionTest) {
  folly::CIDRNetwork peerPrefix;

  EXPECT_CALL(*mockSessionMgr_, shutdownDynamicPeer(peerPrefix)).Times(1);

  mockSessionMgr_->shutdownSession(peerPrefix);
}

} // namespace facebook::nettools::bgplib
