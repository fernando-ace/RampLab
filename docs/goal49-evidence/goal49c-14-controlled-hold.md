# Goal49C-14 — automatic ControlledHold on the frozen safe material fixture

## Initial remote reality

```
14d63061ef6884733209de5311e2362d7972bbc2	refs/heads/main
b86377d3837e1d95cfcff76af7169950d05e2326	refs/heads/goal49-selective-integrated-recovery-rebuild-14d6306
```

The initial `git fetch --prune origin` succeeded. The workspace fetch refspec
tracked main only, so the Goal49 branch was explicitly fetched into its remote
tracking ref before the required `git switch -C`. Starting HEAD was the required
b86377d3837e1d95cfcff76af7169950d05e2326, with a clean working tree.

Used `/workspace/ramplab-venv/bin` first in PATH. CMake/CTest 3.31.10,
Ninja 1.13.0.git.kitware.jobserver-pipe-1, g++ (Debian 14.2.0-19) 14.2.0.
Created absent `build-goal49c` as Release/Ninja, reusing existing dependency
sources. No full CTest run during development.

## Preserved fixture and recovery plumbing

The original material scenario setup is extracted into a shared test helper
without changing its contents. Topology, routes, physical stand (60,-80),
South Junction attachment (100,-80), pushback motion, 0.1 m/s taxi speed,
readiness/schedule facts, and all safety thresholds remain unchanged.

The same shared departure edge has a one-second `surface_closure` at 7089,
reopening at 7090. This opens a legitimate recovery epoch while AX101 is parked,
before its unchanged 7090 readiness/pushback request. The trigger itself is
checked to preserve the original A-first result. RecoveryState contains only
its current snapshot and the already revealed closure, with no future edge
arrival, release, benefit, or outcome fields.

Candidate generation now includes the parked phase already supported by the
runtime action adapter. Fixed alternatives 60, 180, 300, and 600 seconds are
privately evaluated; no future traffic timing or material metric chooses a
candidate at generation time. Existing direct GSE, stand, and relevant
resequence priorities are retained. The existing comparison and 300-second
threshold are untouched. The bounded private horizon is two hours so this
fixture's actual terminal outcome can be evaluated (its final time is 13170).
The 10000-event budget is retained.

The existing production private evaluator is extracted into a read-only method
for focused inspection, and the normal planner calls that same method. Terminal
validity also rejects existing safety-threshold violations, collisions,
deadlocks, outstanding reservations, and active holds. No runtime hold or
reservation behavior changes.

## Requirement conflict requiring user resolution

The mission's strict event order requires AX202 to clear the shared edge before
the ControlledHold release. On the frozen fixture AX202 enters at 7241 and
clears at 8841: traversal alone is 1600 seconds. A hold capped at 600 seconds
cannot span that traversal. This conflicts with the frozen speed/route and
unchanged hold-duration constraints regardless of recovery-epoch placement.

The proposed operational proof instead measures hold start, AX202 entry,
hold release, AX202 clearance, then AX101 pushback and later edge entry. The
existing reservation gate keeps AX101 at its safe stand after hold release.
The user was asked whether that event ordering is acceptable. Until answered,
this evidence must not be described as satisfying every Goal49C-14 gate.

Goal49 remains unmerged; KAUO remains DATA_BLOCKED; Goal50 is not started.

## Measured operational result

| Measure | Reactive / Shadow authoritative / private Tier 0 | Integrated / private selected hold |
|---|---:|---:|
| AX101 shared-edge taxi entry | 7120 | 8871 |
| AX202 shared-edge taxi entry | 8720 | 7241 |
| AX202 shared-edge release | 10320 | 8841 |
| AX101 pushback begins | 7090 | 8841 |
| Hold start / release | none | 7089 / 7269 |
| Hold duration | none | 180 |
| Worst departure delay | 10090 | 8611 |
| Aggregate departure delay | 13258 | 11779 |
| Completed turnarounds | 3 | 3 |
| Objective failures / timeouts / turnaround failures | 0 / 0 / 0 | 0 / 0 / 0 |
| Aircraft minimum separation (m) | 12.6 | 40 |
| Aircraft / ground / fleet collisions | 0 / 0 / 0 | 0 / 0 / 0 |
| Surface acquisitions / releases | 7 / 7 | 7 / 7 |
| Deadlocks / outstanding reservations / active terminal holds | 0 / 0 / 0 | 0 / 0 / 0 |
| Final time | 13170 | 13170 |

All route assignments remain identical to the frozen routing API results.
The current-plan candidate is first, followed by `hold:1` (60 s),
`hold:1:180`, `hold:1:300`, and `hold:1:600`, within the existing cap of 8.
Each action uses AX101's numeric aircraft ID `1` in task_id and its duration
in target_id. The 60-second candidate is executable but ends unsafe and worse
(worst delay 10149), so is rejected by private validity. The 180/300/600-second
candidates are safe and tie at the better outcome. Existing strict outcome
ranking keeps the first of those candidates, `hold:1:180`. There is no hard-coded
action preference or known outcome in production code.

The existing materiality comparison reports a 1479-second worst-delay
improvement; aggregate improvement is also 1479 seconds. All terminal
predictions use actual private simulation, with no categorical timeout shortcut.
Tier 0 uses 3575 private events and the selected hold 3557; both simulate
6081 seconds and terminate `simulation_finished`. Service wait is 905 seconds;
GSE travel is unavailable in the existing RecoveryOutcome mapping.

Integrated live revalidation succeeds (`application_result=accepted`). The
selected private delays and ordering match realized Integrated exactly.
Shadow selects the same candidate and records the same predicted material
improvement, while producing no authoritative hold start/release events.
Reactive and Shadow snapshots are compared after every event, and final event
history, formatted event log, metrics, and snapshots must match exactly without
normalization. Planning diagnostics live separately in recovery epoch records.

Private evaluation leaves parent snapshots, event history, holds and next queued
event unchanged. Parent continuation is compared with an untouched replay for
full event history, metrics and terminal snapshot. The accepted Integrated hold
is also reconstructed by the existing replay architecture at the terminal
boundary. No second recovery epoch is forced.

Seed-42 Integrated and Shadow repeats compare full authoritative event history,
metrics and final snapshot. Integrated also compares recovery fingerprint,
full candidate ordering, selected candidate, and every private evaluation field.
The measured fingerprint is 850552943722534814.

A second trigger timing on the same frozen fixture (closure 7000, reopen 7001)
is used only for a no-benefit regression: with the candidate cap set to 2, the
original 60-second alternative expires before readiness, is valid, and ties
Tier 0. The planner retains the current plan and the parent applies no hold.
This does not suppress candidates in the main end-to-end proof or production.
