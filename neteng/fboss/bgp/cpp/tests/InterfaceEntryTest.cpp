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

#include <folly/IPAddress.h>
#include <gtest/gtest.h>

#include "neteng/fboss/bgp/cpp/nexthopTracker/InterfaceEntry.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

using namespace ::testing;
namespace facebook::bgp {

/**
 * Verify removing and adding addresses are reflected in the interface entry
 */
TEST(InterfaceEntryTest, UpdateAddrTest) {
  auto ifName = "eth0";
  InterfaceEntry ifEntry{ifName};

  EXPECT_EQ(ifEntry.getIfName(), ifName);

  // Initially no IPs
  auto ipReachability = ifEntry.getIpReachabilityMap();
  EXPECT_EQ(ipReachability.size(), 0);

  // add a new Address (/32 expands to 1 IP)
  auto isUpdated = ifEntry.updateAddr(kV4Prefix8_1Slash32, true);

  EXPECT_TRUE(isUpdated);
  ipReachability = ifEntry.getIpReachabilityMap();
  EXPECT_EQ(ipReachability.size(), 1);

  // Check the IP is present
  auto ip = folly::IPAddress("9.0.0.1");
  EXPECT_TRUE(ipReachability.contains(ip));
  EXPECT_FALSE(ipReachability.at(ip)); // Default reachability is false

  // try to add the same address
  isUpdated = ifEntry.updateAddr(kV4Prefix8_1Slash32, true);

  EXPECT_FALSE(isUpdated);
  ipReachability = ifEntry.getIpReachabilityMap();
  EXPECT_EQ(ipReachability.size(), 1);

  // Add a /31 prefix (2 IPs: 9.0.0.2 and 9.0.0.3)
  isUpdated = ifEntry.updateAddr(kV4Prefix8_2Slash31, true);

  EXPECT_TRUE(isUpdated);
  ipReachability = ifEntry.getIpReachabilityMap();
  EXPECT_EQ(ipReachability.size(), 3); // 1 from /32 + 2 from /31

  auto ip2 = folly::IPAddress("9.0.0.2");
  auto ip3 = folly::IPAddress("9.0.0.3");
  EXPECT_TRUE(ipReachability.contains(ip2));
  EXPECT_TRUE(ipReachability.contains(ip3));

  // try to remove an address not present
  isUpdated = ifEntry.updateAddr(kV4Prefix8_4Slash32, false);

  EXPECT_FALSE(isUpdated);
  ipReachability = ifEntry.getIpReachabilityMap();
  EXPECT_EQ(ipReachability.size(), 3);

  // remove the /32 address that is present
  isUpdated = ifEntry.updateAddr(kV4Prefix8_1Slash32, false);

  EXPECT_TRUE(isUpdated);
  ipReachability = ifEntry.getIpReachabilityMap();
  EXPECT_EQ(ipReachability.size(), 2); // Only /31 IPs remain
  EXPECT_FALSE(ipReachability.contains(ip));
  EXPECT_TRUE(ipReachability.contains(ip2));
  EXPECT_TRUE(ipReachability.contains(ip3));
}

/**
 * Verify updating the ifindex correctly changes the entry
 * and its return isUpdated is returned correctly
 */
TEST(InterfaceEntryTest, UpdateIfIndexTest) {
  auto ifName = "eth0";
  InterfaceEntry ifEntry{ifName};

  EXPECT_EQ(ifEntry.getIfName(), ifName);

  // update ifIndex
  auto isUpdate = ifEntry.updateIfIndex(10);
  EXPECT_EQ(ifEntry.getIfIndex(), 10);
  EXPECT_TRUE(isUpdate);

  // no update
  isUpdate = ifEntry.updateIfIndex(10);
  EXPECT_EQ(ifEntry.getIfIndex(), 10);
  EXPECT_FALSE(isUpdate);
}

/**
 * Verify updating reachability for specific IPs works correctly
 */
TEST(InterfaceEntryTest, UpdateReachabilityTest) {
  auto ifName = "eth0";
  InterfaceEntry ifEntry{ifName};

  EXPECT_EQ(ifEntry.getIfName(), ifName);

  // Add a /31 prefix (2 IPs: 9.0.0.0 and 9.0.0.1)
  auto isUpdate = ifEntry.updateAddr(kV4Prefix2Slash31, true);
  EXPECT_TRUE(isUpdate);

  auto ip1 = folly::IPAddress("9.0.0.0");
  auto ip2 = folly::IPAddress("9.0.0.1");

  // Both IPs should exist with default reachability = false
  EXPECT_FALSE(ifEntry.isReachable(ip1));
  EXPECT_FALSE(ifEntry.isReachable(ip2));

  // Update reachability for first IP
  isUpdate = ifEntry.updateReachability(ip1, true);
  EXPECT_TRUE(isUpdate);
  EXPECT_TRUE(ifEntry.isReachable(ip1));
  EXPECT_FALSE(ifEntry.isReachable(ip2)); // Second IP unchanged

  // Update same reachability again - should return false (no change)
  isUpdate = ifEntry.updateReachability(ip1, true);
  EXPECT_FALSE(isUpdate);
  EXPECT_TRUE(ifEntry.isReachable(ip1));

  // Update reachability for IP not in the interface - should return false
  auto unknownIp = folly::IPAddress("10.0.0.1");
  isUpdate = ifEntry.updateReachability(unknownIp, true);
  EXPECT_FALSE(isUpdate);
  EXPECT_FALSE(ifEntry.isReachable(unknownIp));
}

/**
 * Verify updating reachability for all IPs works correctly
 */
TEST(InterfaceEntryTest, UpdateReachabilityForAllIPsTest) {
  auto ifName = "eth0";
  InterfaceEntry ifEntry{ifName};

  // Add a /31 prefix (2 IPs)
  ifEntry.updateAddr(kV4Prefix2Slash31, true);

  auto ip1 = folly::IPAddress("9.0.0.0");
  auto ip2 = folly::IPAddress("9.0.0.1");

  // Initially both should be unreachable
  EXPECT_FALSE(ifEntry.isReachable(ip1));
  EXPECT_FALSE(ifEntry.isReachable(ip2));

  // Update all IPs to reachable
  auto isUpdate = ifEntry.updateReachabilityForAllIPs(true);
  EXPECT_TRUE(isUpdate);
  EXPECT_TRUE(ifEntry.isReachable(ip1));
  EXPECT_TRUE(ifEntry.isReachable(ip2));

  // Update all again with same value - should return false
  isUpdate = ifEntry.updateReachabilityForAllIPs(true);
  EXPECT_FALSE(isUpdate);

  // Update all IPs to unreachable
  isUpdate = ifEntry.updateReachabilityForAllIPs(false);
  EXPECT_TRUE(isUpdate);
  EXPECT_FALSE(ifEntry.isReachable(ip1));
  EXPECT_FALSE(ifEntry.isReachable(ip2));
}

} // namespace facebook::bgp
