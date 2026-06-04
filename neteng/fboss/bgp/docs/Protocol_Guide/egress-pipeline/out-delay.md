# Out-Delay (Route Advertisement Delay)

## TLDR

BGP Out Delay is a config knob that instructs a BGP Speaker to delay advertisements of newly learned routes. When a new route is learned, it is immediately added to the FIB, but advertisement to other neighbors is delayed for at least out-delay seconds. This allows the switch to converge (learn and install the route from other neighbors in the case of ECMP), before we attract traffic to the new route. Out Delay is not applied when modifying route parameters, or when withdrawing a route.

## Use Cases

### 1. Preventing Traffic Funneling During Spine Undrain (Primary Use Case)

#### Setting Up the Context

Consider traffic flowing from a server in one pod (P001) to a server in another pod (P008) in the same DC. In a four-post topology, this traffic is ECMP'd to the four FSWs in P001, then further ECMP'd to the N SSWs in each spine (shown as five in typical diagrams, but could be as many as 36 in production), which forward to the FSWs in P009, which forward to the RSW hosting the server.

Now, let's say one of the SSW spines (S001) has been drained. On the source RSW we will see an ECMP to three FSWs (FSW002 - FSW004), each FSW has ECMP to the N SSWs in its spine, which will forward to FSW002 - FSW004 in the destination pod, which will forward to the RSW hosting the server.

Note that this is the case for multiple prefixes including both VIPs and pod aggregates in the topology, and this kind of traffic accounts for a very high percentage of all traffic in the DC.

![fabric diagram](px/8zWmJ)

> **Note**: The wiki page references diagrams showing traffic flows with red and blue lines. See the [reference wiki page](#references) for visual diagrams.

#### The Problem

Given the above context let's say we now undrain spine S001 and we don't have out-delay functionality. Routing is not atomic. Each SSW in S001 learns the prefix at slightly different times. Let's say SSW001.S001 learns and processes it first and all the other SSWs in the spine are a lot slower. SSW001.S001 adds the route to its FIB and advertises it to FSW001 in all the pods in the DC, which adds it to their FIB and advertise it to the source RSWs to which they connect.

As soon as this route is added in the RSWs, it ECMPs 1/4th of all traffic to FSW001 of each pod, which forwards all this traffic to SSW001.S001 (there is no ECMP yet).

Notice that all the other FSWs have an ECMP of N to the spine, while FSW001, so far, only has one path to the spine, but is still getting 1/4th of all traffic, which as we said, is a very high percentage of all DC traffic.

**This causes a funneling effect.** We have not fully converged in S001, yet we are drawing full capacity on a single SSW, which will be overwhelmed and drop a majority of the traffic.

#### Solution

To solve the above problem, we introduce the concept of out-delay. When we learn a new prefix, we add it to our FIB, but delay advertising it to our neighbor. In our network, we set FSW→SSW and SSW→FSW out-delay to 2 seconds each, but FSW→RSW out-delay is 7 seconds.

In our example, when we undrain spine S001, the SSWs in S001 learn the route from FSW001.P002 in two seconds and add it to their FIB. They wait two seconds and advertise it to FSW001 in P001 (and other pods to which they connect). FSW001 adds the route to its FIB and waits seven seconds before advertising it to the source RSW (and other RSWs in the pod). Seven seconds is significant time for routing, and so, by the time the source RSW learns the route and ECMPs 1/4th of the traffic to FSW001, this FSW has learned the route from all the other SSWs in S001 and has its ECMP path set to the N SSWs, thereby ensuring that traffic is not funneled.

**Typical configuration values:**
- FSW→SSW out-delay: 2 seconds
- SSW→FSW out-delay: 2 seconds
- FSW→RSW out-delay: 7 seconds

#### Peeling Another Layer Off The Onion

The above description glossed over how spine drain / undrain is done. In production, to drain a spine the following is the logic:

* First drain each FSW in the spine. For example, to drain spine 001, then drain FSW001 in all the pods. Draining an FSW means that it stops advertising reachability, even while keeping its FIB intact. So, it stops attracting upstream or downstream traffic from/to the pod. Traffic to/from the pod will use the other three FSWs (assuming four-post)
* When all FSWs in the spine are drained, there is no traffic to the spine. As a precautionary step, also drain the SSWs in the spine, i.e., SSWxx.s001 when draining spine 001

To undrain the spine, we do things in reverse:

* First undrain the SSWs in the spine. Since FSWs are still drained, there is no traffic in the spine
* Then undrain one FSW at a time in that spine. This starts attracting upstream and downstream traffic.

Given that we're only undraining one FSW at a time, do we still have the funneling problem without out-delay? Well, yes. Consider that we undrain FSW001.P001. Let's say SSW001.S001 learns pod aggregates and advertises out neighbors really fast and all other SSWs in the spine are a lot slower. As we said, 1/4th of all traffic to this pod is now attracted to this spine, and there is no ECMP set up. So, what should have been carried over 36 links, is now carried by a single link. No doubt, this is 1/4th the traffic to only a single pod, but it could still be more than what one link can handle, resulting in packet drops.

### 2. Route Flap Dampening

When a peer repeatedly advertises and withdraws a route:

```
Without out-delay:
T0: Receive 10.0.0.0/24 → Immediately advertise to peers
T1: Withdraw 10.0.0.0/24 → Immediately withdraw from peers
T2: Receive 10.0.0.0/24 → Immediately advertise to peers
T3: Withdraw 10.0.0.0/24 → Immediately withdraw from peers
Result: 4 UPDATE messages sent

With out-delay (5 seconds):
T0: Receive 10.0.0.0/24 → Defer advertisement
T1: Withdraw 10.0.0.0/24 → Cancel deferred advertisement
T2: Receive 10.0.0.0/24 → Defer advertisement
T3: Withdraw 10.0.0.0/24 → Cancel deferred advertisement
T5: Timer expires, but prefix withdrawn → No message sent
Result: 0 UPDATE messages sent
```

### 3. Batch Processing During Convergence

During large-scale route updates (e.g., IGP reconvergence):

```
Without out-delay:
- Each route change triggers immediate UPDATE
- 1000 route changes = 1000 UPDATE messages
- Peak CPU usage as each message built individually

With out-delay (10 seconds):
- 1000 route changes accumulate in deferred set
- Single timer expiry processes all changes
- Efficient batching reduces CPU overhead
```

## Important Behaviors

### Initial Dump Handling

We do not delay initial dump. I.e., when our peer has just restarted (we are the helper node) we do not delay the initial dump by out-delay timer. For the most part, prefixes in initial dump are not new — they were heard before. There is a corner case here: the initial dump may have some "new" prefixes that are not yet out-delay seconds old. However, we send all prefixes in the initial dump, including these. This is not a problem. The router that has just come up will be waiting for EoR from all its neighbors anyway.

### When Out-Delay is NOT Applied

Out-delay is only applied to **newly learned routes**. It is NOT applied in these cases:
- Modifying route parameters (e.g., changing attributes)
- Withdrawing a route
- Initial dump to a peer that just restarted

## Architecture

### Data Structures

```cpp
// Per-peer deferred updates map
folly::F14FastMap<folly::CIDRNetwork, DeferredUpdateEntry> deferredUpdates_;

// Set of prefixes with pending timers
std::unordered_set<folly::CIDRNetwork> newDeferredPrefixes_;

// Priority queue of timer expiries
std::priority_queue<AdjRibOutDelayEntry> outDelayPQ_;

// Timer to process expirations
std::unique_ptr<folly::HHWheelTimer::Callback> outDelayTimer_;
```

### Timer Management

Out-delay uses a **priority queue** and **single timer** for efficiency:

```cpp
struct AdjRibOutDelayEntry {
  std::chrono::time_point<std::chrono::system_clock> expiryTimeStamp;
  std::unordered_set<folly::CIDRNetwork> prefixesToProcess;
  bool sendEoR;

  // Ordered by earliest expiry first
  bool operator<(const AdjRibOutDelayEntry& other) const {
    return expiryTimeStamp > other.expiryTimeStamp;
  }
};
```

**How it works**:
1. When route received, add to `deferredUpdates_` with expiry = now + out-delay
2. Add to `newDeferredPrefixes_` set for batching
3. Schedule timer for earliest expiry in priority queue
4. On timer expiry:
   - Pop all entries with expiry ≤ now
   - Move prefixes to packing list
   - Schedule next timer for new earliest expiry

## Flow Examples

### Announcement with Out-Delay

```
1. RIB announces new route for 10.0.0.0/24
   ├─> AdjRib::processRibAnnouncedEntry() called
   ├─> Out-delay configured (e.g., 30 seconds)
   └─> Deferred instead of immediate processing

2. Route added to deferredUpdates_ map:
   deferredUpdates_[10.0.0.0/24] = {
     .entry = RibOutAnnouncementEntry,
     .expiryTime = now + 30s,
     .isWithdrawal = false
   }

3. Prefix added to newDeferredPrefixes_ set:
   newDeferredPrefixes_.insert(10.0.0.0/24)

4. Timer scheduled if not already running:
   scheduleOutDelayTimer()

5. After 30 seconds, timer fires:
   ├─> processOutDelayPrefixes() called
   ├─> Move 10.0.0.0/24 to packing list (attrToPrefixMap_)
   └─> Trigger sender: scheduleSendBgpUpdates()

6. Sender builds and sends BGP UPDATE message
```

### Withdrawal Cancelling Deferred Announcement

```
T0: Route 10.0.0.0/24 received
    └─> Added to deferredUpdates_, timer set for T0+30s

T5: Route 10.0.0.0/24 withdrawn (before timer expiry!)
    ├─> processRibWithdraw() detects prefix in deferredUpdates_
    ├─> Remove from deferredUpdates_ map
    ├─> Remove from newDeferredPrefixes_ set
    └─> No UPDATE message sent (announcement never propagated)

T30: Timer fires
     └─> Prefix not in deferredUpdates_, skipped
     └─> Zero UPDATE messages sent for this churn
```

### Update Replacing Deferred Route

```
T0: Route 10.0.0.0/24 received with nexthop 1.1.1.1
    └─> Deferred, timer set for T0+30s

T10: Same route updated with new nexthop 2.2.2.2
     ├─> Overwrite deferredUpdates_[10.0.0.0/24] with new entry
     ├─> Reset expiry to T10+30s (timer restarted)
     └─> Old announcement never sent

T40: Timer fires
     └─> Send UPDATE with latest nexthop 2.2.2.2 only
     └─> Only 1 UPDATE sent instead of 2
```

## Configuration

### Per-Peer Configuration

Out-delay is configured per BGP peer in the config:

```thrift
struct BgpPeerTimers {
  1: i32 hold_time_seconds;
  2: i32 keep_alive_seconds;
  3: i32 out_delay_seconds;  // ← Out-delay value
  4: optional i32 graceful_restart_seconds;
}
```

Example:
```json
{
  "peer_address": "2001:db8::1",
  "bgp_peer_timers": {
    "hold_time_seconds": 90,
    "keep_alive_seconds": 30,
    "out_delay_seconds": 60  // 60-second out-delay
  }
}
```

### Applying Configuration

```cpp
PeerConfig peerConfig(
    // ... other params
    std::chrono::seconds(60), // outDelay
    // ... other params
);

auto adjRib = std::make_shared<AdjRib>(
    // ...
    std::optional<std::chrono::seconds>(peerConfig.outDelay),
    // ...
);
```

### Default Behavior

- **No out-delay**: Routes processed immediately (standard BGP behavior)
- **Out-delay = 0**: Same as no out-delay
- **Out-delay > 0**: Deferred processing with configured delay

## Update Group Considerations

**Current Status**: Out-delay is **not yet supported** for update groups.

From code comments in `AdjRibGroup.cpp`:
```cpp
// TODO: Out-delay support will be added in future diff
// When implemented: check and remove from deferredUpdates_ here
```

**Per-Peer Mode**:
- ✅ Fully supported with `AdjRib::deferredUpdates_`
- ✅ Timer management in `scheduleOutDelayTimer()`

**Update Group Mode** (Future):
- ⚠️ Not yet implemented
- ⚠️ Group-level deferred updates would need careful design:
  - Peers in group may have different out-delay values
  - Solution: Group by out-delay in UpdateGroupKey

## Integration with Change List

Out-delay interacts with change list processing:

```cpp
void AdjRib::processRibAnnouncedEntry(
    const RibOutAnnouncementEntry& entry) {
  // ...

  if (outDelay_ && *outDelay_ > std::chrono::seconds(0)) {
    // Defer to out-delay timer
    addDeferredUpdate(entry);
    scheduleOutDelayTimer();
    return;  // Don't add to packing list yet
  }

  // No out-delay: immediate processing
  tryUpdateAttrToPrefixMap(/* ... */);
}
```

**Interaction**:
1. Change list consumer populates `deferredUpdates_`
2. Out-delay timer periodically moves entries to packing list
3. Packing list processed by sender coroutine

## Performance Impact

### Memory

**Overhead**:
- `deferredUpdates_` map: ~100 bytes per deferred prefix
- `outDelayPQ_`: ~50 bytes per timer entry
- **Example**: 10,000 deferred prefixes ≈ 1.5 MB

**Mitigation**:
- Deferred entries transient (cleared after delay expires)
- No long-term memory growth

### CPU

**Benefits**:
- Reduced UPDATE message generation during churn
- Better batching of route changes
- Lower peer CPU usage (fewer messages to process)

**Costs**:
- Timer management overhead (negligible with priority queue)
- Map lookups on every route change

### Latency

**Trade-off**:
- Routes delayed by configured out-delay duration
- Faster convergence for transient flaps (avoids churn)
- Prevents traffic funneling by allowing ECMP to converge first

**Typical Values**:
- **Datacenter fabric (spine/FSW)**: 2-7 seconds (prevent funneling, allow ECMP convergence)
- **Conservative (route dampening)**: 60-300 seconds (tolerate long flaps)
- **Aggressive (quick propagation)**: 5-30 seconds (some dampening)

## Statistics

### Metrics

| Metric | Description |
|--------|-------------|
| `deferredUpdates_.size()` | Current deferred prefix count |
| `newDeferredPrefixes_.size()` | Prefixes awaiting batch processing |
| `transientRouteUpdatesSuppressed` | Count of updates eliminated by out-delay |

### Logging

```cpp
XLOGF(DBG3, "Deferred prefixes {} for peer {}",
      newDeferredPrefixes_.size(),
      formatPeerName());
```

## Code References

### Implementation

- **AdjRib.h** (`fbcode/neteng/fboss/bgp/cpp/adjrib/AdjRib.h`)
  - Out-delay data structures
  - `struct AdjRibOutDelayEntry`

- **AdjRib.cpp** (`fbcode/neteng/fboss/bgp/cpp/adjrib/AdjRib.cpp`)
  - `scheduleOutDelayTimer()`: Timer scheduling
  - `cleanUpOutDelay()`: Cleanup on session termination
  - `processRibAnnouncedEntry()`: Deferred vs immediate processing

### Configuration

- **ConfigStructs.h** (`fbcode/neteng/fboss/bgp/cpp/config/ConfigStructs.h`)
  - `PeerConfig::outDelay` field
  - `UpdateGroupKey::outDelay` (for future grouping)

### Testing

- **AdjRibOutBackpressureTest.cpp**
  - `MockOutDelayPrefixes()`: Test helper
  - Timer scheduling and cancellation tests

## References

- [BGP Out Delay Wiki](https://www.internalfb.com/wiki/Net_Systems/Teams/Routing_Protocol/BGP++/Features/Out_Delay/) - Detailed explanation with diagrams of datacenter spine undrain use case
- [Egress Pipeline Overview](egress-pipeline.md)
- [Egress Backpressure](egress-backpressure.md)
- [Change List Integration](changelist-integration.md)
- [Shadow RIB Integration](shadowrib-integration.md)
- [BGP UPDATE Serialization](serialization.md)
