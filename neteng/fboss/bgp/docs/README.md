# BGP++

BGP++ is an in-house BGP implementation written in C++, supporting Meta's
production network — Data Center, Express Backbone. It can be used as a
standalone routing daemon as well as a BGP peer library.

In Data Center, BGP++ runs on all FBOSS devices in both Front-End and Back-End.
As a library, it powers BgpMonitor, BgpCollector, VipInjector and other
services relying on BGP socket I/O functionality.

## Health Dashboard

- [Routing Protocols Dashboard](https://www.internalfb.com/intern/unidash/routing_protocols/)
- [BGP Dashboard](https://www.internalfb.com/intern/unidash/routing_protocols/bgp/)

## Protocol Guide

- [Architecture Deep-Dive](Protocol_Guide/Architecture) — Threads, components, diagram
- [BGP++ Deep Dive](Protocol_Guide/BGP++_Deep_Dive) — Queues, data structures, draining, warm/cold boot
- [FiberSocket and AsyncSocket Integration](Protocol_Guide/io/fibersocket) — Fiber-aware I/O
- [Egress Pipeline](Protocol_Guide/egress-pipeline/egress-pipeline) — Overview of 3 pipeline flavors
  - [Change List Integration](Protocol_Guide/egress-pipeline/changelist-integration) — Pub-sub change propagation
  - [Egress Queue Backpressure](Protocol_Guide/egress-pipeline/egress-backpressure) — Flow control
  - [Out-Delay](Protocol_Guide/egress-pipeline/out-delay) — Route advertisement delay
  - [Serialization](Protocol_Guide/egress-pipeline/serialization) — Wire-format and zero-copy
  - [Shadow RIB Integration](Protocol_Guide/egress-pipeline/shadowrib-integration) — Central route state store
- [Path Scale Limit Analysis](Protocol_Guide/BGP_FBOSS_path_scale_limit_analysis) — Scaling benchmarks
- [BGP Route Limit](Protocol_Guide/BGP_route_limit) — Max-route config and monitoring

## Features

- [Add-Path (RFC 7911)](Features/AddPath) — Multiple paths per prefix
- [UCMP (Unequal Cost Multi-Path)](Features/UCMP) — Receive, program, and advertise link bandwidth weights
- [Graceful Restart / EoR](Features/GracefulRestart) — End-of-RIB timer, stateful GR, stale route cleanup
- [BGP Policy](Features/BGP_Policy) — Policy use cases, match/action matrix, config model

## Build & Run

- [Build and Run BGP++ on FBOSS](Developer_Guide/build_bgp++_on_fboss) — NetCastle, fbpkg, developer hack
- [Build and Run BGP++ on EOS](Developer_Guide/run_bgp++_on_eos) — Arista EOS lab testing

## On-Call

- [Oncall Duties](On-Call/oncall_duties) — Responsibilities, alert severity, SEV handling
- [Qualification and Deployment](On-Call/Qualification_And_Deployment) — CI/CD pipeline, conveyor
- [ODS Time Series Usage](On-Call/ODS_time_series_usage) — Managing ODS quota
- [Runbook](On-Call/runbook) — SSH, package, service, config, logging, CLI, emergency restart, drain/undrain
- [UCMP Runbook](https://www.internalfb.com/wiki/Net_Systems/Teams/Routing_Protocol/BGP++/Features/UCMP/Runbook/) — UCMP config, deployment, monitoring, tooling *(old wiki)*

## Troubleshooting

- [Routing Debug Cheat Sheet](Troubleshooting/routing_debug_cheatsheet) — CLI commands for BGP, Open/R, Wedge-Agent
- [BGP_FIB_NOT_SYNC](Troubleshooting/fib_not_sync) — RIB/FIB desync runbook
- [Frequent Session Flaps](Troubleshooting/frequent_session_flaps) — Session flap investigation
- [SLI Memory Violation](Troubleshooting/sli_memory_violation_runbook) — Memory limit alerts
- [DC Front-End Network Runbook](Troubleshooting/dc_frontend_network_runbook) — Config delta checking
- [Debugging with LLDB](Troubleshooting/lldb_debugging_guide) — Core files and live debugging
- [Troubleshooting (old wiki)](https://www.internalfb.com/wiki/Net_Systems/Teams/Routing_Protocol/BGP++/On-Call/Troubleshooting/) — Unexpected updates, session down *(old wiki)*

## SEV Recaps

- [S608394: Default Route Path Reduction Post FA Update](SEV_Recap/S608394_Default_route_path_reduction_post_FA_Update) (2026-01-07)
- [S614433: Flood of BGPd Unclean Exits on RDSWs](SEV_Recap/S614433_Flood_of_bgpd_unclean_exits_on_RDSWs_in_RCD2) (2026-01-27)
- [S618398: BGP Session Missing After Forwarding Stack Restart](SEV_Recap/S618398_BGP_session_missing_after_forwarding_stack_restart_on_MontBlanc) (2026-02-05)
- [S619541: BGP 0 Received Prefixes After FA Grid Drain](SEV_Recap/S619541_BGP_has_0_received_prefixes_with_session_establishment_after_FA_grid_drain) (2026-02-06)

## Design Reviews

- [2025 Design Review Documents](Design_Review/DesignReview-2025) — Index of 18 design docs
- [Architecture & Design (old wiki)](https://www.internalfb.com/wiki/Net_Systems/Teams/Routing_Protocol/BGP++/Design/) — Fiber-based design philosophy, other BGP implementations *(old wiki)*

## Testing & Emulation

- [BGP Emulation Framework (old wiki)](https://www.internalfb.com/wiki/Net_Systems/Teams/Routing_Protocol/BGP++/Developer_Guide/Emulation/) — Topologies, config templates, netcastle, CI *(old wiki)*

## Developer Guide

- [Developer Guide](Developer_Guide/developer_guide) — Code locations, thrift changes, profiling, logging, binary loading, lab switches

### Code Locations

| Component | Path |
|-----------|------|
| BGP++ daemon (adjrib, peer manager, rib, config) | `fbcode/neteng/fboss/bgp/cpp/` |
| BGP++ thrift definitions | `fbcode/neteng/fboss/bgp/if/` |
| BGP++ fiber library (session, parser, serializer) | `fbcode/neteng/fboss/bgp/cpp/lib/` |
| FBOSS CLI | `fbcode/neteng/fboss/tools/cli/` |

### Build & Test

```bash
# Build daemon

BGP++ in DC:
buck2 build @mode/opt fbcode//neteng/fboss/bgp/cpp:bgpd_cpp

BGP++ in BB:
buck2 build @mode/opt fbcode//neteng/fboss/bgp/cpp:bgpd_cpp_bb

# Run all unit tests
buck2 test fbcode//neteng/fboss/bgp/cpp/...
```

## Onboarding & Learning

### BGP Protocol Resources

- [BGP Crash Course](https://www.internalfb.com/intern/wiki/Net_Systems/FBOSS/BGPD/#Bgp_protocol_crash_course)
- [BGP Tutorial (protocol basics)](http://www.cs.fsu.edu/~xyuan/cis6930/APRICOT2004-BGP00.pdf)
- [Internet Peering Guide](https://www.nanog.org/meetings/nanog51/presentations/Sunday/NANOG51.Talk3.peering-nanog51.pdf)

### Video Recordings

- [BGP++ Design & Implementation @ Neteng Tech Talk (2018)](https://www.internalfb.com/intern/hacktv/view/360508924754524/?timestamp=1809)
- [BGP++ @ Network Summit (2019)](https://www.internalfb.com/intern/hacktv/view/422780368295597/?timestamp=604)
- [BGP Tech Talk Session 1](https://fb.workplace.com/groups/open.routing/permalink/1113677232368949/) — Protocol basics, DC routing, BGP++ design
- [BGP Tech Talk Session 2](https://fb.workplace.com/groups/open.routing/permalink/1115327388870600/) — Operational features in DC
- [BGP Tech Talk Session 3](https://fb.workplace.com/groups/open.routing/permalink/1127342244335781/) — Tooling and troubleshooting

## Legacy Documentation

The [old BGP++ wiki](https://www.internalfb.com/wiki/Net_Systems/Teams/Routing_Protocol/BGP++/) contains additional content not yet migrated:

- [BGP++ on Arista](https://www.internalfb.com/wiki/Net_Systems/Teams/Routing_Protocol/BGP++/Arista/) — Arista EOS integration
