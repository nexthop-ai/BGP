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

#include "neteng/fboss/bgp/cpp/lib/tests/FiberBgpPeerManagerTestUtils.h"

void waitTillSessionsComeUp(
    folly::fibers::FiberManager& fm,
    std::shared_ptr<TestFiberBgpPeerManager> peerMgr,
    const std::unordered_set<BgpPeerId>& peerSet,
    const std::chrono::seconds& timeout) {
  auto future = fm.addTaskFuture([&]() mutable {
    auto start = std::chrono::steady_clock::now();
    // busy waits till sessions come up
    while (true) {
      auto elapsed = std::chrono::steady_clock::now() - start;
      if (elapsed > timeout) {
        XLOG(ERR, "waitTillSessionsComeUp timeout");
        break;
      }
      // TODO: remove busy waiting by forking notify queue
      facebook::nettools::bgplib::fiberSleepFor(std::chrono::milliseconds(1));
      // confirm that sessions come up
      bool allSessionsAreUp = true;
      for (const auto& peerId : peerSet) {
        if (!peerMgr->isPeerUp(peerId)) {
          allSessionsAreUp = false;
        }
      }
      if (allSessionsAreUp) {
        break;
      }
    }
  });
  // wait till session comes up
  std::move(future).get();
}

void waitTillSessionsGoDown(
    folly::fibers::FiberManager& fm,
    std::shared_ptr<TestFiberBgpPeerManager> peerMgr,
    const std::unordered_set<BgpPeerId>& peerSet,
    const std::chrono::seconds& timeout) {
  auto future = fm.addTaskFuture([&]() mutable {
    auto start = std::chrono::steady_clock::now();
    // busy waits till sessions come up
    while (true) {
      auto elapsed = std::chrono::steady_clock::now() - start;
      if (elapsed > timeout) {
        XLOG(ERR, "waitTillSessionsGoDown timeout");
        break;
      }
      // TODO: remove busy waiting by forking notify queue
      facebook::nettools::bgplib::fiberSleepFor(std::chrono::milliseconds(1));
      // confirm that sessions come up
      bool allSessionsAreDown = true;
      for (const auto& peerId : peerSet) {
        if (peerMgr->isPeerUp(peerId)) {
          allSessionsAreDown = false;
        }
      }
      if (allSessionsAreDown) {
        break;
      }
    }
  });
  // wait till session goes down
  std::move(future).get();
}
