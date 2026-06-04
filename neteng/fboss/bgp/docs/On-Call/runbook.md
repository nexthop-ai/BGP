# BGP++ in DC Runbook

This is a generic runbook covering all cases regardless of whether the
troubleshooting need is during a DR or non-DR scenario. Use this runbook as a
reference and filter if needed for specific scenarios.

## SSH to Device

```bash
sush2 netops@ma04-02.labdca1
```

## Package

BGP binary and related files are packaged into an `fbpkg` identified as
**`neteng.fboss.wedge_bgpd`**. On FBOSS devices, BGP is provisioned by `chef`
and upgraded by DC Network Push.

### Installation Location

```
/etc/packages/neteng-fboss-bgpd/
├── <hash>/
│   ├── bgp_config_selector.py
│   ├── bgpd.service
│   ├── bgpd_wrapper.par
│   ├── cpp/
│   │   └── bgpd_cpp
│   ├── METADATA
│   └── neteng.fboss.wedge_bgpd:<version>.CHECKSUMS
└── current -> /etc/packages/neteng-fboss-bgpd/<hash>/
```

## Service

BGP++ service instance (daemon) is identified by `bgpd`. It is managed by
`systemd` on FBOSS devices.

### Safe Operations

```bash
# Check service status
systemctl status bgpd

# Check the PID (binary name is bgpd_cpp)
pidof bgpd_cpp

# View service configuration
cat /etc/systemd/system/bgpd.service
```

Key properties in the service file:
- `After=wedge_agent.service` — starts after wedge_agent
- `BindsTo=wedge_agent.service` — lifecycle bound to wedge_agent
- `StartLimitIntervalSec=0` — unlimited restarts

### Unsafe Operations

Use these only while **prototyping/debugging** on lab devices.
On production devices, start/stop/restart are handled by automated tooling.

```bash
# Stopping BGP
systemctl stop bgpd

# Starting BGP
systemctl start bgpd

# Restarting BGP (stop & start)
systemctl restart bgpd
```

## Config

`coop` generates the BGP configuration on FBOSS devices. It consumes either the
**Template** or **GSC** to create device config. All generated configurations
are located under `/etc/coop/`. There are multiple BGP configurations generated
— usually two corresponding to LIVE and DRAINED states.

On drain event, the tool directs `/etc/coop/bgpcpp.conf` to DRAINED state
config and restarts the service.

```
/etc/coop/bgpcpp/                    # LIVE config directory
├── current                          # Current active config
├── current.version
├── disruptive
└── release.json
/etc/coop/bgpcpp.conf -> /etc/coop/bgpcpp/current   # Symlink to active config
/etc/coop/bgpcpp_drain/              # DRAINED config directory
├── current
├── current.version
├── disruptive
└── release.json
/etc/coop/bgpcpp-drained.conf -> /etc/coop/bgpcpp_drain/current
/etc/coop/bgpcpp_warm/               # WARM config directory
├── current
├── current.version
├── disruptive
└── release.json
```

### Making a Config Change

Directly modifying configuration content is **STRICTLY PROHIBITED**. However,
for prototyping on lab switches, you can modify the configuration file an
restart the service to pick up the change. All BGP sessions will be
re-established gracefully.

## Logging

```bash
# Live logs via journalctl
journalctl -f -u bgpd

# Tail current logs
tail /var/facebook/logs/bgpd.log

# Historical archived logs
ls /var/facebook/logs/fboss/archive/bgpd*

# Grep on archived logs
zgrep -i "state transition:" /var/facebook/logs/fboss/archive/bgpd.log-2021030*
```

## CLI

Two CLIs are available for inspecting routing state: **`fboss2`** and **`fcr`**.

### FBOSS CLI

```bash
fboss2 show bgp --help
```

Common commands:

| Command | Description |
|---------|-------------|
| `fboss2 show bgp summary` | Show basic info on all BGP sessions |
| `fboss2 show bgp summary --detail` | Show detailed info including UCMP config |
| `fboss2 show bgp neighbors` | Show detailed info on BGP sessions |
| `fboss2 show bgp neighbors <peer-addr>` | Show specific neighbor detail |
| `fboss2 show bgp table` | Dump BGP RIB |
| `fboss2 show bgp table <prefix>` | Show specific prefix |
| `fboss2 show bgp table <prefix> --detail` | Show prefix with full detail |
| `fboss2 show bgp originated-routes` | Show statically configured routes |
| `fboss2 show bgp config` | Show BGP configuration |
| `fboss2 show bgp version` | Show BGP version |

### Cheatsheet: Arista to FBOSS CLI

| Arista | FBOSS |
|--------|-------|
| `show ip\|ipv6 bgp summary` | `fboss2 show bgp summary [--detail]` |
| `show ip\|ipv6 bgp neighbors` | `fboss2 show bgp neighbors` |
| `show ip\|ipv6 bgp neighbors <peer-addr>` | `fboss2 show bgp neighbors <peer-addr>` |
| `show ip\|ipv6 bgp neighbors <peer> advertised-routes` | `fboss2 show bgp neighbors <peer> advertised post-policy` |
| `show ip\|ipv6 bgp neighbors <peer> received-routes` | `fboss2 show bgp neighbors <peer> received pre-policy` |
| `show ip\|ipv6 bgp neighbors <peer> routes` | `fboss2 show bgp neighbors <peer> received post-policy` |
| `show ipv6 bgp` | `fboss2 show bgp table` |
| `show ipv6 bgp <prefix>` | `fboss2 show bgp table <prefix>` |
| `show ipv6 bgp <prefix> detail` | `fboss2 show bgp table <prefix> --detail` |

### FCR CLI

FCR provides a vendor-agnostic interface for accessing
routing and device information.

```bash
fcr
# Entering custom device REPL
# Try ? or <tab> for auto-completion

root@fcr[<device>]> show bgp summary
root@fcr[<device>]> show bgp neighbors
root@fcr[<device>]> show bgp table
```

## Emergency Restart via FCR

**Use only during SEV0/SEV1 scenarios** when there is a need to recover devices
faster without depending on other health checks or scope lock dependencies.
Involve and work with dc_network oncall to have a designated engineer carry out
the execution.

```bash
# Single device
fcr --device=fa006-uu001.snc1 --commands="sudo systemctl restart bgpd"

# Multiple devices from file
fcr --device_file=device_file --commands="sudo systemctl restart bgpd"

# DC wide for all FA-UUs
fcr --device=fa0[0-9][1-9]-uu00[1-9].snc1 \
    --commands="sudo systemctl restart bgpd" \
    --skip_device_limits_check

# Region wide for all FA-UUs
fcr --device=fa0[0-9][1-9]-uu00[1-9].snc[1-9] \
    --commands="sudo systemctl restart bgpd" \
    --skip_device_limits_check

# Fleetwide (NOT recommended — iterate DCs with sleep)
for dc in $(netwhoami counts dc_type!=OLR,serf_state=in_use,cluster_topology=FABRIC \
    -g dc -o dc | grep -o -P '(?<==).*(?=:)'); do
  fcr --device=fa0[0-9][1-9]-uu00[1-9].$dc \
      --commands="sudo systemctl restart bgpd" \
      --skip_device_limits_check
  sleep 10
done
```

### Useful netwhoami Queries

```bash
# List of DCs fleetwide
netwhoami counts serf_state=in_use,cluster_topology=FABRIC,role=FA,lc_type=fa_uu \
    -g dc -o dc | grep -o -P '(?<==).*(?=:)'

# List of Regions fleetwide
netwhoami counts serf_state=in_use,cluster_topology=FABRIC,role=FA,lc_type=fa_uu \
    -g region -o region | grep -o -P '(?<==).*(?=:)'
```

## Drain/Undrain

For production devices, use the `drainer` tool. The
approach below is for development and prototyping only.

```bash
# Drain using fboss_local_drainer
fboss_local_drainer drain

# Check if device is drained
fboss_local_drainer is_drained

# Undrain
fboss_local_drainer undrain
```

For production drain/undrain operations, use the `drainer`
tool with appropriate task references:

```bash
drainer drain <device> --task <task_id>
drainer undrain <device> --task <task_id>
```
