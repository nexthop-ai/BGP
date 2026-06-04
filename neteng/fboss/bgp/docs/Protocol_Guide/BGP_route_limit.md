# Overview of BGP Max-route Config Change

## Context

Following the XFN review of the benchmarking results, the Routing and DNE teams,
in collaboration with XFN, have decided to lower the per-peer maximum route
limit to ensure BGP operates safely within the memory constraints as a
short-term measure to prevent SEV0. Please refer to S475626 Network SEV doc -
Routing Breakout for more root-cause details.

## What is BGP max_routes limit

BGP is conceptually divided into three main components: adj-rib-in, rib, and
adj-rib-out. The `max_routes` parameter, sitting in adj-rib-in, sets an upper
limit on the number of routes that can be received over a single BGP session,
and this configuration is applied on a per-session basis. IPv4 and IPv6 sessions
with the same peer can have different limits. An example BGP configuration is as
follows:

```json
"pre_filter": {
  "max_routes": 20000,   // per-peer session limit
  "warning_only": true,
  "warning_limit": 0
}
```

In the Data Center Front End (DC FE), the prefix limit is typically set with two
values:

- [MAX_NUM_PREFIXES_FA](https://fburl.com/code/uf0xyer5)
- [MAX_NUM_PREFIXES_MA](https://fburl.com/code/504n20px)
- [MAX_NUM_PREFIXES_DCTYPEF_ILD_MA](https://fburl.com/code/n55opepd)
- [MAX_NUM_PREFIXES_FABRIC](https://fburl.com/code/rcppg503)

## What happens when max_routes limit is hit

When a BGP speaker encounters a route surge that exceeds the `max_routes` limit,
BGP can exhibit two different behaviors depending on the configured setting.

### [Prod Behavior] Case 1: `warning_only = true`

- **Behavior:**
  - BGP will accept advertised routes up to the "max_routes" limit and discard
    any routes beyond that limit. For example, if a peer advertises 100K routes
    but is configured with a 20K limit, the excess 80K routes will be dropped,
    which has been verified by both the SEV0 scenario and post-DNE lab
    verification.
  - BGP will **_NOT_** terminate sessions between local and remote nodes,
    thereby preventing route churn.
- **Pros:**
  - BGP maintains the accepted "max_routes" safely without causing churn.
- **Cons:**
  - If BGP restarts unexpectedly (e.g., due to a crash) or intentionally (e.g.,
    during a drain), it may receive route advertisements in **_ANY_** order,
    potentially leading to the loss of critical routes.

### Case 2: `warning_only = false`

- **Behavior:**
  - BGP will accept advertised routes up to the "max_routes" limit.
  - If a route exceeds the configured limit, BGP will **_immediately shut
    down_** the session, resulting in the withdrawal of all paths from that
    peer. When the BGP session is re-established, those routes will be
    re-advertised.
- **Pros:**
  - BGP will automatically recover once the excessive route surge condition is
    resolved, thanks to automatic session flapping.
- **Cons:**
  - BGP will withdraw "good" prefixes received before hitting the limit, leading
    to significant route churn.

## Monitoring and Alerting

To prevent unexpected route churn, BGP in the Data Center is configured with
`warning_only=true` across all roles, ensuring that critical routes remain
unaffected even when the high watermark is reached. This setup necessitates
effective monitoring and alerting to notify on-call personnel to respond to
system load changes. The Routing team has identified gaps:

1. An [existing alert detector](https://fburl.com/code/v6e73x5s) is designed to
   trigger when **_75%_** of the limit is reached in the FA layer. However, this
   alert was marked as unhealthy and failed to work during SEV0 due to the
   absence of a correct recipient. This issue is being addressed by
   [D67173994](https://www.internalfb.com/diff/D67173994) and
   [D67161853](https://www.internalfb.com/diff/D67161853).
2. The current alert uses the ODS counter `fboss.bgp.peer.maxPeerRcvdPrefixes`,
   which logs the historical high watermark of received prefixes throughout the
   BGP daemon's lifecycle. This value does not decrease even if the number of
   received prefixes drops. The Routing team plans to update the counter to
   accurately reflect the current maximum received prefixes in the system.
