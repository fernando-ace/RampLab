# Goal49D material-positive automatic GSE recovery

Goal49D establishes a synthetic material-positive ReassignVehicle proof using the
unchanged production recovery architecture and 300-second materiality threshold.
The first source checkpoint was published before extended validation. This document
and the accompanying raw output record the final automatic, parity, and safety proof.
Goal49 remains unmerged; this is not final Goal49 certification.

## Remote reality and toolchain

Initial literal live refs:
```
14d63061ef6884733209de5311e2362d7972bbc2	refs/heads/main
85e6e32adf7fb52be466adba8f90cbf819bab55e	refs/heads/goal49-selective-integrated-recovery-rebuild-14d6306
```
`git ls-remote origin refs/heads/goal4` returned no ref; the exact canonical branch
query above supplied the Goal49 ref. Work started at that exact tip. The initial
checkout fetched only main, so the canonical Goal49 ref was explicitly fetched and
added to the local fetch/tracking configuration.

`/workspace/ramplab-venv/bin` was first in PATH. CMake and CTest: 3.31.10;
Ninja: 1.13.0.git.kitware.jobserver-pipe-1; g++: Debian 14.2.0-19.
Reused `build/` (Release, Ninja), configuring with the venv CMake and building only
`airside_tests`. No full CTest run or experiment matrix was started.

## Fixture and operational mechanism

The existing `TurnaroundTest.AutomaticIntegratedGseRecoveryDiagnostic` is upgraded
to a raw materiality/isolation gate. The automatic proof is
`TurnaroundTest.IntegratedGseRecoveryMateriallyRescuesStrandedCriticalTask`.
Both use `material_gse_scenario()` in `tests/operations/turnaround_test.cpp` and seed 42.

The foundation is `turnaround_flight_bank_outage.yaml`, retaining only AX101 /
TO-AX101 and removing its unrelated task-duration disruption. All modes receive
identical facts. Scheduled departure is 900 seconds and target off-block is 800.

| Item | Fact |
| --- | --- |
| Critical task | Task 3, BaggageUnload, required capability `baggage`, 240 s service |
| Source | Vehicle 2, BaggageCart-1, dispatched at 121 s |
| Outage / recovery epoch | 130 s; task is Assigned to vehicle 2, dependencies complete |
| Preferred backup | Vehicle 3, BaggageCart-Backup, healthy, idle, uncommitted |
| Other legitimate backup | Vehicle 4, Near baggage cart, healthy, idle, uncommitted |
| Normal reactive choice | Vehicle 4: approach 221.7 m versus vehicle 3's 299.7 m |
| Access closure | New synthetic edge 18 closes at 131 s, reopens at 800 s |
| Unaffected backup route | Backup Depot → Service Depot → North Junction → Gate A1 Baggage Stand |
| Critical dependency chain | Deboarding → baggage unload → baggage load (also needs cleaning) → pushback preparation → readiness/departure |

The ordinary dispatcher minimizes approach distance, so it legitimately picks cart
4, which waits for its access road. Integrated private replay finds cart 3's
unaffected route better. No mode-specific outage, safety, failure, dispatch, or
comparison behavior was introduced. The replacement travels under the existing
fleet motion model; it is not teleported.

The historical fixture's only backup was selected by normal reactive recovery,
so Tier 0 and reassignment tied at 109 s. Adding real operational choices and a
shared timed access closure creates the material difference. A longer closure
(reopening at 2400 s) was also explored: Tier 0 still completed all objectives
with no failures and 1669 s delay through existing recovery. A categorical failure
would require more artificial failure facts; the final focused proof therefore
uses the expressly permitted delay criterion and preserves successful REACTIVE
fallback. The final fixture reopens at 800 s.

## Raw private hard gate

The raw diagnostic stops after the actual outage event at the same 130 s boundary,
then separately evaluates Tier 0 and one valid ReassignVehicle candidate. It asserts
that the unchanged production materiality function accepts the candidate before
the separate automatic mode proof is run.

| Field | Tier 0 / current_plan | vehicle:3:3 |
| --- | ---: | ---: |
| Valid | true | true |
| Completed | true | true |
| Completed objectives | 1 | 1 |
| Objective failures | 0 | 0 |
| Timeouts | 0 | 0 |
| Turnaround failures | 0 | 0 |
| Worst departure delay (s) | 532 | 0 |
| Aggregate departure delay (s) | 532 | 0 |
| Service wait (s) | 745 | 82 |
| Private events | 1188 | 525 |
| Simulated horizon from boundary (s) | 1302 | 770 |
| Termination | simulation_finished | simulation_finished |

`RecoveryOutcome.departure_delay_seconds` maps **mean** delay in this runtime,
not aggregate delay. With one aircraft, mean and aggregate coincide here. Aggregate
was independently obtained from the aircraft's actual departure-delay metric.
GSE travel seconds are not mapped by the existing evaluator; no value is invented.

Material criterion: `532 - 0 = 532 >= 300` seconds of worst departure-delay
improvement under `recovery_outcome_materially_improves`. The configured default
`minimum_delay_improvement_seconds == 300.0` is explicitly asserted. Neither the
ordering nor the threshold changed.

## Automatic generation, cap, and selection

The natural outage epoch produces this deterministic order:
```
current_plan
vehicle:3:3
vehicle:3:4
```
Both repair candidates are valid. Cart 4 privately reproduces Tier 0's outcome.
Cart 3 is the unique material improvement, and the planner automatically selects
`ReassignVehicle(task_id="3", target_id="3")`. No candidate or planner invocation
is injected into the authoritative Integrated run.

The raw test regenerates the real boundary's candidates twice with cap 2 and
requires `[current_plan, vehicle:3:3]`, including exact action equality. Existing
`StrandedAssignedTaskSurvivesCandidateCapAheadOfDiagnostics` and
`DirectGseRepairRemainsAheadOfHoldAtCandidateCap` regressions also pass, preserving
direct GSE repair priority ahead of diagnostic action families.

Outage epoch time: 130 s. Fingerprint: `11071817376211835091`. Live action
revalidation/application returns `accepted`; dispatch and ownership-transfer events
both identify task 3 and vehicle 3 at the selected epoch.

## Causality and realized outcomes

All times below are exact authoritative simulation seconds; full event sequences
are retained in `goal49d-focused-tests.txt`.

| Milestone | REACTIVE / SHADOW | Private prediction / INTEGRATED |
| --- | ---: | ---: |
| Source task dispatched to vehicle 2 | 121 | 121 |
| Vehicle 2 outage revealed | 130 | 130 |
| Planner selects replacement | no live action / shadow recommends at 130 | 130 |
| Authority transfers to replacement | cart 4, 130 | cart 3, 130 |
| Cart 3 travel start / first observed movement | unused | 130 / 130 |
| Source safely unavailable at refuge | preserved outage semantics | 147 |
| Critical baggage service starts | 831 | 168 |
| Critical task completes | 1071 | 408 |
| Baggage-load dependency becomes ready | 1071 | 408 |
| Baggage load completes | 1312 | 649 |
| Pushback preparation completes / turnaround ready | 1432 | 769 |
| Aircraft departs | 1432 | 900 |
| Completed turnarounds / failed | 1 / 0 | 1 / 0 |
| Worst / aggregate departure delay | 532 / 532 | 0 / 0 |
| Service wait | 745 | 82 |

The backup accepts normal fleet authority, moves from its depot, services the
critical task, and unlocks the prerequisite chain. All six tasks complete exactly
once, every prerequisite completes before its dependent starts, and task ownership
ends at the selected vehicle. The observed public fleet travel start is sampled at
the 130 s event boundary; service start/completion are authoritative task events.

The source follows existing refuge evacuation semantics. During evacuation its
public snapshot briefly reports Idle, while the fleet's unavailable-pending state
excludes it from dispatch. It subsequently reports Unavailable at the safe bay
(node 10). It has no later dispatch, service start, or task-completion event. The
proof preserves this behavior rather than changing snapshot or fleet semantics.

## Safety, healthy vehicles, and isolation

The raw boundary asserts both backups are compatible, idle and without an aircraft
assignment. Private action validation succeeds and fleet assignment validates the
route. In Integrated, cart 4 remains idle and unassigned throughout; FuelTruck-1
retains the healthy fueling task in both baseline and recovery. Only the critical
baggage task has a reassignment event. Every active mobile task has a distinct
vehicle owner at every Integrated event boundary. No healthy committed task is
stolen, no task completes twice, and dependency ordering remains valid.

Baseline, private prediction and live recovery all have zero fleet collisions,
zero fleet deadlocks, no unresolved deadlock, zero leaked active reservations,
zero unfinished/failed fleet requests, zero unresolved service requests, and zero
aircraft/aircraft or aircraft/ground collisions. All tasks and the turnaround
complete normally.

Private evaluation preserves the parent's snapshot, event history, and next event
time. Replay equivalence also passes. After both evaluations the parent is run to
completion against a fresh untouched control: complete event history, final
snapshot and every final metric are exactly equal. This continuation check detects
hidden queue, fleet reservation, authority, metric or workflow mutations beyond
the immediate snapshot. Accepted-action replay of the completed Integrated run
also succeeds.

## SHADOW, prediction, and determinism

SHADOW independently opens the same outage epoch and recommends `vehicle:3:3` with
the same candidates, private evaluations, fingerprint and predicted improvement.
Every SHADOW application-result field remains empty: its cart 4 reassignment is
ordinary reactive fleet behavior, not the recommended cart 3 action.

REACTIVE, SHADOW and repeated SHADOW run in lockstep. At **every event boundary**,
next event times, full snapshots (including task ownership, workflow and vehicle
movement), and event histories are exactly equal. Final metrics, including
reservation counters and safety, are also exactly equal. Recovery epochs are a
separate diagnostics stream; no authoritative event normalization is needed.

The selected private action is separately applied only to an isolated replay of
the shadow boundary, then executed normally. Its **entire authoritative event
history, final snapshot and all final metrics exactly match automatic Integrated**.
This includes task completion 408 s, turnaround readiness 769 s, departure 900 s,
all completion/failure fields, delays, service wait and safety. Direction and exact
realization match, beyond the evaluator's mapped summary fields.

Integrated is repeated with seed 42. Every next event time, snapshot and event
history agrees; final metrics agree. All recovery epochs agree in time, mode,
trigger, fingerprint, triage, suppression/planning fields, candidate order, full
private evaluation objects (including event counts/horizons/termination), selected
action and application result. SHADOW's full epoch objects are likewise checked
against its repeat.

## Focused validation and publication

The accompanying raw output contains the exact gtest filter and **54 passing tests
from 7 suites**. It covers the two GSE proofs, ReassignVehicle/fleet adapter
validation, outage and accepted-action replay, parent isolation, Shadow isolation,
ReassignStand, ResequenceTasks, material ControlledHold, non-beneficial recovery,
candidate caps, mode parsing and deterministic selection. The 54-test filter is
focused recovery validation, not full CTest or the eight-family matrix.

Source-first proof commit: `3061a90d10be446889fc41f0264f9abfad4295ba`
(`Add material Goal49 automatic GSE recovery proof`). It was immediately pushed,
fetched and verified local == tracking == live with a clean tree before extending
parity/determinism coverage.

Final validation commit message: `Validate Goal49 automatic GSE recovery parity`.
Its full SHA and final live-ref equality are reported in the completion response,
after commit/push/fetch verification (a commit cannot embed its own hash).
Only the focused test source and these two evidence files change from the required
starting tip. No runtime architecture, action adapter, dispatcher, comparison
implementation, production scenario or readiness gate changes.

## Goal49 boundary

ReassignVehicle material-positive proof, ReassignStand, ResequenceTasks and
ControlledHold are complete within their focused scopes. Remaining Goal49 work:
focused certification, eight-family matrix, 120 serial, 120 parallel, matrix parity,
final safety certification, prediction analysis, final evidence ZIP and final
Goal49 declaration/consolidation/merge. None of that matrix work was started here.
KAUO remains DATA_BLOCKED; all operations in this proof are synthetic. Goal49
remains unmerged and Goal50 has not started.
