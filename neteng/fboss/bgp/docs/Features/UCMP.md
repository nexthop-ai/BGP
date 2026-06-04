# Unequal Cost Multi-Path (UCMP)

At its core, the UCMP feature is about **receiving**, **programming**, and
**advertising** weight. The weight is associated with every BGP route received
from or sent to a peer. It is encoded as a [Link Bandwidth Extended Community](
https://tools.ietf.org/html/draft-ietf-idr-link-bandwidth-07) per the RFC.

## Link Bandwidth

Link Bandwidth (LBW) is an extended community attribute that carries the
bandwidth capacity associated with a BGP route's next-hop. It enables routers
to distribute traffic proportionally across next-hops based on their actual
capacity rather than treating all paths equally (ECMP).

## Receive Link Bandwidth

Controls how the link-bandwidth community from a peer is handled on the receive
side. Configured per peer or peer-group.

### Knobs

| Knob | Behavior |
|------|----------|
| `DISABLE` | Do not accept any link bandwidth advertised from peer (default safety) |
| `ACCEPT` | Accept community advertised from peer, if any |
| `SET_LINK_BPS` | Override with the link-bandwidth value from local configuration. Ignore peer-advertised value. |

### Thrift Configuration

```thrift
enum ReceiveLinkBandwidth {
  DISABLE = 0,   // Do not accept any link bandwidth from peer
  ACCEPT = 1,   // Accept community advertised from peer
  SET_LINK_BPS = 2,  // Set link-bandwidth to link bps from config, ignore peer value
}
```

Configured in `BgpPeer` or `PeerGroup` structs via the
`receive_link_bandwidth` field.

### Example

Context: FADU advertises `::/0` with 100G LBW to SSW.
The SSW-FADU link speed is 200G.

| Knob | Result in post-policy AdjRib |
|------|------------------------------|
| `DISABLE` | No LBW value (peer value dropped) |
| `ACCEPT` | 100G (peer value accepted) |
| `SET_LINK_BPS` | 200G (overridden with local link speed config) |

### Use Case: SET_LINK_BPS

SSW wants to do UCMP to FAs based on link speed. Links
to fa01 are 100G, links to fa02 are 200G:

```
::/0
    via fa01-du01.atn6, weight=100G
    via fa02-du01.atn6, weight=200G
```

## Program Link Bandwidth

Programming of UCMP weights in hardware is controlled by a global boolean knob.
When enabled, UCMP weights are derived from link-bandwidth of ECMP paths and
programmed in HW. Receive and advertise functions work regardless of this
setting.

### Configuration

```json
{
  "compute_ucmp_from_link_bandwidth": true,
  "ucmp_width": 65536
}
```

- `compute_ucmp_from_link_bandwidth` — Global knob to enable HW programming
- `ucmp_width` — Quantization bucket size for UCMP weights. Kept high by
default so received weight(32-bit float) fits in 32-bit integer without overflow

**Important:** Weight is associated with next-hops only if **all** ECMP paths
have a link-bandwidth value. If even one path is missing link-bandwidth, ECMP
behavior is used (weight = 0).

## Advertise Link Bandwidth

Controls how link-bandwidth community is advertised to peers. Knobs fall into
two categories:

- **ORIGINATE** — LBW is derived from configuration (peer config or ECMP
path peers)
- **TRANSFORM** — LBW is derived from transforming received communities of ECMP
paths (requires all ECMP paths to have LBW)

### Knobs

| Knob | Category | Behavior |
|------|----------|----------|
| `DISABLE` (default) | — | Do not advertise link bandwidth |
| `BEST_PATH` | Transform | Advertise the link-bandwidth of the best-path |
| `SET_LINK_BPS` | Originate | Set LBW to a specified BPS value from peer/peer-group config (`link_bandwidth_bps`) |
| `AGGREGATE_RECEIVED` | Transform | Sum LBW of all ECMP paths (received from peers). Used for AWP. |
| `AGGREGATE_LOCAL` | Originate | Sum the `link_bandwidth_bps` of all ECMP path peers from config. Used for DMZ/LWP. |
| `RIB_POLICY_LBW` | Originate | Set LBW via external entity through rib-policy API (e.g., FA Controller CTE max-flow-min-cut) |

### Thrift Configuration

```thrift
enum AdvertiseLinkBandwidth {
  DISABLE = 0,
  BEST_PATH = 1,
  SET_LINK_BPS = 2,
  AGGREGATE_RECEIVED = 3,
  AGGREGATE_LOCAL = 4,
  RIB_POLICY_LBW = 5,
}
```

### Full Configuration Example

```json
{
  "peer_groups": [
    {
      "name": "PEERGROUP_FSW_SSW_V6",
      "description": "BGP peering from FSW to SSW",
      "peer_tag": "SSW",
      "advertise_link_bandwidth": 0,
      "link_bandwidth_bps": "100G",
      "receive_link_bandwidth": 1
    }
  ],
  "compute_ucmp_from_link_bandwidth_community": true,
  "ucmp_width": 65536,
  "ucmp_quantizer_config": {
    "min_step_bps": "100G",
    "error_pct_threshold": 0.1,
    "fixed_quantized_bps_list": ["2400G", "3600G"]
  }
}
```

## Link Bandwidth Quantization

An optional transformation of the link-bandwidth value during advertisement.
Motivation: reduce control plane churn from small capacity changes.

### Problem

RSW performs UCMP towards FSWs based on spine capacity. SSW advertises 100G
LBW to FSWs. FSW receives from 36 SSWs and aggregates before advertising to
RSWs. In stable state, RSW receives 3600G from all 8 FSWs. When one SSW goes
down, FSW would advertise 3500G — a small change that causes unnecessary churn
across all RSWs.

### Solution: Non-Uniform Quantizer

A uniform quantizer (fixed step size) generates non-uniform quantization error.
The Non-Uniform Quantizer uses variable step sizes to yield more evenly
distributed errors.

**Algorithm:** Starting at full capacity (e.g., 3600G), reduce step by step.
Each new quantized value is chosen when the quantization error hits the
threshold (e.g., 10%). Step size decreases as capacity decreases.

**Sample quantization (3600G full capacity, 10% error threshold):**

```
Input     Output    Step
3600G  →  3600G
3500G  →  3600G     (400G step, error = 3600/3300 - 1)
3400G  →  3600G
3300G  →  3600G
3200G  →  3200G
3100G  →  3200G     (300G step)
3000G  →  3200G
2900G  →  2900G
...
300G   →  300G
200G   →  200G
100G   →  100G
```

### Quantizer Configuration

```thrift
struct BgpUcmpQuantizerConfig {
  1: string min_step_bps;          // e.g., "100G" — minimum step, typically link speed
  2: double error_pct_threshold;   // e.g., 0.1 — upper bound for quantization error
  3: list<string> fixed_quantized_bps_list;  // e.g., ["2400G", "3600G"] — full capacities
}
```

- `error_pct_threshold` controls quantization level across
  all layers (topology agnostic)
- `fixed_quantized_bps_list` captures all full capacities
  to support (e.g., both 24 and 36 SSW configurations)

### Benefits

1. Reduces control plane and data plane churn from single link failures
2. Addresses HW limitations on next-hop group size (128 Minipack, 64 Wedge100)
3. Single knob (`error_pct_threshold`) controls all layers

## Policy Support (Per Route UCMP)

The UCMP knobs above apply to all routes for a peer.
Per-route UCMP allows customizing behavior for different
route types via BGP policy.

### Example Use Case

Pod aggregates use `AGGREGATE_LOCAL`, but VIP routes
need `AGGREGATE_RECEIVED` from SSW to FADU.

```
match <...> -> allow
  set link_bandwidth LbwExtCommunityActionType::AGGREGATE_RECEIVED
```

### Unified Enum

## Auto Link Bandwidth

When `link_bandwidth_bps` is set to `"auto"`, BGP++
derives the link-bandwidth from the underlying hardware
interface speed instead of a static config value. This
handles partial LAG/Port-Channel failures automatically.

### Example

FAUU connects to two EB devices with 400G LAG
(4 x 100G links). Normally ECMP with equal weights. If
one LAG member fails to one EB, UCMP with weights
`[3, 4]` is used automatically.

### How It Works

1. Resolve peer address to interface via subnet match
2. Read and subscribe to interface speed from HW
3. On speed change, re-run all routes of affected peers through policy

**Limitation:** Only works for physical (P2P) BGP
peerings, not loopback or remote peerings.

## Link Bandwidth Unit

Link bandwidth values use the **RFC standard: bytes per
second**. A 10Gbps interface sets ~1,250,000,000 as the
LBW community value. CLI output formats this for
readability (e.g., "100G").

## Wire Format

The link-bandwidth community is an extended community
(8 octets). The LBW value is 4 octets in **IEEE floating
point** format:

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| Type high=0x40| Type low=0x04 |          ASN                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                  Link Bandwidth Value (float)                 |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

## Code References

| File | Description |
|------|-------------|
| `neteng/fboss/bgp/cpp/lib/BgpStructs.h` | `BgpExtCommunityAsSpecificExtTypeC` — community parsing |
| `neteng/fboss/bgp/cpp/common/BgpAttributes.h` | `BgpAttributes` — get/set/prune LBW community utilities |
| `neteng/fboss/bgp/cpp/common/RouteInfo.h` | `RouteInfo` — UCMP weight and BgpAttributes APIs |
| `neteng/fboss/bgp/cpp/adjrib/AdjRib.cpp` | ReceiveLinkBandwidth and AdvertiseLinkBandwidth implementation. `updateLbwExtCommunity` |
| `neteng/fboss/bgp/cpp/rib/RibEntry.cpp` | `RibEntry::selectBestPath` — weight aggregation |

## References

- [Link Bandwidth RFC](https://tools.ietf.org/html/draft-ietf-idr-link-bandwidth-07)
- [Extended Communities RFC 4360](https://tools.ietf.org/html/rfc4360)
- [UCMP Runbook](https://www.internalfb.com/wiki/Net_Systems/Teams/Routing_Protocol/BGP++/Features/UCMP/Runbook/) — Configuration, deployment, monitoring, and tooling
