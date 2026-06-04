# BGP++ Architecture Deep-Dive

## Architecture Diagram

![BGP++ Architecture](px/4n9fZ)

Excalidraw link: https://www.internalfb.com/excalidraw/EX162258

## References

- Legacy but golden BGP++ Internals: https://fb.quip.com/aYmVA4Wb3M9B
- BGP RFC: https://tools.ietf.org/html/rfc4271
- Equal-Cost Multipath Considerations for BGP:
  https://datatracker.ietf.org/doc/draft-lapukhov-bgp-ecmp-considerations/
- BGP Graceful Restart RFC: https://datatracker.ietf.org/doc/html/rfc4724
- BGP Add-Path RFC: https://datatracker.ietf.org/doc/html/rfc7911
- BGP++ Reliability Review:
  [BGP++ - Network Resiliency Questionnaire](https://docs.google.com/document/d/1E3Av-c2fK18pHGVpZaJ8uw7sw9cXwdGBNLG12HsXCEU/edit#heading=h.i2ekhjrwbbxe)

## Overview

### BGP Core

The bgp++ core resides in fbcode/neteng/fboss/bgp/cpp, which implements BGP++
functionality (partially) from RFCs mentioned above. Here are some of the main
components:

- **Main**:
  - Create PeerManager, RIB, NeighborWatcher, ServiceFramework threads.
  - Handle SIGTERM and other signals — stop all threads to exit gracefully.

- **PeerManager Thread**:
  - Entry point for anything to do with peer management, which includes:
    - Session UP/DOWN
    - Prefix announcement/withdrawal/EoR/etc.
    - Interface down with arp/ndp entry resolution.

  - **FiberBgpPeerManager** (known counterintuitively as sessionMgr\_ in the
    code)
    - A fiber based management module. See bgplib part.

  - **AdjRib**:
    - Maintain **Adj-Rib-In** and **Adj-Rib-Out** of the BGP RFC, for
      interfacing between the external peers (via bgplib) on the one hand and
      **RIB** on the other via **PeerManager**.
    - AdjRib does NOT have separate thread but share the same thread as
      PeerManager
    - AdjRib is per-peer based and it interacts with FiberBgpPeerManager via
      queues.

  - **PolicyManager**:
    - BGP is very heavily policy driven. There are policies to filter or update
      paths in both directions.
    - When we receive an update from a neighbor (Adj-Rib-In), we can filter or
      update the path before sending it to RIB.
    - Similarly, before announcing something in our FIB to a neighbor
      (Adj-Rib-Out), we can filter or update the path.

- **RIB Thread**
  - Routing Information Base. As the name suggests, it implements RIB
    functionality of the BGP RFC. Maintain a database of all routes received
    from all peers to form **ribEntries\_** (aka, a prefix -> RibEntry map).
    BGP++ does best path selection and creates FIB (Forwarding Information Base)
    including ECMP next-hops.
  - **FibFboss**:
    - Interface with **wedge_agent** — keep RIB in BGP in-sync with FIB in
      **wedge_agent**.
    - On BGP++ initialization (first time start or graceful restart), do a
      “full-sync”, thereafter send add/delete updates.

- **NeighborWatcher Thread**:
  - When a direct link to one of our BGP sessions goes down, we must immediately
    withdraw all routes learned from it, recompute best paths, recompute FIB,
    and update all other neighbors accordingly.
  - NeighborWatcher subscribes to **FSDB** (A central store for all states on
    switch) to get notified when ARP/NDP(Neighbor Discovery) can’t be resolved.
    It further notifies **PeerManager** for session tear-down.

- **ServiceFramework Thread**:
  - **BgpService**:
    - Implement all the thrift functionality to interface with the BGP daemon.
    - BgpService is built on ServiceFramework, so we inherit all its
      functionality, for example TLS (Transport Layer Security)
  - **BgpServiceStream**:
    - Implement all the streaming functionalities to the external subscribers
      via thrift interface.
    - For example, **MPBgpMonitor** relies on this to receive streaming of local
      updates.

- **Config**:
  - Read and parse BGP config under different directories based on different
    state:
    - **LIVE**: /etc/coop/bgpcpp/current
    - **WARM**: /etc/coop/bgpcpp_warm/current
    - **DRAIN**: /etc/coop/bgpcpp_drain/current

- **Stats**:
  - ODS counters definition and API to update them inside modules.

### BGP Library (bgplib)

This bgplib resides in `fbcode/nettools/bgplib` and implements all functionality
to do with the peer via TCP socket. It is the main building block for everything
to do with the external-facing peer.

**Note that bgplib is used, not just in BGP++, but also by VIP Injector and old
Bgp Monitor/Collector etc., so a change here can affect many different
binaries.**

```
Bgplib does NOT carry with its own eventbase. It is the user’s responsibility to
pass in the fiber manager and pump the eventbase accordingly to start async I/O
processing.
```

- **FiberBgpPeerManager**
  - Main entry point in bgplib for management of all peers.
  - It will
    - 1. listen for TCP connections from peers for passive sessions or
    - 2. actively initiate TCP connections to peers for active sessions based on
         configuration.
    - 3. Handle collisions in the case of both active and passive sessions.
  - Calls `FiberBgpPeer->run()` to initiate processing BGP messages to and from
    this peer (see `activeSessionInfo->peer->run()` in
    `FiberBgpPeerManager::runBgpPeer()`).

- **FiberBgpPeer**: Get/send messages over TCP socket to the external peer.
- **FiberBgpParser**: Used by `FiberBgpPeer` to parse messages from external
  peers.
- **BgpSerializer**: Used by `FiberBgpPeer` to send messages over TCP sockets to
  external peers.
- **FiberSocket**: Implement socket level functionality based on
  `folly::AsyncSocket`.
- **Queue**: Implement functionality for RQueue, WQueue, RWQueue.
