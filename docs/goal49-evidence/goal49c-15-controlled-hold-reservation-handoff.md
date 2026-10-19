# Goal49C-15 — ControlledHold to surface-reservation handoff

## Initial remote reality and scope

`git fetch --prune origin` succeeded. Literal initial live refs:

```
14d63061ef6884733209de5311e2362d7972bbc2	refs/heads/main
1d33cccf5b7d16eb3fd6450b32b13b5510da35e4	refs/heads/goal49-selective-integrated-recovery-rebuild-14d6306
```

The supplied checkout initially had main on local `work`. Its fetch refspec
tracks main only, so Goal49 was explicitly fetched into its tracking ref and
checked out on the canonical branch. Certification work starts from exactly
`1d33cccf5b7d16eb3fd6450b32b13b5510da35e4`, with a clean working tree.

Applied toolchain: `/workspace/ramplab-venv/bin` first in PATH; CMake/CTest
3.31.10, Ninja 1.13.0.git.kitware.jobserver-pipe-1, g++ Debian 14.2.0-19.
`build-goal49c` was absent, so configured Release/Ninja using existing
`build/_deps` dependency sources. Only the focused test target is required.

## Corrected causal contract

The old requirement that ControlledHold remain active until AX202 fully clears
north_south_taxi is superseded by this certification. A maximum 600-second hold
cannot span the intentional 1600-second traversal. That is not its responsibility.

1. The bounded recovery action holds AX101 long enough for AX202 to acquire the
   contested resource, reversing the Tier 0 acquisition order.
2. After the hold expires, normal surface reservations maintain exclusive
   ownership until AX202 releases the resource. AX101 can then proceed.

ControlledHold is a bounded sequencing intervention, not a replacement for
surface reservation control. The selected duration remains 180 seconds; the
600-second cap, 0.1 m/s speed, topology, routes, stands, schedule facts, closure
trigger, safety thresholds, materiality threshold, and candidate cap remain
unchanged. There are no production-code or fixture changes in this pass.

## Explicit authoritative certification

`SurfaceTrafficTest.Goal49ControlledHoldHandsOffToSurfaceReservation` runs the
existing material fixture through normal Integrated planning, with no manual
recovery-action call. It reconstructs all surface edge owners from authoritative
acquisition/release events after every advance. It rejects double acquisition,
release without ownership, and release by the wrong aircraft; owners must be
empty at termination. AX202 has exactly one contested-edge ownership interval.

| Time | Authoritative event/state |
|---:|---|
| 7089 | AX101 hold starts |
| 7241 | AX202 acquires/enters north_south_taxi while hold is active |
| 7269 | Hold releases; AX202 remains reservation owner |
| 7269–8841 | Hold inactive; AX101 remains at (60,-80), with no pushback start and `pushback_area_reserved` wait reason |
| 8841 | AX202 releases; AX101 first permitted movement is pushback, later in event sequence at the same time |
| 8871 | AX101 acquires the shared edge for taxi after pushback completes |

AX101 also acquires a protective pushback reservation at 8841, after AX202's
release. The 8871 acquisition is the subsequent taxi entry; the existing
`taxi_contention_history` helper deliberately distinguishes these phases.
Both acquisitions are checked against AX202's release. This is why certification
reports both first permitted movement and taxi entry rather than conflating them.

Every observed state from AX202's entry through clearance verifies AX202's
ownership, AX101's unchanged safe stand position, and hold active/inactive state
against the exact release boundary. Every observed aircraft pair during the
post-hold interval is checked against the 12 m minimum. Terminal metrics check
zero aircraft/ground/fleet collisions, zero deadlocks, full completion, no
outstanding reservations, 7 acquisitions / 7 releases, unchanged assigned routes,
and no active holds. Event sequence assertions verify release-before-movement
ordering even at the equal 8841 timestamp.

The counterpart runs the identical revealed closure in Reactive mode with no
hold: AX101 enters at 7120 and AX202 at 8720. Integrated reverses these to
AX202 at 7241 and AX101 at 8871. Normal reservations alone preserve A-first;
the hold causes the inversion, and reservations preserve its safe continuation.

## Preserved functional gates

The focused selection is exactly the prior C14 44 test names plus the new
handoff regression (45 tests). It includes private baseline/hold validity and
parent isolation, automatic Integrated selection and accepted replay, Shadow
recommendation without authoritative action, event-by-event Reactive/Shadow
parity, Integrated/Shadow deterministic repeats, no-benefit selectivity, hold
phase/duration validation, and ReassignVehicle/Stand/Resequence regressions.
The original C14 raw evidence is retained as historical evidence.

Unchanged results verified by the focused proof output:

| Measure | Tier 0 / Reactive / authoritative Shadow | Selected hold / Integrated |
|---|---:|---:|
| Worst departure delay (s) | 10090 | 8611 |
| Aggregate departure delay (s) | 13258 | 11779 |
| Completed objectives | 3 | 3 |
| Objective failures / timeouts / turnaround failures | 0 / 0 / 0 | 0 / 0 / 0 |
| Minimum aircraft separation (m) | 12.6 | 40 |
| Surface acquisitions / releases | 7 / 7 | 7 / 7 |

Improvement is 1479 seconds against the unchanged 300-second threshold. Normal
Integrated and Shadow choose `hold:1:180`; Integrated live revalidation accepts
it and its predictions equal realized delays. Shadow applies no authoritative
hold and preserves exact Reactive event history, log, metrics, and snapshots.
Recovery fingerprint remains 850552943722534814. Private evaluation leaves
parent snapshot, history, active holds, and next event unchanged; continuation
matches untouched replay. The 60-second alternative remains unsafe; safe
180/300/600-second alternatives retain deterministic ranking. No-benefit holds
retain Tier 0 and produce no authoritative action.

## Validation and status

**45/45 focused tests passed**, zero failures, 27.74 seconds wall time, using
`ctest --test-dir build-goal49c -R <exact C14 names plus handoff> --output-on-failure -j 2`.
No full CTest suite was run. Summary output is retained in
`goal49c-15-focused-tests.txt`; raw output for all eleven Goal49 tests from that
same run is retained in `goal49c-15-proof-tests.txt`.

The handoff regression passed on its first run. Its post-hold interval contains
1573 observed states, each with no active hold, AX202 ownership, AX101 safely
parked and `pushback_area_reserved`; minimum observed interval separation is
40.0979 m. Global Integrated minimum separation remains 40 m.

Certification status: **GOAL49C CONTROLLEDHOLD COMPLETE**. This tests/docs-only
checkpoint is committed as `Certify Goal49 ControlledHold reservation handoff`
and pushed to the canonical Goal49 branch. Final local/tracking/live equality
and clean-tree checks are performed after publication and reported in the task
response (avoiding a self-referential commit SHA in this document).
Goal49 remains unmerged, KAUO remains DATA_BLOCKED, and Goal50 is not started.
Next is Goal49D material-positive automatic GSE recovery proof, followed by
focused Goal49 certification, eight-family matrix, 120 serial / 120 parallel,
parity, safety certification, prediction analysis, final evidence bundle, and
Goal49 declaration/consolidation/merge.
