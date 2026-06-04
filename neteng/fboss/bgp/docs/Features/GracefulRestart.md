# Graceful Restart (GR) and End-of-RIB (EoR)

BGP Graceful Restart (RFC 4724) allows BGP++ to preserve
forwarding state during a restart, minimizing traffic
disruption. End-of-RIB (EoR) is the mechanism that
signals completion of initial route exchange after a
session is established.

## End-of-RIB (EoR) Behavior

Upon startup, BGP++ starts an **EoR Timer**
(configurable via
[`eor_time_s`](https://www.internalfb.com/code/fbsource/fbcode/configerator/structs/neteng/fboss/bgp/bgp_config.thrift?lines=692),
default = **120 seconds**). Within the timeout,
BGP++ waits for EoR messages from **all static
peers**.

- **All EoR received before timeout:** Best path
  calculation is triggered immediately, and updates
  are sent to peers.
- **Timeout expires (some peers haven't sent EoR):**
  Best path calculation is triggered after the full
  120-second wait.

The timer is scheduled in
[`PeerManager::scheduleTimers()`](https://www.internalfb.com/code/fbsource/fbcode/neteng/fboss/bgp/cpp/peer/PeerManager.cpp?lines=339)
and fires
[`notifyRibEoR()`](https://www.internalfb.com/code/fbsource/fbcode/neteng/fboss/bgp/cpp/peer/PeerManager.cpp?lines=169)
to force initial RIB computation.

### RIB READ_ONLY Mode

Before best path calculation is triggered, the RIB
operates in **READ_ONLY** mode. In this mode, the RIB
only records peer announcements — no best path
computation or route advertisement occurs. This ensures
all routes are collected before making path selection
decisions.

## Stateful Graceful Restart

If BGP++ has previous state from a prior run (e.g., a
peer was already down before the restart), it does
**not** wait for that peer's EoR. This prevents a single
persistently-down peer from delaying convergence on
every restart.

## GR State File

BGP++ persists GR state to a file on disk for crash
recovery:

- The state file includes an **integrity signature**
  — files without a proper termination marker (EoR)
  are rejected on load.
- A **sentinel file** distinguishes planned exits from
  crashes, enabling correct monitoring counters.
- On planned shutdown, the `isDaemonShutdown_` flag
  enables **fast shutdown** via O(1) tree clear
  instead of per-entry cleanup.

## Stale Route Cleanup

After restart, routes learned from the previous session
are marked **stale**:

- Stale entries are marked **in-place with flag bits**
  for O(stale) cleanup, rather than O(total) tree
  walk.
- Once new routes are received from the peer (or the
  stale timer expires), stale entries are removed.

## GR Capability Negotiation

GR capability is negotiated during the BGP OPEN message
exchange. BGP++ advertises and expects:

- **Restart Time** — configurable per peer, advertised
  in the GR capability
- **Address Family preservation** — indicates which
  AFI/SAFI forwarding state is preserved
- **Restart State bit (R-bit)** — set when the speaker
  has restarted and forwarding state was preserved

## GR-Eligible vs Non-GR Reset Reasons

When a session goes down, BGP++ distinguishes between
GR-eligible and non-GR-eligible reset reasons:

- **GR-eligible:** HOLD_TIMER_EXPIRED, TCP connection
  reset, peer restart with R-bit set — routes are
  retained as stale during restart
- **Non-GR-eligible:** Administrative shutdown,
  NOTIFICATION received, configuration change —
  routes are immediately withdrawn

## References

- [RFC 4724: Graceful Restart Mechanism for BGP](https://tools.ietf.org/html/rfc4724)
- [Architecture Deep-Dive](../Protocol_Guide/Architecture) — GR in the context of BGP++ architecture
