# Build and Run BGP++ on FBOSS Lab Devices

This guide provides instructions for reserving a lab device, building BGP++, and running it on FBOSS lab devices for development, testing, and debugging purposes.

## Reserving a Lab Device

Before deploying BGP++, you need to reserve a FBOSS lab device.

1. Go to [NetCastle Device Pool](https://www.internalfb.com/netcastle/device?pools=dne.test)
2. Find an available device
3. Click **Reserve** to reserve the device for your testing

## Loading Your Binary

There are two methods to load your BGP++ binary onto a lab device:

### Method 1: Build Ephemeral Package (Official)

This is the official method used for rollouts. It takes longer (~20+ minutes) but only requires 2 commands.

#### Step 1: Build the ephemeral package

```bash
fbpkg build -E neteng.fboss.wedge_bgpd --tags ${USER}-test --expire 1d
```

#### Step 2: Deploy using fboss-updater

SSH to the device and update:

```bash
sush2 root@<device_ip>
sudo fboss-updater update bgp --package-id <fbpkg_id>
```

#### Step 3: Verify the package

```bash
ls /etc/packages/neteng-fboss-bgpd/current/
```

### Method 2: Quick Developer Hack (Faster)

This method takes more commands but is much faster for iterative development and testing.

#### Step 1: Build the BGP daemon

```bash
buck2 build @mode/opt neteng/fboss/bgp/cpp:bgpd_cpp --show-output
```

#### Step 2: Locate the built binary

The `--show-output` flag displays the binary path. Example output:

```
fbcode//neteng/fboss/bgp/cpp:bgpd_cpp buck-out/v2/gen/fbcode/e15e45f7d939217c/neteng/fboss/bgp/cpp/__bgpd_cpp__/bgpd_cpp
```

#### Step 3: Copy the binary to the device

Use the path from the build output:

```bash
suscp2 buck-out/v2/gen/fbcode/<hash>/neteng/fboss/bgp/cpp/__bgpd_cpp__/bgpd_cpp root@<device>:/tmp/bgpd_cpp
```

#### Step 4: SSH to the device and swap the binary

```bash
sush2 root@<device>

systemctl stop bgpd
mv /etc/packages/neteng-fboss-bgpd/current/cpp/bgpd_cpp{,.good}
cp /tmp/bgpd_cpp /etc/packages/neteng-fboss-bgpd/current/cpp/bgpd_cpp
systemctl start bgpd
```

## Verifying the Deployment

### Check BGP++ Status

```bash
sudo systemctl status bgpd
```

### Check BGP Sessions

```bash
fboss2 show bgp summary
```

### Check BGP++ Logs

```bash
tail /var/facebook/logs/bgpd.log
```

## Troubleshooting

### BGP++ Not Starting

1. Check logs: `tail /var/facebook/logs/bgpd.log`
2. Verify the binary was copied correctly
3. Check for configuration errors

### Restoring Original Binary (Method 2)

If you need to restore the original binary after using Method 2:

```bash
sush2 root@<device>
systemctl stop bgpd
mv /etc/packages/neteng-fboss-bgpd/current/cpp/bgpd_cpp.good /etc/packages/neteng-fboss-bgpd/current/cpp/bgpd_cpp
systemctl start bgpd
```

## Releasing the Lab Device

When you are done testing, release your lab device reservation via [NetCastle](https://www.internalfb.com/netcastle/device?pools=dne.test).
