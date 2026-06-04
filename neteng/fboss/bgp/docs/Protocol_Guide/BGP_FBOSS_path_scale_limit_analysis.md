# BGP-FBOSS Path Scale Limit Analysis with Variant Prefix Uniqueness

## Terminology

- **Prefix Limit**: This is the `max_routes` limit received over a single BGP
  session.
- **Path Scale**: This is the combination of both received (adj-rib-in) and
  advertised (adj-rib-out) total path count, which is a result of: number of
  prefixes received/advertised \* number of sessions received from/advertised
  to.
- **Ingress Peers**: This is the BGP peer sessions where the prefixes are
  received from.
- **Egress Peers**: This is the BGP peer sessions where the prefixes are
  advertised to.

## Metric to Use

- **"Path Scale"** is the metric to determine the BGP's max supported limit
  going forward across this document.
- "Path Scale" can be affected by a few factors:
  - Number of prefixes received
  - Number of ingress peers
  - Number of egress peers
  - Policy evaluation result
- "Path Scale" is conveyed via 2 ODS counters:
  - `fboss.bgp.peer.totalRcvdPrefixes` - received prefixes from ingress
  - `fboss.bgp.peer.totalSentPrefixes` - advertised prefixes towards egress

## Table of Benchmarking Result

The following table summarizes the path scale from PROD and DNE-LAB environments
in a stable state with the simulation of production topology. We have reached
the following conclusions in a stable state:

- To compensate for the simplification of LAB setup compared to prod and make us
  confident to operate BGP in a comfortable environment with buffer room, we
  recommend using 40K as the per-peer limit with an estimated max 1.82 Million
  path in DCType-1 FA grid.
- This can be enforced as a max-limit of 20k per address family (v4 and v6) or
  of 40k for one active address family.

**NOTE:**

- The max per-peer (V4 + V6) BGP prefix limit supported without encountering
  memory exhaustion is 80K.
- The theoretical max path scale to not reach the memory limit of BGP is 3.64
  Million paths.

| Environment | Topology        | Possible Max Path Scale (METRIC) | Per-Peer Prefix Limit (V4 + V6) (Configurable) | Peak Memory Utilization |
| ----------- | --------------- | -------------------------------- | ---------------------------------------------- | ----------------------- |
| PROD        | DCType-1 FA/MA  | 1.76 Million                     | 40K                                            | ~2.5G                   |
|             | DCType-F/ILD MA | 2.24 Million                     | 40K                                            | ~2.9G (extrapolated)    |
|             | OLR MA          | 2.24 Million                     | 40K                                            | ~2.9G (extrapolated)    |
| LAB         | DNE-LAB         | **3.64 Million [MAX_LIMIT]**     | **80K [MAX_LIMIT]**                            | ~4.9G                   |
|             |                 | 3.91 Million                     | 86K                                            | CRASHED                 |

_Formula: count of received prefixes _ count of ingress peers + count of
advertised prefixes _ count of egress peers_

## Spurious Prefix Consideration

**NOTE:**

- During the event of SEV, EBs were advertising spurious prefixes.
  - These EB devices advertising spurious prefixes to DC devices may not always
    arrive at the FBOSS (read FAUU/MA) at the same time.
  - Due to the max_route_scale knob on FBOSS, a subset of received prefixes will
    be accepted by FBOSS devices.
  - This filtering of prefixes could range from the best case of FBOSS devices
    accepting the same set of prefixes from all of its upstream peers to a worst
    case of it receiving unique prefixes from each of its upstream peers.
- DNE lab testing reveals that bgpcpp supports a threshold of:
  - > 70% for DC type1 at a per-peer prefix limit of 40k (v4+v6).
  - Theoretically, for MAs in ZAS this threshold is >80% at a per-peer prefix
    limit of 40k (v4+v6).
- In the SEV0:
  - We observed that this number was 92.5% on the worst affected FBOSS device,
    which is well within the acceptable threshold of 80% mark (at 3.6M
    bgp-paths).

## Appendix

- [BGP-FBOSS Path Scale Limit Analysis with Variant Prefix Uniqueness](https://fburl.com/gdoc/u1w3j67f)
- [S475626 Analysis of BGP Per-peer Prefix Limit for EB-UU](https://fburl.com/gdoc/6rfzzevc)
- [FBOSS Agent testing for variation in pct overlapping bad
  prefix](https://docs.google.com/document/d/
