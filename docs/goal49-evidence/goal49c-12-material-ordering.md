# Goal49C-12: material amplification published; safety proof incomplete

Status: **PARTIAL — MATERIAL AMPLIFICATION PUBLISHED**.

The existing RecoveryOutcome comparisons prefer B-first by 1,499 seconds of
worst departure delay, above the unchanged 300-second threshold. Both runs
complete all three aircraft. The proof is **not complete**: B-first reports
zero minimum aircraft separation. Its new safety assertion deliberately fails.
The original ordering regressions remain unchanged and pass.

## Initial remote reality

```text
14d63061ef6884733209de5311e2362d7972bbc2 refs/heads/main
60c73a2456f10ae605af603f0edd69fd2da2750d refs/heads/goal49-selective-integrated-recovery-rebuild-14d6306
```

The branch was checked out at the required `60c73a2456f10ae605af603f0edd69fd2da2750d`
with a clean working tree. The initial workspace branch was `work` at main.
The fetch refspec omitted the Goal49 branch, requiring an explicit tracking-ref
fetch. No user patch was discarded.

## Toolchain and build

PATH: `/workspace/ramplab-venv/bin:$PATH`.

- CMake 3.31.10
- CTest 3.31.10
- Ninja 1.13.0.git.kitware.jobserver-pipe-1
- g++ (Debian 14.2.0-19) 14.2.0

The supplied workspace did not contain `build-goal49c`. It was configured as
Release/Ninja with BUILD_TESTING=ON. No existing build was deleted. Subsequent
changes used the configured incremental build:

```sh
/workspace/ramplab-venv/bin/ninja -C build-goal49c airside_tests -j2
```

No full CTest run was performed.

## Preserved baseline

Before material source edits, both original ordering tests passed:

| Measurement (simulation seconds) | A-first | B-first |
|---|---:|---:|
| AX101 taxi entry | 1041 | 1351 |
| AX202 taxi entry | 1201 | 1161 |
| Follower wait | AX202 1170 | AX101 1310 |
| AX101 departure | 1371 | 1681 |
| AX202 departure | 1541 | 1491 |

## Identical operational facts

The material helper starts with the exact C-11 `surface_ordering_scenario()`.
No graph nodes, edges, geometry or routes change. Exact assigned route nodes
and edges are checked against the original fixture, with zero agent replans.

The narrow search found no configurable per-aircraft surface deadline or speed;
maximum_wait_seconds is an operational-cost normalization scale, not a timeout.
Both departure routes end at the same North Junction handoff. No new timeout,
closure, route, fixture geometry or scoring rule was introduced.

The experiment uses the existing scenario-wide aircraft speed (0.1 m/s), common
AX101/AX202 preparation baselines (7200/6000), and AX101 scheduled departure
13000. The pair is released after earlier servicing and AX303 have cleared.
Preparation baselines align the pair with AX202's longer gate-to-junction travel
under the reduced speed. These facts are identical in both variants. Only
AX101's validated preparation offset (-100 versus +200) changes between runs.
AX101's common later departure schedule absorbs its later B-first taxi, preventing
its delay from erasing AX202's benefit. These are synthetic facts, not KAUO data.

## Material outcomes

| Measurement (simulation seconds unless counts) | A-first | B-first |
|---|---:|---:|
| AX101 shared taxi entry | 7140 | 8871 |
| AX202 shared taxi entry | 8740 | 7241 |
| Follower wait start | AX202 7423 | AX101 7410 |
| Leader final shared-edge release | AX101 8740 | AX202 8841 |
| Follower final shared-edge release | AX202 10340 | AX101 10471 |
| AX101 departure | 13170 | 13170 |
| AX202 departure | 10510 | 9011 |
| AX303 departure | 3398 | 3398 |
| completed | true | true |
| completed_objectives | 3 | 3 |
| objective_failures | 0 | 0 |
| timeouts | 0 | 0 |
| turnaround_failures | 0 | 0 |
| worst departure delay | 10110 | 8611 |
| mean departure delay (RecoveryOutcome.departure_delay_seconds) | 4426 | 3926.333333333333 |
| aggregate departure delay | 13278 | 11779 |
| termination | simulation_finished | simulation_finished |
| final simulation time | 13170 | 13170 |

Final snapshots have all three aircraft Departed, no runway owner and an empty
runway queue. All shared-edge ownership remains serialized. B-first is preferred
by the existing worst-departure-delay comparison, improving 1499 seconds; aggregate
improvement is also 1499 seconds. No failure is swapped. Read-only public wrappers
call the unchanged internal comparison functions used by plan_recovery. No
RecoveryCandidate, private evaluation or recovery-mode integration was added.

## Determinism and safety blocker

Same-seed (42) repeats compare full event histories, full metrics and final
snapshots for equality. Aggregate delays, termination and closest observations
also match. Both outer focused-test iterations produce the same timings and
outcomes, including the same safety failure.

Both variants have zero reported aircraft/aircraft, aircraft/ground and fleet
collisions; seven surface acquisitions and seven releases; zero outstanding fleet
reservations, unfinished requests, held reservation owners or deadlocks; monotonic
event sequences/timestamps; and minimum aircraft/ground separation 76.0316 m.

A-first minimum aircraft separation is 13.1 m. B-first minimum is **0 m**, failing
the retained >=12 m assertion. At t=7241, AX101 is ParkedAtGate at the frozen
South Junction stand and AX202 is Taxiing through that same position. Collision
counters remain zero; they do not establish valid separation.

The comparison test also reproduces the untouched C-11 B-first run using baseline
configuration and asserts exact equality of its events and metrics with the
original helper. It reports the same parked-stand overlap at t=1161 and minimum
separation 0 m. Thus the separation issue is inherited from the authoritative
fixture, not introduced by the amplification. It has not been hidden, excluded,
reclassified as safe, or repaired by changing the frozen topology/routes.

## Focused validation

First diagnostic checkpoint ran exactly the two original ordering tests and the
two material tests: 3 passed, 1 failed (B-first separation).

Final filter, repeated twice:

```sh
build-goal49c/airside_tests \
  --gtest_filter='SurfaceTrafficTest.Goal49NorthSouthTaxiAFirstOrdering:SurfaceTrafficTest.Goal49NorthSouthTaxiBFirstOrdering:SurfaceTrafficTest.Goal49MaterialNorthSouthTaxi*' \
  --gtest_repeat=2
```

Five tests per iteration: four pass and one fails, so 8/10 executions pass.
The only failure is `Goal49MaterialNorthSouthTaxiBFirst` at its minimum-separation
assertion. `Goal49MaterialNorthSouthTaxiOutcomeComparison` passes the actual
ordering and materiality predicates with the default threshold. Each material
test also executes an identical-seed replay and verifies determinism.

Raw output: `goal49c-12-focused-tests.txt`.

## Publication and boundaries

Experiment source was committed and pushed first as
`a10f0712f551ef94e6af17edf4d0cd391ac3268f`, with local/tracking/live equality verified.
The follow-up records exact timing assertions, comparison, determinism and the
unresolved safety gate. This partial fixture is intentionally not a completed
proof. ControlledHold runtime and its existing tests remain intact and unused.
No SHADOW or INTEGRATED_RECOVERY run, main merge or Goal50 work was performed.
KAUO remains DATA_BLOCKED; no production KAUO run was performed.
