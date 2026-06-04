# BGP SLI Memory Violation Runbook

## ALERT DOC:

This is a per-dc per-role alert. This alert means there were at least 10 devices
in that role whose memory exceeded 85% of maximum memory of BGPd(5GB). There are
a few scenarios that could lead to this:

- Session flaps
- Route churn
- FIB disconnects
- Route surge
- Memory leak

## INVESTIGATION:

The first step is to investigate the root cause of memory violation.

ODS query with some of the common metrics to root cause -
[ODS Query](https://fburl.com/canvas/qh46nbtd) (Tune the ODS query according to
alert timelines)

To investigate and remediate a particular root cause:

1. **Session Flaps** Investigate why BGP has frequent session flaps. To check
   which peer(s) are flapping:
   `fbagentc --host [switch-name] --port 6909 getCounters | grep sessionStateChanges`
   To see why they are flapping:
   `fbagentc --host [switch-name] --port 6909 getCounters | grep peerError` To
   see ucmp configs:
   `fbagentc --host [switch-name] --port 6909 getCounters | grep ucmp`

## REMEDIATION:

TBD

2. **Route churn** TBD

3. **FIB disconnects** TBD

4. **Route surge** TBD

5. **Memory leak** TBD
