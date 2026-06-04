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

#include "neteng/fboss/bgp/cpp/lib/fibers/BgpPeerDisplayInfo.h"

namespace facebook::nettools::bgplib {

#define BGP_SESSION_STATE_NAME(type) \
  {                                  \
    case BgpSessionState::type:      \
      return #type;                  \
  }

std::string getBgpSessionStateName(BgpSessionState state) {
  switch (state) {
    BGP_SESSION_STATE_NAME(IDLE);
    BGP_SESSION_STATE_NAME(ACTIVE);
    BGP_SESSION_STATE_NAME(CONNECT);
    BGP_SESSION_STATE_NAME(OPEN_SENT);
    BGP_SESSION_STATE_NAME(OPEN_CONFIRM);
    BGP_SESSION_STATE_NAME(ESTABLISHED);
    default:
      return "UNKNOWN";
  }
}

#define RESET_REASON_NAME(type) \
  {                             \
    case ResetReason::type:     \
      return #type;             \
  }

std::string getResetReasonName(ResetReason reason) {
  switch (reason) {
    RESET_REASON_NAME(OPEN_MSG_TIMER_EXPIRE);
    RESET_REASON_NAME(HOLD_TIMER_EXPIRE);
    RESET_REASON_NAME(SOCKET_ERR);
    RESET_REASON_NAME(PARSE_ERR);
    RESET_REASON_NAME(SESSION_ERR);
    RESET_REASON_NAME(NOTIFICATION_RCVD);
    RESET_REASON_NAME(MANUAL_STOP);
    default:
      return "UNKNOWN";
  }
}

} // namespace facebook::nettools::bgplib
