/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * Vendored from github.com/facebook/openr/openr/common/NetworkUtil.h, with
 * adaptations for OSS:
 *   1. Removed `#include <openr/common/Constants.h>` (unused by BGP++).
 *   2. Removed `#include <openr/if/gen-cpp2/OpenrCtrl_types.h>` and
 *      `#include <openr/if/gen-cpp2/Types_types.h>` (only needed for
 * OpenrError).
 *   3. Replaced `throw thrift::OpenrError(...)` with `throw
 * std::runtime_error(...)` — same semantics for callers that catch
 * std::exception.
 *
 * See public_tld/openr/PROVENANCE.md for removal trigger.
 */

#pragma once

#include <stdexcept>

#include <fmt/core.h>
#include <folly/Format.h>
#include <folly/IPAddress.h>
#include <thrift/lib/cpp/util/EnumUtils.h>
#include <thrift/lib/cpp2/Thrift.h>

#include <openr/if/gen-cpp2/Network_types.h>

namespace std {

template <>
struct hash<openr::thrift::IpPrefix> {
  size_t operator()(openr::thrift::IpPrefix const&) const;
};

template <>
struct hash<openr::thrift::BinaryAddress> {
  size_t operator()(openr::thrift::BinaryAddress const&) const;
};

template <>
struct hash<openr::thrift::MplsAction> {
  size_t operator()(openr::thrift::MplsAction const&) const;
};

template <>
struct hash<openr::thrift::NextHopThrift> {
  size_t operator()(openr::thrift::NextHopThrift const&) const;
};

template <>
struct hash<openr::thrift::UnicastRoute> {
  size_t operator()(openr::thrift::UnicastRoute const&) const;
};

} // namespace std

namespace openr {

template <class IPAddressVx>
thrift::BinaryAddress toBinaryAddressImpl(const IPAddressVx& addr) {
  thrift::BinaryAddress result;
  result.addr()->append(
      reinterpret_cast<const char*>(addr.bytes()), IPAddressVx::byteCount());
  return result;
}

inline thrift::BinaryAddress toBinaryAddress(const folly::IPAddress& addr) {
  return addr.isV4() ? toBinaryAddressImpl(addr.asV4())
      : addr.isV6()  ? toBinaryAddressImpl(addr.asV6())
                     : thrift::BinaryAddress();
}

inline thrift::BinaryAddress toBinaryAddress(const std::string& addr) {
  return toBinaryAddress(folly::IPAddress(addr));
}

template <typename T>
inline folly::IPAddress toIPAddress(const T& input) {
  return input.type != decltype(input.type)::VUNSPEC
      ? folly::IPAddress(input.addr)
      : folly::IPAddress();
}

inline folly::IPAddress toIPAddress(const std::string& binAddr) {
  return folly::IPAddress::fromBinary(
      folly::ByteRange(
          reinterpret_cast<const uint8_t*>(binAddr.data()), binAddr.size()));
}

inline folly::IPAddress toIPAddress(const thrift::BinaryAddress& addr) {
  return folly::IPAddress::fromBinary(
      folly::ByteRange(
          reinterpret_cast<const unsigned char*>(addr.addr()->data()),
          addr.addr()->size()));
}

inline thrift::IpPrefix createIpPrefix(
    thrift::BinaryAddress const& prefixAddress,
    int16_t prefixLength) {
  thrift::IpPrefix ipPrefix;
  ipPrefix.prefixAddress() = prefixAddress;
  ipPrefix.prefixLength() = prefixLength;
  return ipPrefix;
}

inline thrift::IpPrefix toIpPrefix(const folly::CIDRNetwork& network) {
  return createIpPrefix(toBinaryAddress(network.first), network.second);
}

inline thrift::IpPrefix toIpPrefix(const std::string& prefix) {
  thrift::IpPrefix ipPrefix;
  try {
    ipPrefix = toIpPrefix(folly::IPAddress::createNetwork(prefix));
  } catch (const folly::IPAddressFormatException& e) {
    throw std::runtime_error(
        fmt::format("Invalid IPAddress: {}, exception: {}", prefix, e.what()));
  }
  return ipPrefix;
}

inline std::string toString(const thrift::BinaryAddress& addr) {
  return addr.addr()->empty() ? "" : toIPAddress(addr).str();
}

inline std::string toString(const thrift::IpPrefix& ipPrefix) {
  return fmt::format(
      "{}/{}", toString(*ipPrefix.prefixAddress()), *ipPrefix.prefixLength());
}

inline std::string toString(const thrift::MplsAction& mplsAction) {
  return fmt::format(
      "mpls {} {}{}",
      apache::thrift::util::enumNameSafe(*mplsAction.action()),
      mplsAction.swapLabel() ? std::to_string(*mplsAction.swapLabel()) : "",
      mplsAction.pushLabels() ? folly::join("/", *mplsAction.pushLabels())
                              : "");
}

inline std::string toString(const thrift::NextHopThrift& nextHop) {
  return fmt::format(
      "via {} dev {} weight {} metric {} area {} {}",
      toIPAddress(*nextHop.address()).str(),
      nextHop.address()->ifName().value_or("N/A"),
      *nextHop.weight(),
      *nextHop.metric(),
      nextHop.area().value_or("N/A"),
      nextHop.mplsAction().has_value() ? toString(nextHop.mplsAction().value())
                                       : "");
}

inline std::string toString(const folly::IPAddress& addr) {
  return addr.str();
}

inline std::string toString(const thrift::UnicastRoute& route) {
  std::vector<std::string> lines;
  lines.emplace_back(fmt::format("> Prefix: {}", toString(*route.dest())));
  for (const auto& nh : *route.nextHops()) {
    lines.emplace_back("  " + toString(nh));
  }
  return folly::join("\n", lines);
}

inline std::string toString(const thrift::MplsRoute& route) {
  std::vector<std::string> lines;
  lines.emplace_back(fmt::format("> Label: {}", *route.topLabel()));
  for (const auto& nh : *route.nextHops()) {
    lines.emplace_back("  " + toString(nh));
  }
  return folly::join("\n", lines);
}

inline folly::CIDRNetwork toIPNetwork(
    const thrift::IpPrefix& prefix,
    bool applyMask = true) {
  folly::CIDRNetwork network;
  try {
    network = folly::IPAddress::createNetwork(
        toIPAddress(*prefix.prefixAddress()).str(),
        *prefix.prefixLength(),
        applyMask);
  } catch (const folly::IPAddressFormatException& e) {
    throw std::runtime_error(
        fmt::format(
            "Invalid IPAddress: {}, exception: {}",
            toString(prefix),
            e.what()));
  }
  return network;
}

inline std::string toBinaryString(const folly::IPAddress& ip) {
  std::string binary;
  binary.assign(reinterpret_cast<const char*>(ip.bytes()), ip.byteCount());
  return binary;
}
} // namespace openr
