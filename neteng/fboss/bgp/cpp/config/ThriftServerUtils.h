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

#include <folly/io/async/SSLContext.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include "neteng/fboss/bgp/cpp/config/ConfigStructs.h"
#include "neteng/fboss/bgp/if/gen-cpp2/bgp_thrift_types.h"

namespace facebook::bgp {

inline folly::SSLContext::VerifyClientCertificate
getThriftServerClientVerification(const BgpGlobalConfig& globalConfig) {
  if (!globalConfig.thriftServerConfig.has_value()) {
    return folly::SSLContext::VerifyClientCertificate::DO_NOT_REQUEST;
  }
  const auto& verifyType =
      globalConfig.thriftServerConfig->verify_client_type();
  if (!verifyType) {
    return folly::SSLContext::VerifyClientCertificate::DO_NOT_REQUEST;
  }
  switch (*verifyType) {
    case thrift::VerifyClientType::ALWAYS:
      return folly::SSLContext::VerifyClientCertificate::ALWAYS;
    case thrift::VerifyClientType::IF_PRESENTED:
      return folly::SSLContext::VerifyClientCertificate::IF_PRESENTED;
    case thrift::VerifyClientType::DO_NOT_REQUEST:
      return folly::SSLContext::VerifyClientCertificate::DO_NOT_REQUEST;
    default:
      return folly::SSLContext::VerifyClientCertificate::DO_NOT_REQUEST;
  }
}

inline apache::thrift::SSLPolicy getThriftServerSSLPolicy(
    const BgpGlobalConfig& globalConfig) {
  if (!globalConfig.thriftServerConfig.has_value()) {
    return apache::thrift::SSLPolicy::DISABLED;
  }
  const auto& verifyType =
      globalConfig.thriftServerConfig->verify_client_type();
  if (!verifyType) {
    return apache::thrift::SSLPolicy::DISABLED;
  }
  switch (*verifyType) {
    case thrift::VerifyClientType::ALWAYS:
      return apache::thrift::SSLPolicy::REQUIRED;
    case thrift::VerifyClientType::IF_PRESENTED:
      return apache::thrift::SSLPolicy::PERMITTED;
    case thrift::VerifyClientType::DO_NOT_REQUEST:
      return apache::thrift::SSLPolicy::DISABLED;
    default:
      return apache::thrift::SSLPolicy::DISABLED;
  }
}

} // namespace facebook::bgp
