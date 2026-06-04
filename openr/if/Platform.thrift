/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * Vendored from github.com/facebook/openr/openr/if/Platform.thrift, with two
 * adaptations for OSS:
 *   1. Removed `include "fb303/thrift/fb303_core.thrift"` and the `extends
 *      fb303_core.BaseService` clause on FibService. BGP++ uses FibService
 *      only as a client and only calls FibService's own methods (getSwitchRunState,
 *      addUnicastRoute, etc.) — never the BaseService methods.
 *   2. No other content changes — wire format remains identical to upstream.
 *
 * See public_tld/openr/PROVENANCE.md for removal trigger.
 */

package "facebook.com/neteng/fboss/bgp/oss/openr"

namespace cpp openr.thrift
namespace cpp2 openr.thrift

include "openr/if/Network.thrift"
include "thrift/annotation/thrift.thrift"

/**
 * Enum to keep track of Client name to Client-ID mapping.
 */
enum FibClient {
  // OpenR Client
  OPENR = 786,

  // BGP Client
  BGP = 0,

  // Some Placeholder Clients
  CLIENT_1 = 1,
  CLIENT_2 = 2,
  CLIENT_3 = 3,
  CLIENT_4 = 4,
  CLIENT_5 = 5,
}

// SwSwitch run states. SwSwitch moves forward from a
// lower numbered state to the next
enum SwitchRunState {
  UNINITIALIZED = 0,
  INITIALIZED = 1,
  CONFIGURED = 2,
  EXITING = 4,
}

struct NextHopStatus {
  1: bool isReachable;
  2: optional i32 igpCost;
}

struct NextHopRegistrationRequest {
  1: list<binary> nexthops;
}

struct NextHopRegistrationResponse {
  1: map<binary, NextHopStatus> nexthopStatuses;
}

struct NextHopDeregistrationRequest {
  1: list<binary> nexthops;
}

struct NextHopDeregistrationResponse {}

struct StreamNextHopStatusRequest {
  1: optional bool stream_all_loopbacks;
}

struct StreamNextHopStatusResponse {
  1: map<binary, NextHopStatus> nexthopStatuses;
}

struct ConnectedNextHopStatus {
  1: binary remoteAddress;
  2: optional string interfaceName;
  3: bool isReachable;
}

struct ConnectedNextHopStatusRequest {
  1: list<ConnectedNextHopStatus> nextHopStatuses;
}

struct ConnectedNextHopStatusResponse {}

exception PlatformError {
  @thrift.ExceptionMessage
  1: string message;
}

exception PlatformFibUpdateError {
  1: map<i32, list<Network.IpPrefix>> vrf2failedAddUpdatePrefixes;
  2: map<i32, list<Network.IpPrefix>> vrf2failedDeletePrefixes;
  3: list<i32> failedAddUpdateMplsLabels;
  4: list<i32> failedDeleteMplsLabels;
}

struct BatchedSyncFibRequest {
  1: i16 clientId;
  2: list<Network.UnicastRoute> routes;
  3: bool isStart;
  4: bool isEnd;
}

struct BatchedSyncFibResponse {}

const map<i16, i16> clientIdtoProtocolId = {786: 99, 0: 253};
const map<i16, i16> protocolIdtoPriority = {99: 10, 253: 20};
const i16 kUnknowProtAdminDistance = 255;

/**
 * Interface to on-box Fib.
 *
 * NOTE (OSS vendor): upstream openr extends fb303_core.BaseService here. The
 * extension is dropped in this OSS copy; BGP++ only uses FibService's own
 * methods as a client.
 */
service FibService {
  SwitchRunState getSwitchRunState();

  void addUnicastRoute(1: i16 clientId, 2: Network.UnicastRoute route) throws (
    1: PlatformError error,
    2: PlatformFibUpdateError fibError,
  );

  void deleteUnicastRoute(1: i16 clientId, 2: Network.IpPrefix prefix) throws (
    1: PlatformError error,
  );

  void addUnicastRoutes(
    1: i16 clientId,
    2: list<Network.UnicastRoute> routes,
  ) throws (1: PlatformError error, 2: PlatformFibUpdateError fibError);

  void deleteUnicastRoutes(
    1: i16 clientId,
    2: list<Network.IpPrefix> prefixes,
  ) throws (1: PlatformError error);

  void syncFib(1: i16 clientId, 2: list<Network.UnicastRoute> routes) throws (
    1: PlatformError error,
    2: PlatformFibUpdateError fibError,
  );

  BatchedSyncFibResponse batchedSyncFib(
    1: BatchedSyncFibRequest request,
  ) throws (1: PlatformError error, 2: PlatformFibUpdateError fibError);

  list<Network.UnicastRoute> getRouteTableByClient(1: i16 clientId) throws (
    1: PlatformError error,
  );

  void addMplsRoutes(
    1: i16 clientId,
    2: list<Network.MplsRoute> routes,
  ) throws (1: PlatformError error, 2: PlatformFibUpdateError fibError);

  void deleteMplsRoutes(1: i16 clientId, 2: list<i32> topLabels) throws (
    1: PlatformError error,
  );

  void syncMplsFib(1: i16 clientId, 2: list<Network.MplsRoute> routes) throws (
    1: PlatformError error,
    2: PlatformFibUpdateError fibError,
  );

  list<Network.MplsRoute> getMplsRouteTableByClient(1: i16 clientId) throws (
    1: PlatformError error,
  );

  void sendNeighborDownInfo(1: list<string> neighborIp) throws (
    1: PlatformError error,
  );

  NextHopRegistrationResponse registerNextHops(
    -1: NextHopRegistrationRequest req,
  ) throws (1: PlatformError error);

  NextHopDeregistrationResponse deregisterNextHops(
    -1: NextHopDeregistrationRequest req,
  ) throws (1: PlatformError error);

  stream<StreamNextHopStatusResponse> streamNextHopStatus(
    -1: StreamNextHopStatusRequest req,
  ) throws (1: PlatformError error);

  ConnectedNextHopStatusResponse updateConnectedNextHopStatus(
    -1: ConnectedNextHopStatusRequest request,
  ) throws (1: PlatformError error);

  FibCachedStateResponse getFibCachedState(
    -1: FibCachedStateRequest request,
  ) throws (1: PlatformError error);
}

struct NextHopVia {
  1: optional string nextHopGroupName;
  2: optional string remoteAddress;
  3: optional string interfaceName;
  4: optional i32 igpMetric;
}

struct NextHopEntry {
  1: string remoteAddress;
  2: optional string interfaceName;
}

struct FibRoute {
  1: Network.IpPrefix prefix;
  2: list<NextHopVia> vias;
  3: i16 adminDistance;
}

struct NextHopGroup {
  1: string nhgName;
  2: map<i32, NextHopEntry> vias;
  3: i32 refCount;
  4: bool isProgrammed;
}

struct FibCachedStateRequest {}

struct RouteState {
  1: list<FibRoute> routes;
}

struct NextHopGroupCacheState {
  1: list<NextHopGroup> nextHopGroups;
  2: i64 nhgCreationCounter;
  3: i64 programmedNhgsCount;
  4: bool isProgrammingPaused;
  5: i32 numNhgsHighWaterMark;
  6: i32 numNhgsLowWaterMark;
}

struct FibCachedStateResponse {
  1: RouteState routeState;
  2: NextHopGroupCacheState nextHopGroupCacheState;
}

service NeighborListenerClientForFibagent {
  void neighborsChanged(1: list<string> added, 2: list<string> removed) throws (
    1: PlatformError error,
  );
}
