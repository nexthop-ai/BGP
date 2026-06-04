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

#include <neteng/fboss/bgp/cpp/common/Consts.h>
#include <neteng/fboss/bgp/cpp/lib/fibers/Utils.h>

extern "C" {
#include <linux/in6.h>
#include <netinet/ip.h>
}

namespace facebook::nettools::bgplib {

using folly::SocketOptionKey;

class GetSockOptionsTest : public ::testing::Test {
 protected:
  static bool
  hasOption(const folly::SocketOptionMap& options, int level, int optname) {
    return options.find({level, optname}) != options.end();
  }

  static void expectOption(
      const folly::SocketOptionMap& options,
      int level,
      int optname,
      int expectedValue) {
    auto it = options.find({level, optname});
    ASSERT_NE(it, options.end())
        << "Option level=" << level << " optname=" << optname << " not found";
    EXPECT_EQ(it->second, expectedValue);
  }
};

// --- getSockOptions (base options: TOS/TCLASS, TCP_MAXSEG) ---

TEST_F(GetSockOptionsTest, IPv4SetsTosOnly) {
  auto options = getSockOptions(/*isV6=*/false, /*disableJumboFrame=*/false);

  expectOption(options, IPPROTO_IP, IP_TOS, IPTOS_CLASS_CS6);
  EXPECT_FALSE(hasOption(options, IPPROTO_IP, IP_TTL));
  EXPECT_FALSE(hasOption(options, IPPROTO_IP, IP_MINTTL));
  EXPECT_FALSE(hasOption(options, IPPROTO_TCP, TCP_MAXSEG));
}

TEST_F(GetSockOptionsTest, IPv6SetsTclassOnly) {
  auto options = getSockOptions(/*isV6=*/true, /*disableJumboFrame=*/false);

  expectOption(options, IPPROTO_IPV6, IPV6_TCLASS, IPTOS_CLASS_CS6);
  EXPECT_FALSE(hasOption(options, IPPROTO_IPV6, IPV6_UNICAST_HOPS));
  EXPECT_FALSE(hasOption(options, IPPROTO_IPV6, IPV6_MINHOPCOUNT));
  EXPECT_FALSE(hasOption(options, IPPROTO_TCP, TCP_MAXSEG));
}

TEST_F(GetSockOptionsTest, IPv4JumboFrameDisabled) {
  auto options = getSockOptions(/*isV6=*/false, /*disableJumboFrame=*/true);

  expectOption(options, IPPROTO_IP, IP_TOS, IPTOS_CLASS_CS6);
  expectOption(options, IPPROTO_TCP, TCP_MAXSEG, 1412);
}

TEST_F(GetSockOptionsTest, IPv6JumboFrameDisabled) {
  auto options = getSockOptions(/*isV6=*/true, /*disableJumboFrame=*/true);

  expectOption(options, IPPROTO_IPV6, IPV6_TCLASS, IPTOS_CLASS_CS6);
  expectOption(options, IPPROTO_TCP, TCP_MAXSEG, 1392);
}

// --- getGtsmSockOptions (GTSM-only options) ---

TEST_F(GetSockOptionsTest, GtsmNulloptReturnsEmpty) {
  auto options = getGtsmSockOptions(/*isV6=*/false, std::nullopt);
  EXPECT_TRUE(options.empty());
}

TEST_F(GetSockOptionsTest, IPv4GtsmMinHops) {
  auto options = getGtsmSockOptions(
      /*isV6=*/false, /*ttlSecurityHops=*/bgp::kMinTtlSecurityHops);

  EXPECT_EQ(options.size(), 2);
  expectOption(options, IPPROTO_IP, IP_TTL, bgp::kMaxTtlSecurityHops);
  expectOption(options, IPPROTO_IP, IP_MINTTL, bgp::kMaxTtlSecurityHops);
  // Must not contain base options
  EXPECT_FALSE(hasOption(options, IPPROTO_IP, IP_TOS));
  EXPECT_FALSE(hasOption(options, IPPROTO_TCP, TCP_MAXSEG));
}

TEST_F(GetSockOptionsTest, IPv4GtsmMidRangeHops) {
  auto options = getGtsmSockOptions(/*isV6=*/false, /*ttlSecurityHops=*/64);

  expectOption(options, IPPROTO_IP, IP_TTL, bgp::kMaxTtlSecurityHops);
  expectOption(options, IPPROTO_IP, IP_MINTTL, 192);
}

TEST_F(GetSockOptionsTest, IPv4GtsmMaxHops) {
  auto options = getGtsmSockOptions(
      /*isV6=*/false, /*ttlSecurityHops=*/bgp::kMaxTtlSecurityHops);

  expectOption(options, IPPROTO_IP, IP_TTL, bgp::kMaxTtlSecurityHops);
  expectOption(options, IPPROTO_IP, IP_MINTTL, bgp::kMinTtlSecurityHops);
}

TEST_F(GetSockOptionsTest, IPv6GtsmMinHops) {
  auto options = getGtsmSockOptions(
      /*isV6=*/true, /*ttlSecurityHops=*/bgp::kMinTtlSecurityHops);

  EXPECT_EQ(options.size(), 2);
  expectOption(
      options, IPPROTO_IPV6, IPV6_UNICAST_HOPS, bgp::kMaxTtlSecurityHops);
  expectOption(
      options, IPPROTO_IPV6, IPV6_MINHOPCOUNT, bgp::kMaxTtlSecurityHops);
  // Must not contain base options
  EXPECT_FALSE(hasOption(options, IPPROTO_IPV6, IPV6_TCLASS));
  EXPECT_FALSE(hasOption(options, IPPROTO_TCP, TCP_MAXSEG));
}

TEST_F(GetSockOptionsTest, IPv6GtsmMidRangeHops) {
  auto options = getGtsmSockOptions(/*isV6=*/true, /*ttlSecurityHops=*/64);

  expectOption(
      options, IPPROTO_IPV6, IPV6_UNICAST_HOPS, bgp::kMaxTtlSecurityHops);
  expectOption(options, IPPROTO_IPV6, IPV6_MINHOPCOUNT, 192);
}

TEST_F(GetSockOptionsTest, IPv6GtsmMaxHops) {
  auto options = getGtsmSockOptions(
      /*isV6=*/true, /*ttlSecurityHops=*/bgp::kMaxTtlSecurityHops);

  expectOption(
      options, IPPROTO_IPV6, IPV6_UNICAST_HOPS, bgp::kMaxTtlSecurityHops);
  expectOption(
      options, IPPROTO_IPV6, IPV6_MINHOPCOUNT, bgp::kMinTtlSecurityHops);
}

} // namespace facebook::nettools::bgplib
