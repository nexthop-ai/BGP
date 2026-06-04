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

/*
 * This file defines exceptions that can be thrown in the bgplib. Currently
 * it implements minimalistic exceptions that are required for parsing and
 * serialization. There are few other exception which haven't been defined
 * here but can potentially be defined when needed (e.g. FsmException and
 * CeaseException)
 */

#pragma once

#include <string>
#include <utility>

#include "neteng/fboss/bgp/if/gen-cpp2/BgpStructs_types.h"

namespace facebook {
namespace nettools {
namespace bgplib {

/**
 * Simple exception thrown in bgplib with just string message
 */
class BgpException : public std::exception {
 public:
  /**
   * Default constructor
   */
  explicit BgpException(const std::string& msg) : msg_(msg) {}

  /**
   * Virtual destructor for subclassing
   */
  ~BgpException(void) noexcept override {}

  /**
   * Returns pointer to the (constant) error description.
   */
  inline const char* what(void) const noexcept override {
    return msg_.c_str();
  }

 private:
  // Optional message associated with this exception
  const std::string msg_;
};

/**
 * This exception is thrown when received bgp message is not as expected
 * according to rfc standards.
 */
class BgpHeaderException : public std::exception {
 public:
  /**
   * Default constructor
   */
  BgpHeaderException(
      BgpNotifMsgHdrErrSubCode subcode,
      std::string data,
      const std::string& msg)
      : subcode_(subcode), data_(std::move(data)), msg_(msg) {}

  /**
   * Virtual destructor for subclassing
   */
  ~BgpHeaderException(void) noexcept override {}

  /**
   * Returns pointer to the (constant) error description.
   */
  inline const char* what(void) const noexcept override {
    return msg_.c_str();
  }

  inline BgpNotifErrCode getCode() const {
    return code_;
  }

  inline BgpNotifMsgHdrErrSubCode getSubCode() const {
    return subcode_;
  }

  inline std::string getData() const {
    return data_;
  }

 private:
  // Notification error code type. This is constant for this class
  const BgpNotifErrCode code_ = BgpNotifErrCode::BN_MSG_HDR_ERR;

  // Notification error subcode for message header
  const BgpNotifMsgHdrErrSubCode subcode_;

  // Data associated with this error
  const std::string data_;

  // Optional message associated with this exception
  const std::string msg_;
};

/**
 * This exception gets thrown when open messages are exchanged between two
 * peers after establishing tcp connection.
 */
class BgpOpenMsgException : public std::exception {
 public:
  /**
   * Default constructor
   */
  BgpOpenMsgException(
      BgpNotifOpenMsgErrSubCode subcode,
      std::string data,
      const std::string& msg)
      : subcode_(subcode), data_(std::move(data)), msg_(msg) {}

  /**
   * Virtual destructor for subclassing
   */
  ~BgpOpenMsgException(void) noexcept override {}

  /**
   * Returns pointer to the (constant) error description.
   */
  inline const char* what(void) const noexcept override {
    return msg_.c_str();
  }

  inline BgpNotifErrCode getCode() const {
    return code_;
  }

  inline BgpNotifOpenMsgErrSubCode getSubCode() const {
    return subcode_;
  }

  inline std::string getData() const {
    return data_;
  }

 private:
  // Notification error code type. This is constant for this class
  const BgpNotifErrCode code_ = BgpNotifErrCode::BN_OPEN_MSG_ERR;

  // Notification error subcode for message header
  const BgpNotifOpenMsgErrSubCode subcode_;

  // Data associated with this error
  std::string data_;

  // Optional message associated with this exception
  const std::string msg_;
};

/**
 * This exception can be thrown while parsing BGP Update message.
 */
class BgpUpdateMsgException : public std::exception {
 public:
  /**
   * Default constructor
   */
  BgpUpdateMsgException(
      BgpNotifUpdateMsgErrSubCode subcode,
      std::string data,
      const std::string& msg)
      : subcode_(subcode), data_(std::move(data)), msg_(msg) {}

  /**
   * Virtual destructor for subclassing
   */
  ~BgpUpdateMsgException(void) noexcept override {}

  /**
   * Returns pointer to the (constant) error description.
   */
  inline const char* what(void) const noexcept override {
    return msg_.c_str();
  }

  inline BgpNotifErrCode getCode() const {
    return code_;
  }

  inline BgpNotifUpdateMsgErrSubCode getSubCode() const {
    return subcode_;
  }

  inline std::string getData() const {
    return data_;
  }

 private:
  // Notification error code type. This is constant for this class
  const BgpNotifErrCode code_ = BgpNotifErrCode::BN_UPDATE_MSG_ERR;

  // Notification error subcode for message header
  const BgpNotifUpdateMsgErrSubCode subcode_;

  // Data associated with this error
  std::string data_;

  // Optional message associated with this exception
  const std::string msg_;
};

/**
 * This exception can be thrown while parsing BGP Route Refresh message
 * if not according to RFC 7313 standards.
 */
class BgpRouteRefreshMsgException : public std::exception {
 public:
  /**
   * Default constructor
   */
  BgpRouteRefreshMsgException(
      BgpNotificationRouteRefreshErrSubCode subcode,
      std::string data,
      const std::string& msg)
      : subcode_(subcode), data_(std::move(data)), msg_(msg) {}

  BgpRouteRefreshMsgException(const BgpRouteRefreshMsgException&) = default;

  BgpRouteRefreshMsgException(BgpRouteRefreshMsgException&&) = default;

  BgpRouteRefreshMsgException& operator=(const BgpRouteRefreshMsgException&) =
      delete;

  BgpRouteRefreshMsgException& operator=(BgpRouteRefreshMsgException&&) =
      delete;

  /**
   * Virtual destructor for subclassing
   */
  ~BgpRouteRefreshMsgException(void) noexcept override {}

  /**
   * Returns pointer to the (constant) error description.
   */
  inline const char* what(void) const noexcept override {
    return msg_.c_str();
  }

  inline BgpNotifErrCode getCode() const {
    return code_;
  }

  inline BgpNotificationRouteRefreshErrSubCode getSubCode() const {
    return subcode_;
  }

  inline std::string getData() const {
    return data_;
  }

 private:
  // Notification error code type. This is constant for this class
  const BgpNotifErrCode code_ = BgpNotifErrCode::BN_ROUTE_REFRESH_MSG_ERR;

  // Notification error subcode for message header
  const BgpNotificationRouteRefreshErrSubCode subcode_;

  // Data associated with this error
  std::string data_;

  // Optional message associated with this exception
  const std::string msg_;
};

/**
 * FSM error.
 * Could be made a sub-type of BgpException, but distinct type may help
 * ensure it is not mistakingly confused with BgpException.
 */
class BgpFsmException : public std::exception {
 public:
  /**
   * Default constructor
   */
  explicit BgpFsmException(const std::string& msg) : msg_(msg) {}

  /**
   * Virtual destructor for subclassing
   */
  ~BgpFsmException(void) noexcept override {}

  /**
   * Returns pointer to the (constant) error description.
   */
  inline const char* what(void) const noexcept override {
    return msg_.c_str();
  }

 private:
  // Optional message associated with this exception
  const std::string msg_;
};

enum class BgpSerializerExceptionCode {
  // Serializer specific exception codes
  AFI_MISMATCH,
  EXCEEDED_MAX_SIZE,
  MISSING_NLRI_INFO,
  UNKNOWN_BGP_UPDATE_TYPE,
  INVALID_ASPATH_INFO,
  INVALID_NLRI_LABEL_INFO,
  PATHID_PATH_NOT_MATCH,
  VERSION_ERROR,
  EXT_NH_ENCODING_NOT_SUPPORTTED,
};

/**
 * Defines exception which can be thrown while serializing route updates using
 * BgpMessageSerializer.
 */
class BgpSerializerException : public std::exception {
 public:
  /**
   * Default constructor
   */
  BgpSerializerException(
      BgpSerializerExceptionCode code,
      const std::string& msg)
      : code_(code), msg_(msg) {}

  /**
   * Virtual destructor for subclassing
   */
  ~BgpSerializerException(void) noexcept override {}

  /**
   * Returns pointer to the (constant) error description.
   */
  inline const char* what(void) const noexcept override {
    return msg_.c_str();
  }

  /**
   * Returns error code associated with this exception.
   */
  inline BgpSerializerExceptionCode getCode() {
    return code_;
  }

 private:
  // Error code associated with this exception
  BgpSerializerExceptionCode code_;

  // Optional message associated with this exception
  const std::string msg_;
};

} // namespace bgplib
} // namespace nettools
} // namespace facebook
