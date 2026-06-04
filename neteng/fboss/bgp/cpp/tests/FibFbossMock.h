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

/**
 * @file FibFbossMock.h
 * @brief FibFboss mock class and classes for mocking the agent behavior.
 * */

#pragma once

#include <fboss/agent/if/gen-cpp2/FbossCtrlAsyncClient.h>
#include <fboss/agent/if/gen-cpp2/ctrl_types.h>
#include <folly/coro/BlockingWait.h>
#include <gmock/gmock.h>
#include "fboss/agent/if/gen-cpp2/FbossCtrl.h"
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/rib/FibFboss.h"
#include "neteng/fboss/bgp/cpp/rib/FibProgrammingHolddown.h"
#include "servicerouter/client/cpp2/ServiceRouter.h"
#include "servicerouter/client/cpp2/mocks/MockSRClientFactory.h"

namespace facebook::bgp {

/**
 * @class MockProgrammingHistory
 * @brief Mock implementation of ProgrammingHistory interface.
 *
 * This class provides mock implementations of the ProgrammingHistory interface,
 * allowing for testing and simulation of the FibFboss class's interactions with
 * programming history behavior.
 */
class MockProgrammingHistory : public ProgrammingHistory {
 public:
  /**
   * @brief Mock method for marking a programming success.
   */
  MOCK_METHOD((void), markProgrammingSuccess, (), (override));

  /**
   * @brief Mock method for marking a programming failure.
   */
  MOCK_METHOD((void), markProgrammingFail, (), (override));

  /**
   * @brief Mock method for returning # of failures.
   */
  MOCK_CONST_METHOD0(getRecentFailureCount, uint32_t());
};

/**
 * @class MockHoldDownState
 * @brief This class provides mock implementations of the HoldDown state class,
 * alllowing for testing and simulation of FibFboss class's interfaction with
 * hold down state behavior.
 */
class MockHoldDownState : public HoldDownState {
 public:
  /**
   * @brief Mock method for clearing the hold down state.
   */
  MOCK_METHOD((bool), clearHoldDownState, (), (override));

  /**
   * @brief Mock method for setting the hold down state.
   */
  MOCK_METHOD((bool), setHoldDownState, (uint32_t), (override));
};

/**
 * @class MockFbossCtrlSvIf
 * @brief Mock implementation of FbossCtrlSvIf interface.
 *
 * This class provides mock implementations of the Fboss agent iterface
 * , allowing for testing and simulation of the BGP's interaction with it.
 */
class MockFbossCtrlSvIf : public facebook::fboss::FbossCtrlSvIf {
 public:
  /**
   * @brief Mock method for synchronizing the FIB (Forwarding Information Base).
   *
   * @param p_clientId Client ID making the request.
   * @param p_routes Vector of unicast routes to synchronize.
   * @return folly::coro::Task<void> Task representing the asynchronous
   * operation.
   */
  MOCK_METHOD(
      (folly::coro::Task<void>),
      co_syncFib,
      (int16_t p_clientId,
       std::unique_ptr<::std::vector<::facebook::fboss::UnicastRoute>>
           p_routes),
      (override));

  /**
   * @brief Mock method for deleting unicast routes.
   *
   * @param p_clientId Client ID making the request.
   * @param p_routes Vector of IP prefixes to delete.
   * @return folly::coro::Task<void> Task representing the asynchronous
   * operation.
   */
  MOCK_METHOD(
      (folly::coro::Task<void>),
      co_deleteUnicastRoutes,
      (int16_t p_clientId,
       std::unique_ptr<::std::vector<::facebook::fboss::IpPrefix>> p_routes),
      (override));

  /**
   * @brief Mock method for adding unicast routes.
   *
   * @param p_clientId Client ID making the request.
   * @param p_routes Vector of unicast routes to add.
   * @return folly::coro::Task<void> Task representing the asynchronous
   * operation.
   */
  MOCK_METHOD(
      (folly::coro::Task<void>),
      co_addUnicastRoutes,
      (int16_t p_clientId,
       std::unique_ptr<::std::vector<::facebook::fboss::UnicastRoute>>
           p_routes),
      (override));

  /**
   * @brief Mock method for getting the alive since timestamp.
   *
   * @return folly::coro::Task<::std::int64_t> Task representing the
   * asynchronous operation.
   */
  MOCK_METHOD(
      (folly::coro::Task<::std::int64_t>),
      co_aliveSince,
      (),
      (override));

  /**
   * @brief Mock method for getting the switch run state.
   *
   * @return folly::coro::Task<::facebook::fboss::SwitchRunState> Task
   * representing the asynchronous operation.
   */
  MOCK_METHOD(
      (folly::coro::Task<::facebook::fboss::SwitchRunState>),
      co_getSwitchRunState,
      (),
      (override));
};

/**
 * @class FibFbossMock
 * @brief Mock implementation of FibFboss class.
 *
 * This class provides a mock implementation of the FibFboss class, allowing for
 * testing and simulation of the FBOSS agent interactions.
 */
class FibFbossMock : public facebook::bgp::FibFboss {
 public:
  /**
   * @brief Constructor for FibFbossMock class.
   *
   * Create mock objects for programming history and hold down state.
   *
   * @param evb Event base for asynchronous operations.
   * @param asyncScope Asynchronous scope for coroutines.
   * @param toRibQ Message queue for sending messages to RIB.
   */
  FibFbossMock(
      folly::EventBase* evb,
      folly::coro::CancellableAsyncScope& asyncScope,
      Fib::FibMessageQueue& toRibQ)
      : FibFboss(evb, asyncScope, toRibQ) {
    programmingHistory_ = std::make_unique<MockProgrammingHistory>();
    holdDownState_ = std::make_unique<MockHoldDownState>();
  }

  /**
   * @brief Creates a mock FBOSS agent client.
   *
   *
   * @return std::unique_ptr<apache::thrift::Client<fboss::FbossCtrl>>
   * Returns a unique pointer to a mocked FBOSS agent client.
   */
  std::unique_ptr<apache::thrift::Client<fboss::FbossCtrl>>
  createFibFbossClientHelper() {
    auto& factory = servicerouter::cpp2::getClientFactory();
    return factory.getSRClientUnique<fboss::FbossCtrl>("agent");
  }

  /**
   * @brief Connects to the FBOSS agent.
   *
   * Creates a mocked version of FbossCtrlAsyncClient and a batch.
   */
  void connectAgent() override {
    client_ = createFibFbossClientHelper();

    // Create a batch.
    batch_ = std::make_unique<Batch>();
  }

  /**
   * @brief Creates a new instance of FibFbossMock.
   *
   * Creates a new instance of FibFbossMock and connects to the FBOSS agent.
   *
   * @param evb Event base for asynchronous operations.
   * @param asyncScope Asynchronous scope for coroutines.
   * @param toRibQ Message queue for sending messages to RIB.
   * @return std::unique_ptr<FibFbossMock>
   */
  static std::unique_ptr<FibFbossMock> createFibFboss(
      folly::EventBase* evb,
      folly::coro::CancellableAsyncScope& asyncScope,
      Fib::FibMessageQueue& toRibQ) {
    auto fib = std::unique_ptr<FibFbossMock>(
        new FibFbossMock(evb, asyncScope, toRibQ));

    fib->connectAgent();
    return fib;
  }
};

} // namespace facebook::bgp
