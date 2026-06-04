# BGP Policy

BGP Policy in BGP++ controls route filtering and
attribute manipulation on both ingress and egress
directions. Each peer has inbound and outbound policies
(route-maps), typically applied at the peer group level.

## Use Cases

- **Drain/Undrain** — Insert BGP community attributes
  to de-preference routes from a device being drained.
  Remove communities on undrain.
- **Constrain Route Propagation** — Use community
  attributes to limit route propagation scope, e.g.,
  `TO_FSW` stops propagation at FSW, `THROU_FSW`
  continues further.
- **Backup Group** — Use communities to control
  whether backup routes learned from alternate paths
  are propagated to upper-layer switches.

## Policy Summary

- Each peer has inbound and outbound policies, same
  for all peers of the same type (peer group/template)
- Matches on communities, AS path, prefix lists,
  origin, weight, community count
- Actions: modify communities (standard and extended),
  link bandwidth, AS path (prepend/overwrite/to-set),
  LOCAL_PREF, MED, ORIGIN, NEXT_HOP, weight
- Uses soft-reconfiguration inbound on all peers

## Policy Handling Requirements

- Add, remove, or rewrite community attributes
  (format: `2byte:2byte`)
- View all communities on a prefix
- Filter prefixes based on community attributes
- Support regexp-based expanded community lists for
  matching and deletion

## Policy Manager Architecture

The BGP++ `PolicyManager` extends the template class
`PolicyManagerBase<BgpPath, ...>` defined at
`neteng/routing/policy/PolicyManagerBase.h`. This
base class is shared with OpenR's prefix manager.
Core policy evaluation logic (term walking, default
deny) lives in the abstract base; BGP++ customizes
behavior through concrete match and action subclasses
in `neteng/fboss/bgp/cpp/policy/`.

The Policy Manager sits in the BGP++ processing
pipeline at two points — ingress and egress:

```
Figure 1: Policy Manager in the BGP++ Processing Pipeline

  +----------+     +-----------+     +------------------+     +-----------+     +-----+
  | FiberLib |---->| AdjRibIn  |---->|  Ingress Policy  |---->|  PreOut   |---->| RIB |
  +----------+     +-----------+     | (Policy Manager) |     +-----------+     +-----+
                        |            +------------------+                          |
                      PreIn                                                       |
                                                                                  |
  +----------+     +-----------+     +------------------+     +-----------+        |
  | AdjRibOut|<----| PostOut   |<----|  Egress Policy   |<----|  PostIn   |<-------+
  +----------+     +-----------+     | (Policy Manager) |     +-----------+
                                     +------------------+
```

During initialization, PeerManager consumes the
configuration and creates a Policy Manager with
corresponding policies from the configuration file.
PeerManager creates one AdjRib per peer. Each AdjRib
has a pointer to the Policy Manager.

In the update process, when an update is received from
FiberLib, it is first stored in AdjRibIn. Each update
is logically placed in PreIn first, then processed by
Policy Manager which applies the ingress policy. If
not withdrawn, the update is placed in the PreOut
queue and then into the RIB.

On the outgoing direction, the update is first
logically placed in PostIn. The Policy Manager
applies egress policies, then places the update
in PostOut toward AdjRibOut.

### Policy Manager Modules

```
Figure 2: Policy Manager Module Structure

  +---------------------------------------------------------------+
  |                      Policy Manager                           |
  |                                                               |
  |  +---------------------------+  +---------------------------+ |
  |  | Policy Statement "ingress"|  | Policy Statement "egress" | |
  |  |                           |  |                           | |
  |  |  +---------------------+  |  |  +---------------------+  | |
  |  |  | Term 1              |  |  |  | Term 1              |  | |
  |  |  |  Matches:           |  |  |  |  Matches:           |  | |
  |  |  |   - Community       |  |  |  |   - PrefixList      |  | |
  |  |  |   - PrefixList      |  |  |  |   - AsPath          |  | |
  |  |  |  Actions:           |  |  |  |  Actions:           |  | |
  |  |  |   - Community ADD   |  |  |  |   - AsPathPrepend   |  | |
  |  |  |   - SetLocalPref    |  |  |  |   - Community SET   |  | |
  |  |  |   - Permit          |  |  |  |   - Permit          |  | |
  |  |  +---------------------+  |  |  +---------------------+  | |
  |  |  +---------------------+  |  |  +---------------------+  | |
  |  |  | Term 2              |  |  |  | Term 2              |  | |
  |  |  |  Matches: ...       |  |  |  |  Matches: ...       |  | |
  |  |  |  Actions: ...       |  |  |  |  Actions: ...       |  | |
  |  |  +---------------------+  |  |  +---------------------+  | |
  |  +---------------------------+  +---------------------------+ |
  |                                                               |
  |  Globally Defined Reference Lists:                            |
  |  +----------------+ +---------------+ +--------------------+  |
  |  | Community List | | AS Path List  | | Prefix List        |  |
  |  | name -> values | | name -> regex | | name -> prefixes   |  |
  |  +----------------+ +---------------+ +--------------------+  |
  +---------------------------------------------------------------+

  Multiple terms within a statement have an implicit OR relationship.
  Each term is evaluated independently.
```

### Matches

| Match Type | Description |
|------------|-------------|
| Community | Exact match or regex (e.g., `65520:[0-9]{3}`). Inline or named reference. |
| CommunityCount | Comparison operators: `=`, `>`, `>=`, `<`, `<=`, `!=` |
| Origin | IGP, EGP, INCOMPLETE |
| PrefixList | Exact or flexible prefix match (more specific). Inline or named reference. |
| AsPath | Exact or regex match. Inline or named reference. |
| AsPathLen | Without confed sequence/set |
| AsPathLenWithConfed | Including confed sequence and confed set |
| Weight | Comparison operators on weight value |

### Actions

| Action | Description |
|--------|-------------|
| AsPathPrepend | Prepend up to 255 times, packing into existing AS_SEQUENCE segment |
| Community | SET, ADD, DELETE standard communities (regex supported for DELETE) |
| LbwExtCommunity | SET, DELETE Link Bandwidth Extended Community (mutually exclusive with ExtCommunity within a policy) |
| ExtCommunity | SET, ADD, DELETE Extended Community (mutually exclusive with LbwExtCommunity within a policy) |
| SetLocalPref | Set LOCAL_PREF value |
| SetOrigin | Set origin attribute |
| SetNextHop | Set next-hop address |
| SetMed | Set MED value |
| SetWeight | Set BGP weight value |
| SetAsPath | Overwrite AS path with specified AS list |
| AsPathToAsSet | Convert AS path segments to AS_SET |
| Permit | Allow the prefix |
| Deny | Withdraw the prefix |

### Logic Operators

**Explicit** — For structs like community list:

```json
{
  "communities": ["65000:123", "65000:124"],
  "boolean_operator": "AND"
}
```

The explicit logic operator can also be used in
specifying local preference and AS path list.

**Implicit** — Multiple terms have an OR relationship.
Each term is evaluated independently.

## Supporting References

Policy components can be globally defined and
referenced by name in policy definitions:

- **Policy Statement** — Defined with a name. Each
  peer configuration specifies which policy to use
  via the ingress/egress policy name fields.
- **Community List, AS Path List, Prefix List** —
  Defined globally with a name and used by reference
  in match clauses.

## Code References

| Component | Path |
|-----------|------|
| Policy Manager | `neteng/fboss/bgp/cpp/policy/` |
| Policy Manager Base (shared) | `neteng/routing/policy/PolicyManagerBase.h` |
| Unit Tests | `neteng/fboss/bgp/cpp/tests/PolicyTest.cpp` |
| Thrift Model | `configerator/structs/neteng/bgp_policy/thrift/bgp_policy.thrift` |

## References

- [BGP Policy (old wiki)](https://www.internalfb.com/wiki/Net_Systems/Teams/Routing_Protocol/BGP++/Features/BGP_Policy/)
- [Policy Manager Design (old wiki)](https://www.internalfb.com/wiki/Net_Systems/Teams/Routing_Protocol/BGP++/Design/PolicyManager/)
