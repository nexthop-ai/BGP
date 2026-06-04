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

#include <folly/IPAddress.h>
#include <folly/io/IOBuf.h>

#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/if/gen-cpp2/BgpStructs_types.h"

namespace facebook::bgp {

/**
 * @brief  Utilities for serializing BGP messages for update groups
 *
 * Provides zero-copy serialization support where the group serializes
 * BGP UPDATE once and distributes UpdateDescriptor to all peers.
 * Each peer clones the IOBuf (cheap, shares memory) and mutates only
 * the nexthop field.
 */
class AdjRibGroupSerializer {
 public:
  /**
   * @brief  Serialize BgpUpdate2 and create UpdateDescriptor for zero-copy
   * distribution
   *
   * Serializes the BGP UPDATE message once and creates an UpdateDescriptor
   * that contains:
   *   - Shared serialized PDU (immutable, zero-copy across all peers)
   *   - Offset information for nexthop field location
   *   - Nexthop value to inject at I/O time
   *
   * The group builds the message with a placeholder nexthop (0.0.0.0 for IPv4,
   * :: for IPv6) and serializes it once. The serializer tracks and returns
   * the nexthop offset during serialization (zero-copy optimization).
   *
   * Each peer's I/O thread will:
   *   1. Clone the IOBuf (cheap, shares underlying memory via refcount)
   *   2. Mutate only the nexthop bytes at the known offset
   *   3. Write to socket
   *
   * @param  message - BgpUpdate2 with placeholder nexthop (0.0.0.0 or ::)
   * @param  as4byte - Use 4-byte ASN encoding
   * @param  extNhEncoding - Support v4 NLRI over v6 nexthop (RFC5549)
   * @return UpdateDescriptor with serialized PDU and nexthop offset info
   *         Returns empty descriptor (serializedGroupPDU == nullptr) on error
   */
  static nettools::bgplib::UpdateDescriptor serializeUpdateAndCreateDescriptor(
      const nettools::bgplib::BgpUpdate2& message,
      bool as4byte,
      bool extNhEncoding) noexcept;
};

} // namespace facebook::bgp
