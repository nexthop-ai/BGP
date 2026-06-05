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

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <folly/IPAddress.h>
#include <folly/container/F14Map.h>

namespace facebook::bgp {

class BgpSwitch;

/*
 * Orchestrates a whole-topology BGP simulation.
 *
 * Loads one BgpSwitch per config file, then wires peering sessions together
 * by resolving each peer's neighbor address to the BgpSwitch that owns it.
 * Links are direct in-process references (no TCP); later phases drive
 * origination, propagation and convergence over these links.
 */
class BgpSimulator {
 public:
  BgpSimulator() = default;
  ~BgpSimulator();

  BgpSimulator(const BgpSimulator&) = delete;
  BgpSimulator& operator=(const BgpSimulator&) = delete;
  BgpSimulator(BgpSimulator&&) = delete;
  BgpSimulator& operator=(BgpSimulator&&) = delete;

  /*
   * Parse each config file (reusing the production Config parser) and build a
   * BgpSwitch for it. The switch name is the config file's stem.
   */
  void loadConfigs(const std::vector<std::string>& configPaths);

  // Add a pre-built switch (for programmatic topologies and tests).
  void addSwitch(std::shared_ptr<BgpSwitch> bgpSwitch);

  /*
   * Resolve peer links from config addresses: each peer's neighbor address is
   * matched to the switch that owns it (by router-id or a peer local address).
   * Unmatched peers are left unlinked and logged (they model external /
   * unmodeled neighbors). Call once after all switches are loaded.
   */
  void resolvePeerLinks();

  const std::vector<std::shared_ptr<BgpSwitch>>& switches() const {
    return switches_;
  }
  size_t numSwitches() const {
    return switches_.size();
  }

 private:
  /*
   * Build the address -> owning-switch index used by resolvePeerLinks(): maps
   * each switch's router-id and each of its peers' local addresses.
   */
  folly::F14FastMap<folly::IPAddress, std::shared_ptr<BgpSwitch>>
  buildAddrToSwitchMap() const;

  std::vector<std::shared_ptr<BgpSwitch>> switches_;
};

} // namespace facebook::bgp
