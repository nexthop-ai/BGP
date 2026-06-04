# BGP_FIB_NOT_SYNC

[original doc with logs](https://fb.quip.com/LDNVAl6dwnan)

This issue happens when the RIB (bgpd routing table) is different from FIB
(routing table used to forward traffic) for more than 15 minutes.

## Checklist

1.  Go to [ODS chart](https://fburl.com/ods/tpy9zj19), enter the entity (such as
    `fsw004.p088.f01.prn3` in this case) and the keys `fboss.bgp.fib.synced` and
    `fboss.bgp.fib.agentProgrammable`.
    - Notice that for Galaxy switch, we need to find the corresponding line
      card.

      For instance, in T114509105, we could see from the `Causing Events` that
      the problematic line card is `fsw004-lc402.p088.f01.prn3`. Use that as the
      entity.

    - If both keys `fboss.bgp.fib.synced` and `fboss.bgp.fib.agentProgrammable`
      overlap completely, it means that the fib is not synced because bgpd has a
      hard time programming to the `wedge_agent`. We could additionally check
      the key `fboss.agent.fsdbDeltaStatePublisher_agent.disconnects.sum.60` to
      see if the agent is not programmable due to FSDB issue.

    - Log into the switch and check the bgpd log (`/var/facebook/log/bgpd.log`)
      to find

      ```
      Failed to program <some number> withdraw and <some number> update to HW due to: <exception>
      ```

      compare that message with the corresponding failures in the wedge_agent
      log (`/var/facebook/log/wedge_agent.log`).

2.  Check the bgp log to see if bgpd crashed
    - If so, since it lasts for more than 15 minutes, bgpd is usually crashing
      in a loop. Check the bgpd log to verify the issue.

3.  Verify that the FIB is indeed not in sync.
    - Check the bgp table (RIB) and the route table (FIB) by

      ```
      fboss2 show bgp table | grep ">" | wc -l
      fboss2 show route details | grep "client 0" -B 1 | grep "Network Address" | wc -l
      ```

    - Usually the former number is slightly larger than the latter number.
      Compare the results of

      ```
      fboss2 show bgp table | grep ">"
      fboss2 show route details | grep "client 0" -B 1 | grep "Network Address"
      ```

      and find the ones that are in bgp table (RIB) but not in the route table
      (FIB).

    - If some routes are in Rib but not in Fib, they might be the skipped
      locally originated-routes. To verify the case, we need to check the
      startup config (usually at `/dev/shm/fboss/bgpcpp_startup_config`) and see
      if the route is in either networks4 or networks6 without `program_to_fib`
      set to `true`. If so, those routes are deliberately skipped. If the
      difference between RIB and FIB is the skipped routes, the FIB is actually
      in sync with RIB.

## bgpd issues

The bgpd log usually has the stack trace (find the term `stack trace` in bgpd
log) and we could further investigate which component crashed.

Possible causes:

- Queue timeout: the bgpd log shows
  ```
  Failed to program 1 withdraw and 0 update to HW due to: Queue Timeout
  ```
  We need to check the thrift connection between bgpd and wedge_agent
- bgpd crashed in a loop

## wedge_agent is not healthy

If wedge_agent is not healthy, bgpd cannot program the routes and hence RIB and
FIB are inevitably not in sync. Need to check with wedge_agent oncall
fboss_agent

Possible causes:

- `FbossFibUpdateError`: bgpd log shows

  ```
  ThriftHandler.cpp:] addUnicastRoutesInVrf thrift request failed in 1565ms
  ThriftHandler.cpp:] addUnicastRoutes thrift request failed in 1565ms
  FibFboss.cpp:] Failed to program 0 withdraw and 2 update to HW due to: ::facebook::fboss::FbossFibUpdateError
  ```

  Need to check with wedge_agent oncall fboss_agent why it takes so long to
  program to the FIB.

- SAI `NextHopGroupMemberSaiId(0)` failure:

  bgpd log shows

  ```
  Rib.cpp:] Trigger Fib programming with fullSync = false
  FibFboss.cpp:] Failed to program 0 withdraw and 193 update to HW due to: ::facebook::fboss::FbossFibUpdateError
  FibFboss.cpp:] Disconnecting wedge_agent ...
  Rib.cpp:] Fib agent is not connected. Skipping fib batch programming.
  ```

  and the wedge_agent log shows:

  ```
  SaiNextHopManager.cpp:] SaiNextHopManager::addManagedSaiNextHop: 2401:db00:e02e:1623::32
  SaiApiError.h:] [nhop-group] Failed to create sai entity NextHopGroupMemberSaiId(0): (NextHopGroupId: 21475036560, NextHopId: 17179869199, Weight: 17): INVALID PARAMETER
  SaiSwitch.cpp:]  Transaction failed with error : [nhop-group] Failed to create sai entity NextHopGroupMemberSaiId(0): (NextHopGroupId: 21475036560, NextHopId: 17179869199, Weight: 17): INVALID PARAMETER attempting rollback
  ```

- Waiting for FibService to come up: bgpd log shows
  ```
  Main.cpp:] Waiting for FibService to come up...
  ```
  wedge_agent log shows
  ```
  ThreadHeartbeat.cpp:] fbossUpdateThreadthread heartbeat missed!
  ThreadHeartbeat.cpp:] fbossBgThreadthread heartbeat missed!
  ```
