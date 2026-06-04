# Oncall duties of routing_protocol

## Overview

Oncall **routing_protocol** needs to stay on top of BGP and Open/R running in
production as well as in FBOSS push canaries, and actively drive resolution of
issues and alerts. The Routing Protocol team has set up monitoring to track BGP
and Open/R status from different perspectives, including binary running status,
noticeable traffic losses, and interested ODS metrics reported from the
binaries. Neighboring teams could also tag **routing_protocol** oncall to
provide insights from routing perspectives.

## What to Monitor

- **Alerts from production Open/R instances**: See
  [this link](https://www.internalfb.com/code/configerator/[master]/source/openr/alerts/)
  for all of the active Open/R alerts.
- **Alerts from production BGP instances**: See
  [this link](https://www.internalfb.com/code/configerator/source/neteng/fboss/monitoring/)
  for all of the active BGP alerts.
- **CI/CT pipeline failure tasks, Oncall tasks, and SEV follow-up tasks**:
  Include FBOSS canary, and EBB netcastle test.
- **BGP tasks**: See [this link](https://fburl.com/unidash/qz2ymjtt) for
  details.
- **Open/R tasks**: See [this link](https://fburl.com/unidash/rl3fzkvo) for
  details.
- **EBB netcastle test failure tasks**
- **User queries on team workplace group**: See
  [this link](https://fb.workplace.com/groups/open.routing) for details.
- **Open source Open/R Issues**
- **DC network push page**: BGP and Open/R Qualification and Build SLA is **1
  day**.
- **Build and qualification pipeline for Open/R**:
  [Conveyor fboss/openr](https://www.internalfb.com/conveyor/fboss/openr/releases)
- **Build and qualification pipeline for BGP**:
  [Conveyor fboss/bgp](https://www.internalfb.com/conveyor/fboss/bgpd/releases)
- **Fboss Integration test pipeline for forwarding_stack bundle**:
  [Conveyor fboss/forwarding_stack_integ_test](https://www.internalfb.com/conveyor/fboss/forwarding_stack_integ_test/releases)
- **Assigned tasks about unit test issues**

## Useful Tools

- [Routing Protocol Alerts](https://fburl.com/am/zdk7wuhz)
- [BGP Dashboard](https://fburl.com/unidash/yl59ifdn)
- [Open/R Dashboard](https://fburl.com/unidash/4bzlwoyu)
- [Oncall Monitoring Hub](https://www.internalfb.com/omh/view/routing_protocol/oncall_profile)
- [Runbook with Troubleshooting Guides](https://www.internalfb.com/runbook/troubleshooting_tips)

## Expectations

### SEV Handling

- Leave everything and focus on sev investigation. When multiple SEVs happened
  at the same time, escalate to other team members for help.
- On-call is expected to respond outside of business hours.
- On-call will be SEV owner by default if the root-cause area is routing team
  related. Inform the team. Try to narrow down the issues by yourself first.
- If you cannot narrow down successfully in 1-2 hours, engage other members,
  especially the domain experts.
- Once the issue is narrowed down, mitigate as soon as possible. Root-cause
  analysis can be done after mitigation.
- During SEV investigation, collect graphs/outputs during investigation.
- Export graphs into pixelcloud as ODS/Scuba data will be lost soon.
- Write SEV report in detail, including timeline, impact, detection, mitigation,
  root-cause, prevention.
- Present and schedule dyrun within the team before SEV review meeting.
- On-call should represent Open/R and BGP in related SEV meetings. These SEVs
  may not be directly related to Open/R or BGP.

### Alerts

- **Critical [sms]**: Leave everything and focus on this. Resolve and mitigate
  issue **as soon as possible**. If not be able to do so by yourself, triage to
  domain PoC at earliest time. Expected to respond outside of business hours.
- **Major [task]**: Resolve or triage within **1 business day**.
- **Minor [email]**: Resolve or triage within **2 business days**.

On-call has the duty to improve alerts such as tuning alert parameters to reduce
false alarms, making sure each alert is actionable and well-documented, or
adding more alerts if needed.

### FBOSS Canary Failures

- Failure to resolve FBOSS canary failures affects the FBOSS combined push. Most
  of the time it's a transient issue. But do log in and confirm before closing
  the task.
- Identify issue and triage to the right owner.
- While working on a fix, we can disable FBOSS canary. Go to
  [this link](https://fburl.com/routing_protocol_am) and click on Disable.
- Make sure the canary device is running with prod version.
- Engage with FBOSS push team.
- Ensure canary-tests are passing by the end of on-call week.

### User Queries

- All external queries should be directed to the on-call person. On-call can
  triage and identify the domain expertise if required.
- User queries include requests to debug issues. They can be from anyone - DC
  Frontend/Backend, EBB, or Github, and for anything(BGP, Open/R, build issues,
  emulator etc.).We usually get them from our team FB group.

### Unit Test Failures

- On-call should attempt to fix flaky unit tests.
- Or identify the owner if it’s out of on-call’s expertise.
- As with alerts, don’t hang on to the task for too long.

### Emulation Test Failures

- Examine the logs to identify the failure and acknowledge the task.
- If not clear, try to reproduce the issue locally in emulation, referring to
  [Emulation Framework](https://www.internalfb.com/intern/wiki/Net_Systems/Emulation/).
- Root cause the issue and re-assign if necessary.
- In case you feel there’s not enough information in root causing the issue,
  think of adding more debugs that can help root cause.
- Involve [Emulation oncall](https://www.internalfb.com/omh/view/emulation) if
  the failure is an infra error.
- Ensure tests are passing at the end of on-call week.
