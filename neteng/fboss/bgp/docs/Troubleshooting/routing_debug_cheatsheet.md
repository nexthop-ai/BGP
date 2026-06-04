# Routing Protocol Debug Cheat Sheet

## BGP State

To debug a BGP problem, the following information should be made available to
the routing oncall. Once this information is collected, there is no requirement
to keep the device in a problematic state, i.e., BGP on the box can be restarted
or even the entire box can be re-provisioned as part of repair/remediation.

| Serial # | Information                                             | Command                                                                                             | Example                                                               |
| -------- | ------------------------------------------------------- | --------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------- |
| 1.       | Binary version                                          | `fboss2 show version bgp` or `fboss2 show bgp version`                                              | [link](https://www.internalfb.com/phabricator/paste/view/P1894613815) |
| 2.       | Status of BGP service                                   | `systemctl status bgpd`                                                                             | [link](https://fburl.com/phabricator/6qdidrdb)                        |
| 3.       | BGP logs                                                | `/var/facebook/logs/bgpd.log` or `/var/facebook/logs/fboss/archive/bgp*`                            | [link](https://fburl.com/phabricator/hghgunwq)                        |
| 4.       | BGP Config                                              | `fboss2 show config running bgp` or `fboss2 show bgp config`                                        | [link](https://www.internalfb.com/intern/paste/P1894614177/)          |
| 5.       | Rib-policy config                                       | `fboss2 show bgp rib-policy [cps/crf/cte]`                                                          | [link](https://fburl.com/phabricator/lsnkth2r)                        |
| 6.       | Summary                                                 | `fboss2 show bgp summary`                                                                           | [link](https://www.internalfb.com/phabricator/paste/view/P1894611331) |
| 7.       | Originated routes                                       | `fboss2 show bgp originated-routes`                                                                 | [link](https://www.internalfb.com/phabricator/paste/view/P1894613132) |
| 8.       | Neighbor                                                | `fboss2 show bgp neighbors {peerAddr}`                                                              | [link](https://fburl.com/phabricator/7xp7t6ei)                        |
| 9.       | Adjacency-Rib-in (pre inbound policy routes from peer)  | `fboss2 show bgp neighbors {peerAddr} received pre-policy`                                          | [link](https://fburl.com/phabricator/pywq7iht)                        |
| 10.      | Adjacency-Rib-in (post inbound policy routes from peer) | `fboss2 show bgp neighbors {peerAddr} received post-policy`                                         | [link](https://fburl.com/phabricator/el0zr522)                        |
| 11.      | Routes rejected by inbound policy                       | `fboss2 show bgp neighbors {peerAddr} received rejected`                                            | [link](https://fburl.com/phabricator/zmspsjh1)                        |
| 12.      | Local RIB (routing table)                               | `fboss2 show bgp table detail`                                                                      | [link](https://fburl.com/phabricator/k24qykjq)                        |
| 13.      | Adjacency-RIB-out (pre outbound policy routes)          | `fboss2 show bgp neighbors {peerAddr} advertised pre-policy`                                        | [link](https://fburl.com/phabricator/e5z0fyct)                        |
| 14.      | Adjacency-RIB-out (post outbound policy routes)         | `fboss2 show bgp neighbors {peerAddr} advertised post-policy`                                       | [link](https://fburl.com/phabricator/96e2trxq)                        |
| 15.      | Routes rejected by outbound policy                      | `fboss2 show bgp neighbors {peerAddr} advertised rejected`                                          | [link](https://fburl.com/phabricator/ny4zkh7t)                        |
| 16.      | LLDP                                                    | `fboss2 show lldp`                                                                                  |                                                                       |
| 17.      | ARP                                                     | `fboss2 show arp`                                                                                   |                                                                       |
| 18.      | NDP                                                     | `fboss2 show ndp`                                                                                   |                                                                       |
| 19.      | Interfaces                                              | `fboss2 show interface`                                                                             |                                                                       |
| 20.      | Kernel logs                                             | `/var/log/messages*`                                                                                | [link](https://fburl.com/phabricator/cnuy3q1k)                        |
| 21.      | Network event logs                                      | `/var/facebook/logs/fboss/network_events.log` or `/var/facebook/logs/fboss/archive/network_events*` | [link](https://fburl.com/phabricator/ey5o2whf)                        |
| 22.      | Techsupport                                             | `fboss2 show bgp techsupport`                                                                       | [link](https://fburl.com/everpaste/ywjxtoib)                          |

## Open/R State

Open/R is the link-state protocol which is currently running in DCType-1
FrontEnd network as well as Express BackBone. The following table provides
common troubleshooting CLI to check Open/R operation and state in the fleet.
Today Open/R has:

- **Breeze tech-support** to collect all related information from internal
  modules.
  - Example run: [Everpaste Example](https://fburl.com/everpaste/83rvu960)
  - **Breeze openr validate** to do initial sanity check/triage: -
    [Open/R Validation](https://fb.workplace.com/groups/open.routing/permalink/1600627867007214/)

Other than that, if you would like to peek into details, you can do the
following commands:

| Serial # | Information                                | Command                                                                     | Sample                                         |
| -------- | ------------------------------------------ | --------------------------------------------------------------------------- | ---------------------------------------------- |
| 1.       | Open/R binary version                      | `breeze openr version`                                                      |                                                |
| 2.       | Status of Open/R service                   | `systemctl status openr`                                                    |                                                |
| 3.       | Open/R logs                                | `/var/facebook/logs/openr.log` or `/var/facebook/logs/fboss/archive/openr*` |                                                |
| 4.       | Open/R Config                              | `breeze config show`                                                        |                                                |
| 5.       | Open/R Auto-discovered Neighbors (UDP)     | `breeze spark neighbors [-details]`                                         |                                                |
| 6.       | Open/R Discovered Links                    | `breeze lm links`                                                           |                                                |
| 7.       | Open/R KvStore Peers (TCP)                 | `breeze kvstore peers`                                                      |                                                |
| 8.1      | Open/R KvStore K-V pairs                   | `breeze kvstore keys [--ttl]`                                               |                                                |
| 8.2      | Open/R KvStore K-V pairs                   | `breeze kvstore keyvals {one particular key}`                               |                                                |
| 9.       | Open/R KvStore Summary                     | `breeze kvstore summary`                                                    |                                                |
| 10.      | Open/R Areas                               | `breeze kvstore areas`                                                      |                                                |
| 11.      | Open/R KvStore Streaming                   | `breeze kvstore snoop --area {area_name} [--ttl]`                           | [link](https://fburl.com/phabricator/nddyov0v) |
| 12.1     | Open/R link-state topology                 | `breeze decision adj`                                                       | [link](https://fburl.com/phabricator/765kh4mw) |
| 12.2     | Open/R link-state topology                 | `breeze decision adj --nobidir`                                             | [link](https://fburl.com/phabricator/89qo7em3) |
| 13.      | Open/R received routes pre-SPF calculation | `breeze decision received-routes`                                           |                                                |
| 14.      | Open/R post-SPF calculated Routes          | `breeze decision routes`                                                    |                                                |
| 15.      | Open/R partial adjacencies                 | `breeze decision partial a {area}`                                          | [link](https://fburl.com/phabricator/y56a55pt) |
| 16.      | Open/R Fib Routes to be programmed         | `breeze fib unicast-routes`                                                 |                                                |
| 17.      | Open/R Routes Programmed by wedge-agent    | `breeze fib routes-installed`                                               |                                                |
| 18.      | Open/R Fib routes streaming                | `breeze fib snoop`                                                          | [link](https://fburl.com/phabricator/div9j4ah) |
| 19.      | Open/R Originated routes                   | `breeze prefix orig`                                                        |                                                |
| 20.      | Advertised Routes Pre Inter-Area Policy    | `breeze prefix adv pre {area_name} {prefix}`                                | [link](https://fburl.com/phabricator/wkkjek7i) |
| 21.      | Advertised Routes Post Inter-Area Policy   | `breeze prefix adv post {area_name} {prefix}`                               | [link](https://fburl.com/phabricator/gjd2qc8x) |
| 22.      | Routes Rejected by Inter-Area Policy       | `breeze prefix adv rej {area_name} {prefix}`                                | [link](https://fburl.com/phabricator/cq29ib7s) |

## Wedge-Agent State

| Serial # | Information                      | Command                              | Sample |
| -------- | -------------------------------- | ------------------------------------ | ------ |
| 1.       | Routes programmed by wedge-agent | `fboss2 show route details <prefix>` |        |
