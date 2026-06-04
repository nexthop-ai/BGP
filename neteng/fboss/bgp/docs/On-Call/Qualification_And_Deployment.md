# BGP & OpenR Continuous Integration and Deployment

## Continuous Integration

![Continuous deployment workflow](px/8zW4d)

### Pre-Commit Testing

When a diff is submitted to Phabricator, we do the following:

- Run relevant Unit Tests
- Run relevant Emulation Tests (rules defined in [netcastle_emulation_rpp.td](https://www.internalfb.com/code/fbsource/tools/utd/migrated_nbtd_jobs/netcastle/))

### Hourly Testing (Every Eight Hours)

We do 8-hourly testing as part of the FBOSS conveyor pipeline. This pipeline tests a **bundle**, which consists of:

| Package | Description |
|---------|-------------|
| wedge_agent | FBOSS agent |
| bgp | BGP++ daemon |
| openr | Open/R daemon |
| qsfp_service | QSFP service |
| forwarding_stack_spec | Metadata defining the bundle (package versions + config timestamp) |

#### Conveyor Pipeline Stages

| Stage | Description |
|-------|-------------|
| Stage 1 | Build (no-op for us) |
| Stage 2 | Every 8 hours: Pick bundle for emulation and cont canary tests using `latest_contbuild` versions. Run all emulation tests and cont canary in parallel |
| Stage 3 | After Stage 2: Pick last bundle that passed all tests. Run daily canary |
| Stage 4 | Bundles that passed daily canary become valid Release Candidates |
| Stage 5 | Deploy to prod (manually triggered) |

#### Conveyor Resources

| Resource | Link |
|----------|------|
| Conveyor Pipeline Wiki | https://www.internalfb.com/intern/wiki/Neteng/FBOSS/conveyor/ |
| Pipeline History | https://www.internalfb.com/intern/svc/services/fboss/forwarding_stack/conveyor/history/ |

### Nightly Testing

The FBOSS conveyor pipeline does not do stress testing. Stress testing is done in nightly runs.

## Deployment

Deployment is done using Service Foundry as a Combined Push of a bundle (wedge_agent + bgpd + openr + qsfp) by the fboss_push_infra team.

### FAQ

#### Where can I find conveyor pipeline results?

The Home Page for FBOSS Conveyor Push Pipeline is [here](https://www.internalfb.com/conveyor/fboss/forwarding_stack/releases)

To get history of pass/fail results, click on "Bundle History" in the right-hand-side box.

#### Is there a push underway, and what is being pushed?

Go to Home Page of push pipeline. In Left Hand Side tab, click on **Releases**. This shows what is being released and the current stage. You can drill down into previous release attempts.

#### What is the status of the current push?

Go to Release tab. For details on Phase 1, Phase 2, etc., click on **Push Plan** in the Left Hand Side box.

#### What is a bundle?

Starting Dec 2019, we do combined push of:
- wedge_agent
- bgpd
- openr
- qsfp

Plus a timestamp for coop configs. A bundle is a tag to track the binaries and coop config timestamp being tested/pushed.

#### Can I see what changes are going into a release?

1. Go to Release tab
2. Click on **Bundle Contents**
3. In "Diff from previous run on", select "prod"
4. To filter by package (e.g., OpenR), click on the package link in the Packages box

### DC UI Dashboard

View deployment status across DCs: https://www.internalfb.com/intern/network/omni/dc?region&subPath

To see BGP version deployment:
1. Click on the version tab in the left pane
2. Untick "Regions" under *Group by*
3. Choose "table" as *view type* in the top panel

## EBB Workflow

EBB Workflow takes a Release Candidate from the DC workflow and does additional testing in an EBB Conveyor pipeline. The goal is to keep DC and EBB on the same code.

## OSS Workflow

We use `getdeps` framework to build Open/R OSS and all its dependencies.

### How it works

1. Open/R has a manifest in `fbcode_builder` listing:
   - Git repo URL to clone from GitHub
   - `cmake` as the build system
   - All dependencies (first-party like folly, fbthrift; third-party like googletest)
2. `fbcode_builder getdeps` recursively downloads/clones Open/R and dependencies
3. Builds from bottom up and installs

### OSS Resources

| Resource | Link |
|----------|------|
| Open/R Manifest | https://www.internalfb.com/intern/diffusion/FBS/browse/master/fbcode/opensource/fbcode_builder/manifests/openr |
| OSS Build Guide | https://www.internalfb.com/intern/wiki/Net_Systems/OpenR/Developer_Guide/#oss-build |
| getdeps Introduction | https://www.internalfb.com/intern/wiki/Test_your_Open_Source_build_with_getdeps.py |
| getdeps Discussion Group | https://fb.workplace.com/groups/getdeps/ |

### OSS CI

Sandcastle job `oss-openr-linux-getdeps` runs on every diff to prevent Open/R first-party dependencies from breaking OSS.

```
bunnylol sandcastle oss-openr-linux-getdeps
```
