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

#pragma once

#include <chrono>
#include <memory>

#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/EventBase.h>
#include <folly/logging/xlog.h>
#include <thrift/lib/cpp2/async/RocketClientChannel.h>

namespace facebook::bgp {

/**
 * @brief Creates a thrift client of the specified type
 *
 * This is a generic utility function that creates thrift clients for various
 * services (FibService, FbossCtrl, etc.) with configurable timeouts.
 *
 * @tparam ClientType The thrift client type to create
 *         (e.g., apache::thrift::Client<openr::thrift::FibService>,
 *         apache::thrift::Client<fboss::FbossCtrl>)
 * @param evb Reference to the EventBase to use for the connection
 * @param host The host IP address to connect to
 * @param port The port to connect to
 * @param connectTimeout Timeout for establishing the connection
 * @param sendTimeout Timeout for sending data
 * @param recvTimeout Timeout for receiving data
 * @return std::unique_ptr<ClientType> The created thrift client
 */
template <typename ClientType>
std::unique_ptr<ClientType> createThriftClient(
    folly::EventBase& evb,
    const folly::IPAddress& host,
    uint16_t port,
    std::chrono::milliseconds connectTimeout,
    std::chrono::milliseconds sendTimeout,
    std::chrono::milliseconds recvTimeout) {
  // Create socket with connect timeout
  folly::AsyncTransport::UniquePtr sock = folly::AsyncSocket::newSocket(
      &evb,
      folly::SocketAddress(host, port),
      static_cast<uint32_t>(connectTimeout.count()));

  // Set send timeout
  sock->setSendTimeout(static_cast<uint32_t>(sendTimeout.count()));

  // Create RocketClientChannel
  apache::thrift::RocketClientChannel::Ptr channel =
      apache::thrift::RocketClientChannel::newChannel(std::move(sock));

  // Set receive timeout
  channel->setTimeout(static_cast<uint32_t>(recvTimeout.count()));

  XLOGF(
      INFO,
      "Thrift client created - host: {}, port: {}, connect timeout: {}ms, "
      "send timeout: {}ms, recv timeout: {}ms",
      host.str(),
      port,
      connectTimeout.count(),
      sendTimeout.count(),
      recvTimeout.count());

  return std::make_unique<ClientType>(std::move(channel));
}

/**
 * @brief Checks if an existing thrift client connection is still healthy
 *
 * @param client The thrift client to check
 * @return true if the client exists and has a good channel, false otherwise
 */
template <typename ClientType>
bool isThriftClientHealthy(const std::unique_ptr<ClientType>& client) {
  if (!client) {
    return false;
  }
  auto channel =
      dynamic_cast<apache::thrift::RocketClientChannel*>(client->getChannel());
  return channel && channel->good();
}

} // namespace facebook::bgp
