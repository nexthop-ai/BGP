# Build and run BGP++ on EOS for testing

This runbook provides step-by-step instructions for deploying and testing BGP++
on Arista EOS devices. This guide is primarily intended for testing custom BGP++
builds or troubleshooting BGP++ issues in lab environments.

## Step 1: Building BGP++ RPM

On the required commit with local changes or latest version, build BGP++ rpm
using:

```bash
/fbsource/fbcode (516c014032)]$ packman --clowntown build --build-local --reason-for-build-local testing neteng/fboss/bgp/cpp/facebook/packman.yml
```

## Step 2: Scp rpm to the device

Command from step1 generates 2 rpms - 1 debug and 1 non-debug. Use the non-debug
version (find the rpm without "debug" in the rpm name).

```bash
scp <rpm> <device_name>:/mnt/flash/
```

Example:

```bash
/fbsource/fbcode (516c014032)]$ scp /data/users/indusuresh/fbsource/fbcode/_rpms/fb-bgpcpp_x86_64/fb-bgpcpp-20250820-11602.x86_64.rpm leb07.labdca1:/mnt/flash/
```

## Step 3: Install the rpm on the device

```bash
ssh devicename

bash
cd /mnt/flash
sudo rpm -Uvh fb-bgpcpp-20250820-11602.x86_64.rpm
```

## Step 4: Restart BGP++

In EOS currently, just shut/no shut on the bgp daemon does not seem to be enough
to restart so kill bgp pid as well:

```bash
bash
ps -ef | grep -i bgp
sudo kill -9 <Bgp pid>
```

List the daemons in the config using (exit from bash if already in bash):

```bash
leb07.labdca1# show running-config section daemon
```

Example daemon configuration:

```
daemon Bgp
   exec /usr/sbin/run_bgp.sh
   no shutdown
daemon EosSdkRpc
   exec /usr/bin/EosSdkRpc --listen 0.0.0.0:9543 --nobfd
   no shutdown
daemon FbConfigAgent
   exec /usr/bin/FbConfigAgent --logtostderr=1 --net_acl_checker_module_enable --net_static_file_acl=/usr/facebook/thrift_acls/FbConfigAgent.json --undefok=net_service_identity,net_static_file_acl,net_acl_checker_module_enable,net_acl_checker_module_enforce,net_auth_checker_kill_switch_file --use_agent_config=1
   no shutdown
daemon FibAgent
   exec /usr/bin/AristaFibAgent --admin_distance=10 --net_acl_checker_module_enable --net_static_file_acl=/usr/facebook/thrift_acls/FibAgent.json --undefok=net_service_identity,net_static_file_acl,net_acl_checker_module_enable,net_acl_checker_module_enforce,net_auth_checker_kill_switch_file --use_agent_config=1 --net_service_identity=FibAgent_lab
   no shutdown
daemon FibAgentBgp
   exec /usr/bin/AristaFibAgent --agent_config_path=/mnt/fb/agent_configs/fib_agent_bgp.conf --admin_distance=10 --net_acl_checker_module_enable --net_static_file_acl=/usr/facebook/thrift_acls/FibAgent.json --undefok=net_service_identity,net_static_file_acl,net_acl_checker_module_enable,net_acl_checker_module_enforce,net_auth_checker_kill_switch_file --use_agent_config=1 --net_service_identity=FibAgent_lab
   no shutdown
```

Shut/no shut on the bgp daemon:

```bash
conf

daemon bgp
shut
no shut
```

## Step 5: Access logs from EOS

```bash
bash

cd /var/log/agents
ls -ltr | grep -i Bgp
```

## Send Announcements:

Add a test prefix to announce, ensure its an openR route if next hop is
required:

```bash
conf

router bgp 64967
  address-family ipv6
    network 4000::/52 route-map ORIGINATE-TEST-PREFIX

route-map ORIGINATE-TEST-PREFIX permit 10
   set ipv6 next-hop 2620:0:1cff:dead:bef1:ffff:fffe:4
```

CLI to check openR routes:

```bash
breeze openr unicast-routes
```

## Send withdrawals:

Remove network statement from the running config to send withdrawals:

```bash
conf
no network 4000::/52
end
```
