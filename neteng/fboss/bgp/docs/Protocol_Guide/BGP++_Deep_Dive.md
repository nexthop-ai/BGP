# BGP++ Deep Dive

This document provides a deep-dive into the internal
architecture, threads, queues, and data structures of
BGP++. For a higher-level overview, see
[Architecture](Architecture).

## References

- [BGP RFC 4271](https://tools.ietf.org/html/rfc4271)
- [ECMP Considerations for BGP](https://datatracker.ietf.org/doc/draft-lapukhov-bgp-ecmp-considerations/)
- [Architecture Diagram (Excalidraw)](https://excalidraw.com/s/EX162258)

## Nomenclature

- **Agent, FBOSS Agent, Wedge Agent**: Used
  interchangeably. The name "wedge agent" is
  historical — FBOSS was initially supported only
  on RSWs (formerly called "wedge"). You may also
  see `FbossCtrl` in some places from the earlier
  "wedge controller" naming.

## Building Blocks

All BGP++ code lives under
`fbcode/neteng/fboss/bgp/cpp/`:

- **`cpp/lib/`** — Session library. Implements all
  functionality for the external-facing peer (TCP
  I/O, BGP FSM, message parsing/serialization).
  Used by BGP++, BGP Monitor, BGP Collector, VIP
  Injector, etc. Changes here affect multiple
  binaries.
- **`cpp/`** — BGP++ daemon. Implements all
  functionality specific to the daemon (AdjRib,
  PeerManager, RIB, FIB, Policy, Config, etc.).

### BgpModuleBase Pattern

Every major module extends `BgpModuleBase`, which
provides:

- Its own `folly::EventBase evb_`
- Its own `folly::coro::CancellableAsyncScope`
- `runInThread()`: spawns a `std::thread`, sets
  thread name to `bgpcpp-{moduleName}`, calls
  virtual `run()`, and starts the EventBase loop

This enforces the "one module, one EventBase, one
thread" architecture. Inter-module communication
is via typed message queues, never shared mutable
state.

### Session Library Components

- **FiberBgpPeerManager**: Main entry point for
  peer management. Listens for passive TCP
  connections and initiates active connections.
  Handles collision detection. Calls
  `FiberBgpPeer->run()` per peer.
- **FiberBgpPeer**: Get/send messages over TCP
  socket to external peer. One fiber per peer.
- **FiberBgpParser**: Parses messages from
  external peer.
- **BgpSerializer**: Serializes messages to send
  to external peer.
- **FiberSocket**: Fiber-aware socket wrapper
  based on `folly::AsyncSocket`. See
  [FiberSocket](io/fibersocket).
- **Queue**: RQueue, WQueue, RWQueue
  implementations.

### BGP++ Daemon Components

- **Main**: Spawns all modules/threads, wires
  queues, handles signals (SIGTERM), orchestrates
  shutdown.
- **SessionManager**: Extends FiberBgpPeerManager.
  Runs on its **own I/O thread** — handles all TCP
  socket operations, BGP FSM, message
  parsing/serialization via per-peer fibers.
- **PeerManager**: Entry point for peer lifecycle
  management. Processes session UP/DOWN events from
  SessionManager. Contains AdjRib instances,
  ShadowRib, ChangeTracker, UpdateGroupManager.
- **AdjRib**: Maintains Adj-Rib-In and Adj-Rib-Out
  per the BGP RFC. Runs as coroutines on
  **PeerManager's EventBase** (not its own thread).
- **RIB**: Routing Information Base. Best path
  selection, FIB creation. Own thread.
- **FibFboss**: Interface with wedge_agent. Full
  sync on restart; incremental add/delete
  thereafter.
- **NeighborWatcher** (DC only): Subscribes to
  FSDB for ARP/NDP resolution and link state.
  Own thread.
- **Watchdog**: Periodic health checks on
  monitored modules (`MonitoredModule`). Own thread.
- **ConfigManager**: Wraps Config, loads split
  policy files.
- **PolicyManager**: Filters or updates paths in
  both directions. See
  [BGP Policy](../Features/BGP_Policy).
- **BgpService**: Thrift interface on port 6909.
- **BgpStreamService**: Streaming thrift interface
  on port 6910.
- **FsdbSyncer** (DC only): Publishes RIB state
  to FSDB.
- **MemoryLimitChecker**: Background RSS
  monitoring.
- **NexthopCache**: Shared data structure for
  nexthop tracking (DC: conditional; BB: always).
- **Stats**: ODS counter updates.

#### BB-Only Components

- **NetlinkWrapper**: Subscribes to netlink events
  for interface/route changes. Own thread.
- **NexthopHandler**: Resolves nexthops via OpenR
  FIB agent. Own thread.

## Threading Model

BGP++ follows the BgpModuleBase pattern: each major
module runs on its own thread with its own EventBase.
Within threads, **coroutines** (`folly::coro::Task`)
and **fibers** provide concurrency.

### DC Thread Inventory

| Thread | Name | Module |
|--------|------|--------|
| Signal Handler | (manual) | BgpSignalHandler |
| NeighborWatcher | `bgpcpp-neighbor_watcher` | NeighborWatcher |
| Watchdog | `bgpcpp-watchdog` | Watchdog |
| RIB | `bgpcpp-rib` | Rib |
| PeerManager | `bgpcpp-peer_manager` | PeerManagerVipManager |
| SessionManager (I/O) | `bgpcpp-fiber_bgp_peer_manager` | SessionManager |
| Thrift Service | (manual) | ServiceFramework("bgpd") |
| Stream Service | (manual) | ServiceFramework("BgpStreamService") |

### BB Additional Threads

| Thread | Name | Module |
|--------|------|--------|
| NetlinkWrapper | `bgpcpp-netlink_wrapper` | NetlinkWrapper |
| NexthopHandler | `bgpcpp-nexthop_handler` | NexthopHandler |

BB uses `PeerManager` (no VIP support) instead of
`PeerManagerVipManager`. BB does not create
NeighborWatcher or FsdbSyncer.

### Key Architecture Points

- **SessionManager runs on a SEPARATE thread from
  PeerManager.** It has its own EventBase and
  FiberManager. This isolates I/O-bound work (TCP)
  from CPU-bound work (policy, route processing).
- **AdjRib runs on PeerManager's EventBase** as
  coroutines, not on its own thread.
- **PolicyManager** is an object created from
  config in Main, passed to PeerManager and RIB.

## Inter-Thread Queues

### Core Queues (created in Main)

| Queue | Type | Direction |
|-------|------|-----------|
| `ribInQ` | `MonitoredBackPressuredQueue<RibInMessage>` (bounded) | PeerManager/NeighborWatcher -> RIB |
| `ribOutQ` | `MonitoredMPMCQueue<RibOutMessage>` | RIB -> PeerManager |
| `neighborEventQ` | `MonitoredMPMCQueue<NeighborWatcherMessage>` | NeighborWatcher -> PeerManager |

### Cross-Thread Queues

| Queue | Type | Direction |
|-------|------|-----------|
| `notifyCoroQueue_` | `centralium::coro::MPMCQueue<ObservableEventT>` | SessionManager -> PeerManager (session UP/DOWN) |
| `adjRibInQueue_` (per-peer) | OutputQueueT | SessionManager -> AdjRibIn (parsed BGP updates) |
| `adjRibOutQueue_` (per-peer) | InputQueueT / BoundedInputQueueT | AdjRibOut -> SessionManager (serialized updates) |
| `fromAdjRibQ_` | `MonitoredMPMCQueue<AdjRib::ObservableMessageT>` | AdjRib instances -> PeerManager (EoR, shutdown, etc.) |

## PeerManager

Runs coroutines on its EventBase:

1. **`processPeerEventLoop`** — Reads
   `notifyCoroQueue_` from SessionManager.
   Processes session UP/DOWN events.
2. **`processRibOutMsgLoop`** — Reads `ribOutQ_`
   from RIB. Publishes changes to ShadowRib and
   ChangeTracker for egress pipeline consumption.
3. **`processAdjRibMsgLoop`** — Reads
   `fromAdjRibQ_` from AdjRib instances (EoR,
   EgressEoR, Shutdown, TriggerSafeMode).
4. **`processNeighborRouteChangeLoop`** — Reads
   `neighborEventQ_` from NeighborWatcher.
5. **`publishUpdatesRoutine`** — Publishes updates
   to ShadowRib.

### Internal Components

- **ShadowRib**: Central route state store for
  egress pipeline. See
  [Shadow RIB](egress-pipeline/shadowrib-integration).
- **ChangeTracker**: Pub-sub for route changes.
  See
  [Change List](egress-pipeline/changelist-integration).
- **UpdateGroupManager**: Groups peers with
  identical egress policies for shared update
  generation (when `enableUpdateGroup_` is set).

### Internal Containers

- `adjRibs_`: `unordered_map<BgpPeerId, AdjRib>`

## AdjRib

Runs as coroutines on PeerManager's EventBase. Two
main loops per peer:

- **`processPeerMessageLoop`** (ingress): Reads
  `adjRibInQueue_` from SessionManager. Processes
  BGP updates, applies ingress policy, writes to
  `ribInQ_`.
- **`processRibMessageLoop`** (egress): Consumes
  changes from ChangeTracker or ribOutQ. Applies
  egress policy, builds BGP UPDATE messages,
  writes to `adjRibOutQueue_` for SessionManager.

### Internal Containers

- **adjRibInLiteTree_** / **adjRibInPathTree_**:
  RadixTree for non-add-path and add-path peers.
- **attrToPrefixMap_**: Groups prefixes with same
  BgpAttributes for efficient UPDATE packing.

### GR Timers

- **remoteGrRestartTimer_**: On graceful session
  down, marks routes as stale and starts timer.
  RIB retains routes. On expiry,
  `cleanupStaleRoutes` withdraws them.
- **stalePathTimer_**: On session re-establishment
  after GR, schedules reconciliation. Re-learned
  routes are removed from stale map. After EoR or
  timer expiry, remaining stale routes are
  withdrawn.

## RIB

Runs coroutines on its own EventBase:

1. **`processRibInMsgLoop`**: Processes
   announcement/withdrawal messages from AdjRib.
   Batch-processes via `fibTimer_` (200ms).
   Calls `programFib()`.
2. **`fibProgrammingLoop`**: Programs FIB via
   `fib_->updateUnicastRoute()` and
   `fib_->program()`.
3. **`processFibMsgLoop`**: Handles
   `FibProgrammedMessage` — sends announcements
   to PeerManager via `ribOutQ_`.
4. **`processRibPolicyMsgLoop`**: Handles
   RibPolicy changes.
5. **`processLocalRoutesRoutine`**: Manages
   locally originated routes.

### RIB Data Structures

- **ribEntries_**:
  `unordered_map<CIDRNetwork, RibEntry>`

### RibEntry

One entry per prefix:

- **routeInfos_**:
  `unordered_map<BgpPeerId, RouteInfo>`
- **multipaths_**: `vector<RouteInfo>`

## FIB

### FIB Queues

- **toRibQ_**: Sends `FibProgrammedMessage` or
  `FibSyncReq` to RIB.

### FIB Data Structures

- **FibProgrammedMessage**: struct of
  `FibProgrammedNexthops` + `isSync`
- **FibProgrammedNexthops**:
  `unordered_map<CIDRNetwork, WeightedNexthopMap>`
- **WeightedNexthopMap**:
  `unordered_map<IPAddress, uint32_t>`

## DC vs BB Differences

| Aspect | DC (Main.cpp) | BB (MainBB.cpp) |
|--------|---------------|-----------------|
| Init framework | `initFacebook()` | `folly::Init` |
| PeerManager | `PeerManagerVipManager` | `PeerManager` (no VIP) |
| NeighborWatcher | Yes | No |
| FsdbSyncer | Yes | No |
| NetlinkWrapper | No | Yes |
| NexthopHandler | No | Yes |
| NexthopCache | Conditional | Always |
| Thrift framework | 2 ServiceFrameworks | 1 NetServiceFramework |
| Thrift VRF | Single (default) | Dual (default + mgmt) |
| FIB wait | FBOSS agent only | FBOSS + OpenR FIB |

## Startup Order

1. Signal handler thread
2. Wait for FIB service (FBOSS agent ready)
3. Create queues (ribInQ, ribOutQ, neighborEventQ)
4. NeighborWatcher thread (DC) or NetlinkWrapper +
   NexthopHandler threads (BB)
5. Watchdog thread
6. Load Config and PolicyManager
7. RIB thread
8. SessionManager (I/O) thread
9. PeerManager thread
10. Thrift service thread(s)
11. FsdbSyncer start (DC)

## Shutdown Order

1. Stop ServiceFrameworks / ThriftServers
2. Stop RIB
3. Stop NeighborWatcher / NetlinkWrapper /
   NexthopHandler
4. Stop FsdbSyncer (DC)
5. Stop PeerManager
6. Stop SessionManager
7. Stop Watchdog
8. Join all threads

## Draining

**Basic idea**: "Know how to route everything but
advertise that you know nothing."

Example: An FSW being drained continues peering
with RSWs and SSWs. It accepts all routes and adds
to FIB (so it can forward traffic), but does not
advertise routes to neighbors.

**Implementation**:
- BGP restarts with the drained config. No explicit
  withdraw messages are sent — neighbors do stale
  cleanup (reconciliation).
- Interconnect and loopback routes are still
  advertised so the device remains reachable for
  management traffic.

## Wedge Agent Warm vs Cold Boot

- **Warm boot**: Only software components update.
  ASIC continues forwarding (headless). Control
  packets are dropped during update. New agent
  reconciles old vs new state. BGP GR is supported.
- **Cold boot**: Hardware/firmware update requires
  ASIC reboot. No forwarding during boot. Ports
  reset (neighbors see flap). Comes up clean.
