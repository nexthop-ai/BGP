# BGP++ Developer Guide

## Code Base

### BGP Daemon and Library

All BGP++ code lives under `fbcode/neteng/fboss/bgp/cpp/`.
The session library (formerly at `fbcode/nettools/bgplib/`)
has been migrated to `fbcode/neteng/fboss/bgp/cpp/lib/`.

```bash
# Build (DC variant)
buck2 build @mode/opt fbcode//neteng/fboss/bgp/cpp:bgpd_cpp

# Build (Backbone variant)
buck2 build @mode/opt fbcode//neteng/fboss/bgp/cpp:bgpd_cpp_bb

# Run all unit tests
buck2 test fbcode//neteng/fboss/bgp/cpp/...
```

### Code Locations

| Component | Path |
|-----------|------|
| BGP++ daemon (adjrib, peer manager, rib, config) | `fbcode/neteng/fboss/bgp/cpp/` |
| BGP++ thrift service definitions | `fbcode/neteng/fboss/bgp/if/` |
| BGP++ fiber library (session, parser, serializer) | `fbcode/neteng/fboss/bgp/cpp/lib/` |
| FBOSS CLI (front-end interface) | `fbcode/neteng/fboss/tools/cli/` |
| BGP config thrift | `fbcode/configerator/structs/neteng/fboss/bgp/bgp_config.thrift` |

## Thrift Changes

When maintaining thrift structures in configerator
and syncing to fbcode, follow the
[Syncing Thrift Files guide](https://www.internalfb.com/intern/wiki/Configerator/User_Guide/Authoring_Configs/Syncing_Thrift_Files_to_Fbcode/).

## CPU & Memory Profiling

[Strobelight](https://www.internalfb.com/intern/wiki/Strobelight-for-services/)
runs on every server and provides on-demand profiling.

### On-Demand Heap Profiling on a Switch

```bash
# Enable heap profiling flag for bgp++
sudo systemctl edit bgpd

# Add before "### Edits below this comment ..."
[Service]
Environment="MALLOC_CONF=prof:true"

# Restart bgpd
sudo systemctl restart bgpd

# Check if heap profiler is enabled
curl [::1]:6909/pprof/heap

# Run on-demand profiling (produces Icicle views)
strobe heap
```

## Logging Levels

| Level | Usage |
|-------|-------|
| `XLOG(ERR)` | Errors (peer disconnected, etc.) |
| `XLOG(INFO)` / `DBG1` | Peer-level events. **DBG1 is enabled by default in production** — do NOT use for high-frequency events. |
| `DBG2` | Message or batch-level events |
| `DBG3` | Prefix-related events |
| `DBG4` | Debug |
| `DBG5` | Verbose |

**Logging convention:** Use `XLOGF(INFO, "format", args)`
format, NOT `XLOG(INFO) << "msg"` stream format.

## Building and Loading Binaries

### Method 1: Ephemeral Package (Official)

```bash
# Build ephemeral package
fbpkg build -E neteng.fboss.wedge_bgpd \
    --tags ${USER}-test --expire 2w

# SSH to device and update
sush2 root@<device>
sudo fboss-updater update bgp --package-id <fbpkg_id>

# Verify
ls /etc/packages/neteng-fboss-bgpd/current/
```

### Method 2: Developer Hack (Fast Iteration)

```bash
# Build locally
buck2 build @mode/opt \
    fbcode//neteng/fboss/bgp/cpp:bgpd_cpp

# Copy binary to device
suscp2 bgpd_cpp root@<device>:/tmp/bgpd_cpp

# SSH and swap binary
sush2 root@<device>
systemctl stop bgpd
mv /etc/packages/neteng-fboss-bgpd/current/cpp/bgpd_cpp{,.good}
cp /tmp/bgpd_cpp \
    /etc/packages/neteng-fboss-bgpd/current/cpp/bgpd_cpp
systemctl start bgpd
```

Wait ~20 seconds and verify with
`fboss2 show bgp summary`.

### Restoring Original Binary

```bash
mv /etc/packages/neteng-fboss-bgpd/current/cpp/bgpd_cpp{.good,}
systemctl restart bgpd
```

## Reserving Lab Switches

Lab switches require hipster group access. Request:
https://www.internalfb.com/intern/permission/group/201399930268581

Reserve a switch via NetCastle:
https://www.internalfb.com/netcastle/device?pools=dne.test

### Accessing a Switch

```bash
sush2 netops@<device>

# Check BGP state
fboss2 show bgp summary
fboss2 show bgp neighbors
```

### Remote CLI Verification

Test CLI changes from your devserver without loading
the binary onto the switch:

```bash
buck2 run fbcode//fboss/cli/fboss2:fboss2 -- \
    -H <device> show bgp summary
```

## Diff Guidelines

- Follow coding guidelines and
  [engineering rules](https://www.internalfb.com/wiki/Network_Routing/Routing_Control/Routing_Protocol/BGP++/)
- Verify changes with unit tests and on lab device
- Capture sample test outputs using pastebin
- Attach test outputs in the diff
- Use bracket tags in title:
  `[BGP++][Feature]: Description`
