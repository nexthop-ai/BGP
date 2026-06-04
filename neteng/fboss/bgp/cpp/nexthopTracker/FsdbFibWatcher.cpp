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

#include "neteng/fboss/bgp/cpp/nexthopTracker/FsdbFibWatcher.h"
#include <fboss/agent/if/gen-cpp2/FbossCtrlAsyncClient.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <folly/CancellationToken.h>
#include <folly/coro/Task.h>
#include <folly/logging/xlog.h>
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/common/ThriftClientUtils.h"
#include "neteng/fboss/bgp/cpp/stats/Stats.h"
namespace facebook::bgp {
namespace thrift_tags = apache::thrift::ident;

FsdbFibWatcher::FsdbFibWatcher(
    std::shared_ptr<NexthopCache> nexthopCache,
    nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage>& ribInQ,
    folly::EventBase* evb,
    std::shared_ptr<fboss::fsdb::FsdbCowStateSubManager> fsdbSubMgr)
    : fsdbSubMgr_(std::move(fsdbSubMgr)),
      nexthopCache_(std::move(nexthopCache)),
      ribInQ_(ribInQ),
      evb_(evb) {}

void FsdbFibWatcher::addPeerAddress(const folly::IPAddress& peerAddr) {
  if (pathsAdded_) {
    XLOGF(
        WARNING,
        "[FsdbFibWatcher] Cannot add peer {} after paths have been registered"
        " — runtime subscription changes are not yet supported",
        peerAddr.str());
    return;
  }
  if (subscribedPeers_.insert(peerAddr).second) {
    XLOGF(INFO, "[FsdbFibWatcher] Added peer address: {}", peerAddr.str());
  }
}

void FsdbFibWatcher::removePeerAddress(const folly::IPAddress& peerAddr) {
  if (pathsAdded_) {
    XLOGF(
        WARNING,
        "[FsdbFibWatcher] Cannot remove peer {} after paths have been"
        " registered — runtime subscription changes are not yet supported",
        peerAddr.str());
    return;
  }
  if (subscribedPeers_.erase(peerAddr)) {
    XLOGF(INFO, "[FsdbFibWatcher] Removed peer address: {}", peerAddr.str());
  }
}

void FsdbFibWatcher::addPaths() noexcept {
  /**
   * Fetch switch IDs from the FBOSS agent so we subscribe to FIB host
   * routes under every switch ID on the device, not just "0".
   **/
  try {
    folly::EventBase evb;
    auto client =
        createThriftClient<apache::thrift::Client<facebook::fboss::FbossCtrl>>(
            evb,
            kLoopBackAddressV6,
            kFbossAgentPort,
            kFbossAgentConnTimeout,
            kFbossAgentSendTimeout,
            kFbossAgentRecvTimeout);
    std::map<int64_t, facebook::fboss::cfg::SwitchInfo> switchInfoMap;
    client->sync_getSwitchIdToSwitchInfo(switchInfoMap);
    for (const auto& [switchId, _] : switchInfoMap) {
      switchIds_.push_back(fmt::format("id={}", switchId));
    }
    XLOGF(
        INFO,
        "[FsdbFibWatcher] Retrieved {} switch IDs from agent: [{}]",
        switchIds_.size(),
        fmt::join(switchIds_, ", "));
  } catch (const std::exception& ex) {
    XLOGF(
        WARNING,
        "[FsdbFibWatcher] Failed to get switch IDs from agent: {}."
        " Defaulting to \"0\"",
        ex.what());
    switchIds_.emplace_back("id=0");
  }

  for (const auto& switchId : switchIds_) {
    auto basePath = fsdbStateRootPath_.agent()
                        .switchState()
                        .fibsInfoMap()[switchId]
                        .fibsMap()[kDefaultVrfId];

    for (const auto& peerAddr : subscribedPeers_) {
      bool isV4 = peerAddr.isV4();
      auto hostPrefix = fmt::format("{}/{}", peerAddr.str(), isV4 ? 32 : 128);
      if (isV4) {
        fsdbSubMgr_->addPath(basePath.fibV4()[hostPrefix]);
      } else {
        fsdbSubMgr_->addPath(basePath.fibV6()[hostPrefix]);
      }
      subscribedPrefixes_.emplace(hostPrefix, std::make_pair(peerAddr, isV4));
      auto pathTokens = isV4 ? basePath.fibV4()[hostPrefix].tokens()
                             : basePath.fibV6()[hostPrefix].tokens();
      XLOGF(
          INFO,
          "[FsdbFibWatcher] Added FSDB path: [{}]",
          fmt::join(pathTokens, "/"));
    }
  }

  pathsAdded_ = true;

  XLOGF(
      INFO,
      "[FsdbFibWatcher] Added {} FSDB paths ({} peers x {} switch IDs)",
      subscribedPeers_.size() * switchIds_.size(),
      subscribedPeers_.size(),
      switchIds_.size());
}

void FsdbFibWatcher::registerPeers(
    const std::vector<folly::IPAddress>& peerAddresses) {
  for (const auto& peerAddr : peerAddresses) {
    addPeerAddress(peerAddr);
  }

  evb_->runInEventBaseThread([this]() { addPaths(); });

  XLOGF(
      INFO,
      "[FsdbFibWatcher] {} static peer addresses queued for subscription",
      peerAddresses.size());
}

void FsdbFibWatcher::stop() noexcept {
  // Subscription lifecycle is managed by the shared FsdbCowStateSubManager
  // owner
}

folly::coro::Task<void> FsdbFibWatcher::co_updateCacheAndPushToRib(
    const std::vector<NexthopStatus>& updates) {
  auto statuses = nexthopCache_->addOrUpdateNextHopStatus(updates);
  if (!statuses.empty()) {
    auto result = co_await co_awaitTry(
        ribInQ_.push(RibInNexthopUpdate(std::move(statuses))));
    if (!result.hasValue()) {
      co_yield folly::coro::co_error(std::move(result).exception());
    }
  }
}

folly::coro::Task<void> FsdbFibWatcher::co_markNeedsReconcile() {
  if (reachableNexthops_.empty()) {
    XLOG(
        INFO,
        "[FsdbFibWatcher] Reconnect: no previously reachable nexthops to"
        " clear");
    co_return;
  }
  XLOGF(
      INFO,
      "[FsdbFibWatcher] Reconnect: clearing {} reachable nexthops —"
      " will rebuild from notifications",
      reachableNexthops_.size());
  FsdbStats::incrFsdbNhtDisconnects();
  FsdbStats::setFsdbNhtConnected(0);
  std::vector<NexthopStatus> batchUpdates;
  for (const auto& ip : reachableNexthops_) {
    batchUpdates.emplace_back(ip, /* isReachable=*/false);
  }
  reachableNexthops_.clear();

  auto result = co_await co_awaitTry(co_updateCacheAndPushToRib(batchUpdates));
  if (result.hasException<folly::OperationCancelled>()) {
    co_yield folly::coro::co_error(std::move(result).exception());
  }
  if (result.hasException()) {
    XLOGF(
        ERR,
        "[FsdbFibWatcher] co_markNeedsReconcile failed: {}",
        result.exception().what());
  }
}

std::pair<bool, std::optional<uint32_t>> FsdbFibWatcher::routeExistsInCowTree(
    const std::string& prefixStr,
    bool isV4,
    const fboss::fsdb::FsdbCowStateSubManager::Data& data) {
  auto agentNode = data->safe_cref<thrift_tags::agent>();
  if (!agentNode) {
    XLOGF(INFO, "[FsdbFibWatcher] routeExists({}): no agent node", prefixStr);
    return {false, std::nullopt};
  }
  auto switchStateNode =
      agentNode->template safe_cref<thrift_tags::switchState>();
  if (!switchStateNode) {
    XLOGF(
        INFO,
        "[FsdbFibWatcher] routeExists({}): no switchState node",
        prefixStr);
    return {false, std::nullopt};
  }
  auto fibsInfoMapNode =
      switchStateNode->template safe_cref<thrift_tags::fibsInfoMap>();
  if (!fibsInfoMapNode) {
    XLOGF(
        INFO,
        "[FsdbFibWatcher] routeExists({}): no fibsInfoMap node",
        prefixStr);
    return {false, std::nullopt};
  }

  for (const auto& switchId : switchIds_) {
    if (!fibsInfoMapNode->count(switchId)) {
      XLOGF(
          INFO,
          "[FsdbFibWatcher] routeExists({}): switchId={} not in fibsInfoMap",
          prefixStr,
          switchId);
      continue;
    }
    const auto& fibInfoNode = fibsInfoMapNode->cref(switchId);
    auto fibsMapNode = fibInfoNode->template safe_cref<thrift_tags::fibsMap>();
    if (!fibsMapNode) {
      XLOGF(
          INFO,
          "[FsdbFibWatcher] routeExists({}): switchId={} no fibsMap",
          prefixStr,
          switchId);
      continue;
    }
    if (!fibsMapNode->count(kDefaultVrfId)) {
      XLOGF(
          INFO,
          "[FsdbFibWatcher] routeExists({}): switchId={} vrfId=0 not in"
          " fibsMap",
          prefixStr,
          switchId);
      continue;
    }
    const auto& fibContainerNode = fibsMapNode->cref(kDefaultVrfId);
    auto mapNode = isV4
        ? fibContainerNode->template safe_cref<thrift_tags::fibV4>()
        : fibContainerNode->template safe_cref<thrift_tags::fibV6>();
    if (!mapNode) {
      XLOGF(
          INFO,
          "[FsdbFibWatcher] routeExists({}): switchId={} no {} node",
          prefixStr,
          switchId,
          isV4 ? "fibV4" : "fibV6");
      continue;
    }
    if (mapNode->count(prefixStr) > 0) {
      std::optional<uint32_t> igpCost;
      const auto& routeNode = mapNode->cref(prefixStr);
      auto fwdNode = routeNode->template safe_cref<thrift_tags::fwd>();
      if (fwdNode) {
        auto nexthopsNode =
            fwdNode->template safe_cref<thrift_tags::nexthops>();
        if (nexthopsNode) {
          for (const auto& nhop : *nexthopsNode) {
            auto costNode = nhop->template safe_cref<thrift_tags::cost>();
            if (costNode) {
              auto cost = static_cast<uint32_t>(costNode->cref());
              if (!igpCost || cost < *igpCost) {
                igpCost = cost;
              }
            }
          }
        }
      }
      XLOGF(
          INFO,
          "[FsdbFibWatcher] routeExists({}): found in switchId={} {}"
          " igpCost={}",
          prefixStr,
          switchId,
          isV4 ? "fibV4" : "fibV6",
          igpCost ? fmt::format("{}", *igpCost) : "none");
      return {true, igpCost};
    }
    XLOGF(
        INFO,
        "[FsdbFibWatcher] routeExists({}): prefix not in switchId={} {}",
        prefixStr,
        switchId,
        isV4 ? "fibV4" : "fibV6");
  }
  return {false, std::nullopt};
}

folly::coro::Task<void> FsdbFibWatcher::co_processFibUpdate(
    fboss::fsdb::FsdbCowStateSubManager::SubUpdate update) {
  /**
   * Purely event-driven: scan updatedPath tokens for subscribed prefix
   * strings. Struct fields use numeric thrift IDs, but map keys (like
   * "fdad:500::d:0/128") appear as literal string tokens. Match these
   * against subscribedPrefixes_ to find affected peers.
   *
   * No notification for a prefix means the route does not exist (peer is
   * unreachable). As notifications arrive, we update nexthop status.
   * On reconnect, markNeedsReconcile() clears all reachable state so
   * status is rebuilt purely from new notifications.
   **/
  FsdbStats::setFsdbNhtConnected(1);
  std::vector<NexthopStatus> batchUpdates;

  for (const auto& pathTokens : update.updatedPaths) {
    for (const auto& token : pathTokens) {
      auto it = subscribedPrefixes_.find(token);
      if (it == subscribedPrefixes_.end()) {
        continue;
      }
      const auto& [peerAddr, isV4] = it->second;
      const auto& prefixStr = it->first;
      auto [exists, igpCost] =
          routeExistsInCowTree(prefixStr, isV4, update.data);
      bool wasReachable = reachableNexthops_.contains(peerAddr);

      if (exists) {
        if (!wasReachable) {
          reachableNexthops_.insert(peerAddr);
          FsdbStats::incrFsdbNhtNexthopReachable();
        }
        batchUpdates.emplace_back(peerAddr, /*isReachable=*/true, igpCost);
        XLOGF(
            INFO,
            "[FsdbFibWatcher] Nexthop {}: {} igpCost={}",
            wasReachable ? "cost update" : "became reachable",
            peerAddr.str(),
            igpCost ? fmt::format("{}", *igpCost) : "none");
      } else {
        if (wasReachable) {
          reachableNexthops_.erase(peerAddr);
          FsdbStats::incrFsdbNhtNexthopUnreachable();
        }
        batchUpdates.emplace_back(
            peerAddr, /*isReachable=*/false, std::nullopt);
        XLOGF(
            INFO,
            "[FsdbFibWatcher] Nexthop unreachable: {} igpCost=none",
            peerAddr.str());
      }
      break; // found the relevant token, done with this path
    }
  }

  XLOG(INFO, "batchUpdates", batchUpdates.size());
  if (!batchUpdates.empty()) {
    XLOGF(
        INFO,
        "[FsdbFibWatcher] Pushing {} nexthop updates to cache"
        " ({} total reachable)",
        batchUpdates.size(),
        reachableNexthops_.size());
    auto result =
        co_await co_awaitTry(co_updateCacheAndPushToRib(batchUpdates));
    if (result.hasException<folly::OperationCancelled>()) {
      co_yield folly::coro::co_error(std::move(result).exception());
    }
    if (result.hasException()) {
      XLOGF(
          ERR,
          "[FsdbFibWatcher] co_processFibUpdate failed: {}",
          result.exception().what());
    }
  }
}

} // namespace facebook::bgp
