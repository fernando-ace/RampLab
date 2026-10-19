# Goal49C-13 — safe material ordering fixture

## Starting remote reality and toolchain

```text
14d63061ef6884733209de5311e2362d7972bbc2	refs/heads/main
4aad550dd13d6068f65223f46fab3958ad7896f2	refs/heads/goal49-selective-integrated-recovery-rebuild-14d6306
```

Started clean on the canonical Goal49 branch at the required `4aad550` tip,
with both published C-12 commits preserved. The initial workspace was `work`
at main; its fetch refspec included only main. Explicitly fetched the Goal49
tracking ref and added its branch-specific fetch refspec. The first tracking
checkout attempt failed; index/worktree matched the target exactly before
completing the checkout with `--no-track` and setting its upstream. No user
changes were present or discarded.

PATH: `/workspace/ramplab-venv/bin:$PATH`. CMake and CTest 3.31.10;
Ninja 1.13.0.git.kitware.jobserver-pipe-1; g++ (Debian 14.2.0-19) 14.2.0.
`build-goal49c` was absent, so configured once as Release/Ninja with
BUILD_TESTING=ON, reusing existing yaml-cpp and googletest source directories.
Existing build trees were retained. All builds targeted `airside_tests -j2`.

## Actual safety gate and original causal observations

`tests/operations/surface_traffic_test.cpp::validate_material_surface_run`
requires minimum aircraft separation **>=12 m** and aircraft-ground separation
**>=8 m**. Equality passes. `surface_safety.hpp::aircraft_separation` separately
requests a movement hold below **20 m**, and classifies collision below **12 m**.
These thresholds were not changed. A-first's original 13.1 m passes the existing
minimum-separation assertion; it is below the movement hold distance.

`Simulation::handle_surface_tick` samples every pair with physical positions,
including parked aircraft, before the movement-interlock phase filter. Thus
zero collision counters alone do not establish separation safety.

Reproduced with seed 42 using the existing material measurement helper, adding
coordinates to its closest-pair observation:

| Variant | Time | Distance | AX101 | AX202 |
|---|---:|---:|---|---|
| A-first | 7110 | 13.1 m | ReadyForPushback, (100,-80) | Taxiing, (113.1,-80) |
| B-first | 7241 | 0 m | ParkedAtGate, (100,-80) | Taxiing, (100,-80) |

A-first's first minimum occurs as AX202 approaches South Junction along the
Gate A2-to-South Junction edge while AX101 is still at the attachment, just
transitioning from parked to ready/pushback. B-first's overlap occurs when
AX202 arrives at South Junction and acquires `north_south_taxi` at t=7241,
while AX101 is not ready until t=7410. The original focused diagnostic had
one pass (A-first) and one failure (B-first minimum-separation assertion).
Raw observations are retained in `goal49c-13-before.txt`.

## Staging correction

Narrow searches of tests, source, and headers found no gate/stand physical pose
independent of the route node. Gate had only a node, and parked/waiting/pushback
snapshots all resolved to that node. This is the modeling limitation preventing
safe off-taxiway staging with the required unchanged taxi graph and route.

Added optional `Gate::stand_position_m`, defaulting to the old node location.
This uses the existing gate and surface-phase model: parked/waiting aircraft
occupy the stand; normal pushback interpolates continuously from stand to the
taxi attachment over the existing reserved 30-second interval. The normal first
edge/destination reservation is acquired before movement and released/reacquired
for taxi as before. Snapshot positions are the simulator's authoritative
physical positions used by its existing separation metrics and interlocks;
no fixture-specific metric exclusions, safety branches, or counters were added.
Gate snapshots also expose the physical stand location.

The fixture's AX101/A1 stand is **(60,-80)**, 40 m west of South Junction
**(100,-80)**. This is a modest stand offset against the fixture's normal
120 m apron connections: 28 m above the existing 12 m gate, and 20 m above the
20 m movement hold distance. Pushback speed is 40/30 m/s, using normal pushback
timing rather than the reduced taxi speed. AX101 remains at that stand while
waiting for AX202 to release the shared edge, then pushes back to South Junction.

No taxi nodes, edges, geometry, paths, or routes changed. AX101 still taxis
South Junction -> north_south_taxi -> North Junction. AX202 still taxis Gate A2
-> South Junction -> north_south_taxi -> North Junction. Exact assigned route
nodes/edges are asserted against the original routing API, with zero replans.
Speed 0.1 m/s, readiness baselines 7200/6000, A schedule 13000, and B-first
offset +200 are unchanged. The material A-first offset requires the small
-100 to -120 adjustment explained below. The original-speed order inversion
regressions retain their original -100/+200 offsets. Recovery comparison code
and its 300 s threshold are unchanged.

Source checkpoint `93ae547a3e0475c3b50717885d66dc64ee22ca59` was committed and
pushed before post-fix build/validation. Fetch, clean status, local/tracking SHA,
and literal live ref all confirmed that exact commit immediately after push.

## Necessary timing correction after physical staging

The first published stand correction eliminated B-first overlap (40 m), but
at the original A-first release it exposed a pushback/approaching-aircraft
interaction: B stopped while A continued its normal pushback toward the
junction. A-first minimum became 10.7 m, with 55 aircraft collision samples.
This failed validation and was not classified as safe. Both original ordering
regressions and the unchanged 1499-second comparison still passed at this
intermediate checkpoint. Raw diagnostic output is retained separately.

A small fixture-only timing correction is therefore necessary: the material
A-first readiness offset is **-120 s instead of -100 s**. The baseline remains
7200, preparation duration 10, B baseline 6000, B-first offset +200, speed 0.1,
and A schedule 13000. Normal pushback now begins at 7090 instead of 7110,
before B is as close to the junction. No interlock behavior was changed.
A-first taxi entry/release advance by 20 s; B-first is unchanged. This gives
A-first 12.6 m minimum, zero collisions, and retains a **1479 s** B-first
improvement (20 s less than the original 1499 s, well above the unchanged
300 s criterion). The stand retains its full 40 m parked-path clearance.

The immediate timing diagnostic confirmed safety/completion; its sole test
failure was the old exact follower-wait timestamp (7423 vs the measured 7402),
which was then corrected. The minimum from post-advance helper snapshots is
12.7 m, whereas the simulator's pre-interlock tick metric is 12.6 m; the safety
gate uses the smaller authoritative metric. No sample was removed or hidden.

## Final focused validation

Status: **GOAL49C-13 SAFE MATERIAL ORDERING COMPLETE**.

| Measurement (seconds unless noted) | A-first | B-first |
|---|---:|---:|
| AX101 shared-edge entry | 7120 | 8871 |
| AX202 shared-edge entry | 8720 | 7241 |
| Follower wait | AX202 7402 | AX101 7410 |
| Leader shared-edge release | 8720 | 8841 |
| Follower shared-edge release | 10320 | 10471 |
| AX101 departure | 13170 | 13170 |
| AX202 departure | 10490 | 9011 |
| AX303 departure | 3398 | 3398 |
| completed | true | true |
| completed objectives | 3 | 3 |
| objective failures / timeouts / turnaround failures | 0 / 0 / 0 | 0 / 0 / 0 |
| worst departure delay | 10090 | 8611 |
| mean departure delay | 4419.333333333333 | 3926.333333333333 |
| aggregate departure delay | 13258 | 11779 |
| minimum aircraft separation (m) | 12.6 | 40 |
| minimum aircraft-ground separation (m) | 76.0316 | 76.0316 |
| aircraft / ground / fleet collisions | 0 / 0 / 0 | 0 / 0 / 0 |
| surface acquisitions / releases | 7 / 7 | 7 / 7 |
| invalid reservation ownership | 0 | 0 |
| outstanding fleet reservations / held owners | 0 / 0 | 0 / 0 |
| deadlocks / unresolved deadlock | 0 / false | 0 / false |
| termination | simulation_finished | simulation_finished |
| final time | 13170 | 13170 |

All aircraft are Departed, runway owner absent, runway queue empty, with zero
unfinished fleet requests. All-edge reservation ownership assertions verify
acquisition without an existing owner and release by the correct owner.

B-first is preferred and materially improves A-first under the unchanged
`recovery_outcome_better` and `recovery_outcome_materially_improves` wrappers.
Worst-delay and aggregate improvements are both exactly **1479 s**. Reverse
preference and material improvement are false. The threshold remains **300 s**.

The focused staging test observes **1602** parked/waiting AX101 samples while
AX202 taxis toward North Junction, with minimum **40 m**, and **29** interior
pushback positions between the stand and attachment. Its minimum assertions
retain >=12 m and additionally require the full 40 m stand clearance.

Seed-42 repeat assertions pass for full event histories, full metrics, final
snapshots, departure times, minimum separation, every RecoveryOutcome field,
aggregate delay, closest-pair observations, and staging samples. Both outer
test iterations produce identical operational outcomes/order inversion.

Final command (no broad CTest):

```sh
build-goal49c/airside_tests \
  --gtest_filter='SurfaceTrafficTest.Goal49NorthSouthTaxiAFirstOrdering:SurfaceTrafficTest.Goal49NorthSouthTaxiBFirstOrdering:SurfaceTrafficTest.Goal49MaterialNorthSouthTaxiAFirst:SurfaceTrafficTest.Goal49MaterialNorthSouthTaxiBFirst:SurfaceTrafficTest.Goal49MaterialNorthSouthTaxiOutcomeComparison:SurfaceTrafficTest.Goal49MaterialSafeStandAndReservedPushback' \
  --gtest_repeat=2
```

**6/6 tests pass in each iteration: 12/12 executions.** Each material variant
test also runs its own identical-seed repeat. Raw final output is in
`goal49c-13-focused-tests.txt`. The two original ordering regressions keep their
original entry/wait/departure times and both use `north_south_taxi`.

## Boundaries

Goal49 remains unmerged. Main stays at `14d63061ef6884733209de5311e2362d7972bbc2`.
No ControlledHold integration, recovery trigger/candidate/selection changes,
SHADOW run, INTEGRATED_RECOVERY run, or Goal50 work. The existing production
recovery comparison files were not changed. KAUO remains **DATA_BLOCKED**;
no KAUO run, geometry, or permission changes occurred.

The next atomic pass can apply ControlledHold to AX101 on this safe fixture,
then prove private/live B-first reproduction, automatic INTEGRATED selection,
and positive SHADOW parity. This pass stops before that integration.
