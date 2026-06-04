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

#include <fmt/format.h>

#include "neteng/fboss/bgp/cpp/lib/BgpException.h"
#include "neteng/fboss/bgp/cpp/lib/fibers/BgpSerializer.h"
#include "thrift/lib/cpp/util/EnumUtils.h"

namespace facebook {
namespace nettools {
namespace bgplib {

void BgpSerializer::validateBgpUpdate2(
    BgpUpdate2 const& update,
    BgpCapabilities const& capa) {
  if (!(*capa.mpExtV4Unicast()) &&
      (!update.v4Announced()->empty() || !update.v4Withdrawn()->empty())) {
    throw BgpSerializerException(
        BgpSerializerExceptionCode::AFI_MISMATCH,
        "sending v4 announce/withdraw when ipv4 unicast is not configured");
  }

  const auto& mpWithdrawn = *update.mpWithdrawn();
  if (!isAddressFamilyAllowed(capa, *mpWithdrawn.afi(), *mpWithdrawn.safi()) &&
      (!mpWithdrawn.prefixes()->empty())) {
    throw BgpSerializerException(
        BgpSerializerExceptionCode::AFI_MISMATCH,
        fmt::format(
            "sending mp withdraws for {}/{} when it is not configured",
            apache::thrift::util::enumNameSafe(*mpWithdrawn.afi()),
            apache::thrift::util::enumNameSafe(*mpWithdrawn.safi())));
  }

  const auto& mpAnnounced = *update.mpAnnounced();
  if (!isAddressFamilyAllowed(capa, *mpAnnounced.afi(), *mpAnnounced.safi()) &&
      (!mpAnnounced.prefixes()->empty())) {
    throw BgpSerializerException(
        BgpSerializerExceptionCode::AFI_MISMATCH,
        fmt::format(
            "sending mp annnounce for {}/{} when it is not configured",
            apache::thrift::util::enumNameSafe(*mpAnnounced.afi()),
            apache::thrift::util::enumNameSafe(*mpAnnounced.safi())));
  }
}

bool BgpSerializer::isAddressFamilyAllowed(
    const BgpCapabilities& capa,
    const BgpUpdateAfi& afi,
    const BgpUpdateSafi& safi) {
  if (afi == BgpUpdateAfi::AFI_IPv4 && safi == BgpUpdateSafi::SAFI_UNICAST) {
    return *capa.mpExtV4Unicast();
  }
  if (afi == BgpUpdateAfi::AFI_IPv6 && safi == BgpUpdateSafi::SAFI_UNICAST) {
    return *capa.mpExtV6Unicast();
  }
  if (afi == BgpUpdateAfi::AFI_IPv4 &&
      safi == BgpUpdateSafi::SAFI_LABELED_UNICAST) {
    return *capa.mpExtV4LU();
  }
  if (afi == BgpUpdateAfi::AFI_IPv6 &&
      safi == BgpUpdateSafi::SAFI_LABELED_UNICAST) {
    return *capa.mpExtV6LU();
  }
  if (afi == BgpUpdateAfi::AFI_LS && safi == BgpUpdateSafi::SAFI_LS) {
    return *capa.mpExtLs();
  }
  return false;
}
} // namespace bgplib
} // namespace nettools
} // namespace facebook
