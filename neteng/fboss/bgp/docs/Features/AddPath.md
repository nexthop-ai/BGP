# Background

![](px/8BlvJ)

"Add Path" is a BGP extension that allows the advertisement of more than one
path for a given address prefix, outlined in [RFC 7911](https://datatracker.ietf.org/doc/html/rfc7911). It is activated for a given BGP
session by negotiating the ADD-PATH capability, where each speaker can provide a
capability value indicating that it is able to send, receive, or both send and
receive additional paths. After negotiating, to actually send additional paths
for some prefix, the sending speaker would include unique 4-byte path identifier
values on its outgoing UPDATE messages for the paths. For example, a speaker
could send two paths (to a peer capable of receiving additional paths) for some
prefix by including path ID values of 0 and 1. BGP specifies no requirements on
how the IDs are assigned to additional paths except that they must fit in a
4-byte field. Besides the fact that they are not only identified by sender and
prefix, additional paths are handled more or less the same as a path received
without the Add Path extension in effect.

# Configuration

To enable Add Path for a BGP++ session, each speaker's BGP config must contain
the proper [add_path](https://www.internalfb.com/code/fbsource/[718f27baf7b6104fa0d67debd277cb81242a1b8e]/fbcode/configerator/structs/neteng/fboss/bgp/if/bgp_attr.thrift?lines=40) values: SEND=1, RECEIVE=2, BOTH=3


# Implementation

As mentioned in the background, an additional path is mostly handled as a
regular path would be without the Add Path extension. This means that the
implementation essentially boils down to ensuring that the RIB is capable of
storing and retrieving (ideally in an efficient manor) multiple paths for the
same prefix from the same peer. Then, anything which operates on a specific path
needs to consider path identifiers (e.g., we need things like "evaluate ingress
policy for the path _with ID 0_ and prefix P from peer X" instead of things like
"create a message indicating that the path with prefix P and nexthop N has been
withdrawn", which disregards path identifiers involved). In our case, this
abstract implementation is realized in BGP++ in a few key places:

## Ingress

### AdjRibIn

![](px/8BlzW)

In the AdjRib layer, received paths are divided into two radix trees:
adjRibInLiteTree\_ and adjRibInPathTree\_. As the names suggest, if Add Path is
active for some session, paths received in that session would solely be stored
in adjRibInPathTree\_. The lite tree is a radix tree whose nodes are
AdjRibEntries, while the path tree is a radix tree whose nodes are maps of
uint32_t to AdjRibEntry. Separate trees are used due to the memory savings of
not needing to store maps with only one entry in the Add Path disabled case. The
uint32_t keys of the path tree maps correspond to received path IDs, and allow
for quick identification of existing paths, mostly in the case that they are
withdrawn. For example, a path received with ID 0 and prefix P would be stored
in a map in the path tree node for P, in an entry with key 0.

## Rib

### Storing Received Paths

![](px/8BlBZ)

AdjRibIn will initially process any announcements or withdraws from a given
peer, and then it needs to coordinate this information with Rib via RibIn
messages that carry any updated path information. To facilitate Add Path, RibIn
messages include path identifiers of updated paths. Then, Rib stores any updated
path information in the routeInfos\_ members of the RibEntries corresponding to
any updated paths (that is, the RibEntries for any prefixes associated with
updated paths). Because routeInfos\_ contains path information from all peers,
it is a two-level map where the outer map is keyed by peer ID and the inner maps
are keyed by path ID. For example, let's say the following received paths flow
through AdjRibIn into Rib: paths with IDs 0 and 1 for prefix P from peer X and
paths with IDs 0 and 1 for prefix P from peer Y. Then, assuming none of these
paths are filtered out by ingress policy, routeInfos\_ for the ribEntry for
prefix P, after all RibIn messages are processed, would look like:

![](px/8BlFH)

### Path Selection

When a path update reaches Rib and we have stored it in its corresponding RibEntry's routeInfos\_, we mark its RibEntry as requiring path selection. Whenever we prepare to update Fib with latest routes (via prepareFibProgramming, which happens at a regular cadence according to Rib::fibBatchTimer\_ and also occurs when Rib receives EOR or when a Fib sync request is received), we will invoke path selection for any marked RibEntries. Path selection on a given RibEntry will return a collection of multipaths, a subset of the entry's routeInfos_ which are selected according to configured selection criteria. The best path is the most preferred among these multipaths. To facilitate Add Path, whenever path selection occurs for a RibEntry, each path within multipaths is assigned a path ID (32 bit unsigned integer) if it has not already been assigned one (which would only occur if the same path was selected in a previous round of path selection). We then determine the delta of paths between the current round of selection and the previous one. New and updated paths need to be announced, and paths which were only selected previously need to be withdrawn. RibOut messages (RibOutAnnouncements and RibOutWithdrawals accordingly) are created to convey this information downstream in the update processing pipeline. Similarly to RibIn messages, these RibOut messages contain the path IDs for any updated paths.

### ID Assignment

We essentially use a simple incrementing integer (starting at 0) for path ID assignment on a per-prefix basis, and a given assignment's lifespan is tied to the life of the corresponding path's routeInfo object. So, if a BGP++ speaker is selecting paths for prefix P for the first time, and 3 multipaths are selected, they would be assigned IDs 0, 1, and 2 with no particular order. If the same speaker then selects paths for prefix P2 for the first time and selects 4 multipaths, they would be assigned IDs 0, 1, 2, and 3 with no particular order. Each prefix has its own path ID interval and we draw the lowest un-assigned ID from the interval whenever we have a newly selected path for this prefix. Now, if this same speaker performs path selection a second time for prefix P, and again selects the paths which have IDs 0 and 2 assigned and also selects one new path, the new path would be assigned ID 3 and the re-selected paths would keep their existing assignments, so the resulting selected paths would have IDs 0, 2, and 3, and the path with ID 1 needs to be withdrawn.

BGP imposes a maximum on these path IDs since they must fit in a 4-byte field (roughly 4 billion). Thus, it is technically possible for a path ID interval to be completely exhausted for some prefix. This would never realistically happen since BGP restarts would renew the intervals, but we can handle the scenario nonetheless. Because it's essentially impossible for there to be a selected path for every valid ID at any given time, there must be "freed" IDs we can safely re-assign if indeed an interval were to be exhausted. We perform a simple algorithm for finding the largest free ID gap whenever exhaustion occurs and then we use that gap as the interval for subsequent assignments.

## Egress

### PeerManager

RibOut messages are processed by PeerManager in processRibOutMsgLoop. The contents of the messages are used to maintain the ShadowRib, which essentially stores the latest selected multipaths (and best path) for each RibEntry in aptly named ShadowRibEntries. Within a ShadowRibEntry, multipaths are stored in a map whose keys are path IDs, so that an update to a specific multipath can be identified and processed quickly. Whenever ShadowRibEntries are updated, PeerManager invokes publishChange on its changeListTracker_. AdjRibOutGroups (themselves responsible for egress policy evaluation and packing and serializing outgoing BGP updates) can then consume these changes as able via processShadowRibEntryChange, and then evaluate egress policy on updated paths accordingly before packing and serializing them into outgoing BGP updates with the correct path identifiers which were assigned in Rib upon path selection.

# Additional Reference

[Ingress design doc](https://docs.google.com/document/d/1b6ihe4kaJRVgxP0ueqQufPJUyiTw98cxwNHzUQkW49g/edit?tab=t.0#heading=h.bnxlevkb0i86)

[Egress design doc](https://docs.google.com/document/d/15YJFlGfLq21khoDfAD5wRhnJNYggetggFKO5XL3cpc0/edit?tab=t.0)
