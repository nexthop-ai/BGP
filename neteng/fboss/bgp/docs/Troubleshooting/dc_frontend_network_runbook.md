# DC Front-End Network Runbook

## How to check bgp config delta between FBOSS pushes

BGP++(binary) along with config are regularly updated in Meta's data center via
regular FBOSS push cycles with "bundle". Developers may need to check what the
delta is between two bundles. Here is the steps:

- step1: login to the box with `sush2`

```
[xiangxu1121@devvm2662.eag0 ~/local/fbsource/fbcode (a9f995591)]$ sush2 ssw017.s007.f01.prn6
```

- step2: go to `/etc/coop/bgpcpp`

```
[netops@ssw017.s007.f01.prn6.tfbnw.net ~]$ cd /etc/coop/bgpcpp
[netops@ssw017.s007.f01.prn6.tfbnw.net /etc/coop/bgpcpp]$
```

- step3: run `git log` to check `forcePickupDesiredConfigs`, which indicates
  this commit is picked by FBOSS push. Check the timestamp from push bundle to
  double confirm the timestamp are within range.

```
[netops@ssw017.s007.f01.prn6.tfbnw.net /etc/coop/bgpcpp]$ git log | grep forcePickupDesiredConfigs -B4
commit e2d59f7570e775618ae032d4577dd86912aa79e0
Author: unknown <unknown@fb.com>
Date:   Wed Jul 10 14:18:46 2024 -0700

    forcePickupDesiredConfigs thrift request for ALL configs (disruptive=False,coordinated=False,allow_rollback=False)
    --
    commit 13954f77afd6540d0996c85e45f03d4ac34ece50
    Author: unknown <unknown@fb.com>
    Date:   Fri Jun 21 12:10:20 2024 -0700

        forcePickupDesiredConfigs thrift request for ALL configs (disruptive=True,coordinated=False,allow_rollback=False)
```

- step4: run `git diff` to compare the before/after change with commit hash

```
[netops@ssw017.s007.f01.prn6.tfbnw.net /etc/coop/bgpcpp]$ git diff e2d59f7570e775618ae032d4577dd86912aa79e0 13954f77afd6540d0996c85e45f03d4ac34ece50 current
diff --git a/bgpcpp/current b/bgpcpp/current
index 7c18ffddd..02d1e7a84 100644
--- a/bgpcpp/current
+++ b/bgpcpp/current
@@ -3382,13 +3382,6 @@
         "65529:26730"
       ]
     },
-    {
-      "name": "COMM_FABRIC_POD_GEOVIP_PRIVATE_SUBAGG",
-      "description": "fabric pod geovip space aggregate",
-      "communities": [
-        "65524:26730"
-      ]
-    },
     {
       "name": "COMM_FABRIC_POD_RSW_LOOP",
       "description": "rsw loopback",
@@ -5672,7 +5665,7 @@
         "policy_version": "2018100700",
         "policy_entries": [
           {
-            "name": "RULE_SSW_FADU_OUT_VIP_FAIRNESS_690",
...
```

Happy debuging!
