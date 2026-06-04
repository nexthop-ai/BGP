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

#include "Utils.h"

#include <chrono>

#include <folly/Random.h>
#include <folly/fibers/Baton.h>
#include <folly/fibers/EventBaseLoopController.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/EventBase.h>

#include "neteng/fboss/bgp/cpp/common/Consts.h"

extern "C" {
#include <linux/in6.h> // IPV6_MINHOPCOUNT
#include <netinet/ip.h>
}

namespace facebook {
namespace nettools {
namespace bgplib {

int64_t getCurrentTimeMs() {
  auto now = std::chrono::system_clock::now();
  auto value = std::chrono::time_point_cast<std::chrono::milliseconds>(now)
                   .time_since_epoch()
                   .count();
  return value;
}

folly::EventBase* getFiberEventBase() {
  CHECK(folly::fibers::onFiber())
      << "Attempt to get fiber event base while not on fiber";

  auto& fm = folly::fibers::FiberManager::getFiberManager();
  auto& lc =
      static_cast<folly::fibers::EventBaseLoopController&>(fm.loopController());

  return &lc.getEventBase()->getEventBase();
}

void fiberSleepFor(std::chrono::milliseconds duration) {
  CHECK(folly::fibers::onFiber()) << "Attempt to sleep while not on fiber";
  auto evb = getFiberEventBase();
  folly::fibers::Baton bt;
  auto timeout = folly::AsyncTimeout::schedule(
      duration, *evb, [&bt]() noexcept { bt.post(); });
  bt.wait();
}

folly::fibers::FiberManager::Options getFiberManagerOptions(
    const uint32_t stackSizeKB) {
  folly::fibers::FiberManager::Options opts;
  opts.stackSize = stackSizeKB * 1024; // double the default size (16KB)
  return opts;
}

std::string getAddressStr(folly::SocketAddress addr) {
  auto ipAddr = addr.getIPAddress();
  if (addr.isIPv4Mapped()) {
    ipAddr = folly::IPAddress::createIPv4(ipAddr);
  }
  return ipAddr.str();
}

long generateJitter(long timeMs, long jitterPercent, long capMs) {
  if (timeMs <= 0) {
    return 0;
  }
  auto jitterMax = std::min<long>(capMs, timeMs * jitterPercent / 100);
  if (jitterMax <= 0) {
    return 0;
  }
  return jitterMax - (folly::Random::rand32() % (2 * jitterMax));
}

folly::SocketOptionMap getSockOptions(bool isV6, bool disableJumboFrame) {
  folly::SocketOptionMap optionsV4{
      {{IPPROTO_IP, IP_TOS}, IPTOS_CLASS_CS6},
  };
  if (disableJumboFrame) {
    optionsV4.insert({{IPPROTO_TCP, TCP_MAXSEG}, 1412});
  }

  folly::SocketOptionMap optionsV6{
      {{IPPROTO_IPV6, IPV6_TCLASS}, IPTOS_CLASS_CS6},
  };
  if (disableJumboFrame) {
    optionsV6.insert({{IPPROTO_TCP, TCP_MAXSEG}, 1392});
  }

  return (isV6 ? optionsV6 : optionsV4);
}

folly::SocketOptionMap getGtsmSockOptions(
    bool isV6,
    std::optional<int32_t> ttlSecurityHops) {
  folly::SocketOptionMap options;
  if (!ttlSecurityHops.has_value()) {
    return options;
  }
  int hops = ttlSecurityHops.value();
  int minTtl = bgp::kMaxTtlSecurityHops + 1 - hops;
  if (isV6) {
    options.insert(
        {{IPPROTO_IPV6, IPV6_UNICAST_HOPS}, bgp::kMaxTtlSecurityHops});
    options.insert({{IPPROTO_IPV6, IPV6_MINHOPCOUNT}, minTtl});
  } else {
    options.insert({{IPPROTO_IP, IP_TTL}, bgp::kMaxTtlSecurityHops});
    options.insert({{IPPROTO_IP, IP_MINTTL}, minTtl});
  }
  return options;
}

} // namespace bgplib
} // namespace nettools
} // namespace facebook
