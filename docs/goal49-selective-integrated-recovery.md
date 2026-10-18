# Goal49 selective integrated recovery

**Status: INCOMPLETE.** This report records published recovery work and the
current evidence. Goal49 is not a completed or approved Goal50 base.

## Corrective continuation checkpoint (2026-10-18)

The required initial live gate for this continuation reported
`14d63061ef6884733209de5311e2362d7972bbc2` for `main` and
`ef8a7e235e98639fea0552fdb7b041e1275eb7a5` for the Goal49 branch. The branch
was checked out at that exact tip with merge-base `14d63061ef6884733209de5311e2362d7972bbc2`.
Literal output and workspace preservation details are in
`docs/goal49-evidence/git-preflight.txt`.

A fresh Release configure and complete build succeeded. After correcting a
stale surface-closure SHADOW assertion, full CTest passed **248/248**. Python
discovery passed **113/114**; the sole failure is the unchanged Goal42 geometry
reproducibility test also present on `origin/main`. Exact commands and
classification are in `docs/goal49-evidence/toolchain-and-preflight.md`.

No Goal49 completion gate was closed by this validation-only checkpoint. In
particular, the current branch still has no genuinely material automatic GSE
recovery scenario, and `ReassignStand`, `ResequenceTasks`, and `ControlledHold`
are candidate kinds without runtime execution support. The eight-family
240-run matrix, parity, safety audit, and final evidence package remain open.

## Repository and salvage status

The task began in a clean checkout on local branch `work` at `main` SHA
`14d63061ef6884733209de5311e2362d7972bbc2`. The failed-run `+1,005/-1`
uncommitted changes were absent. The live Goal49 branch was fetched and
checked out at its published tip `3b60b614738e03d2df44e84fa647123369b25442`.
Only changes needed for the recovery work described below were recreated.

Initial live remote output:

```text
14d63061ef6884733209de5311e2362d7972bbc2	refs/heads/main
3b60b614738e03d2df44e84fa647123369b25442	refs/heads/goal49-selective-integrated-recovery-rebuild-14d6306
```

## Published implementation checkpoints

- `180d3055390c743d7b262f09b7b5590fc41af525` — outage-stranded dispatcher
  reassignment, candidate eligibility, deterministic executable-candidate
  priority, and focused tests.
- `16c22bd385de31d69f0e6f4037d402e3d6089a18` — retain recovery candidates and
  private evaluation records on each recovery epoch.
- `af58020def93e3b3e126cd921c4a01a861191bdc` — runtime intervention provenance,
  replay of surface closures and vehicle outages, and replay of accepted
  recovery actions using event-prefix and queue-sequence boundaries.
- `a090b38d39b6727aed8179647790b13af647ee41` — multi-epoch replay test.
- `6a0e04c29c82584680fe2e8364fd2a53e8067aec` — REACTIVE/SHADOW parity test.

## Current recovery behavior and focused evidence

The dispatcher accepts explicit reassignment for a queued request and for a
still-assigned request whose current owner is unavailable. It rejects
reassignment from a healthy owner, unavailable replacements, incompatible
replacements, and missing requests. On an outage handoff it removes old
request ownership, increments reassignment accounting, and emits deterministic
unassignment/reassignment events. The existing turnaround integration test
also verifies that a replacement vehicle travels, performs service, and the
request completes through its normal lifecycle.

Candidate generation recognizes incomplete `Assigned` or `InProgress` tasks
when a matching vehicle outage has been revealed. Such vehicle reassignment
proposals sort ahead of unsupported stand, resequence, and hold proposals.
Unsupported action kinds do not consume candidate evaluation slots. A focused
test with a two-candidate cap confirms the stranded GSE action survives ahead
of many stand alternatives and repeats in the same order.

Each recovery epoch retains its candidate list and all private evaluations.
The automatic synthetic outage diagnostic currently shows:

| Evaluation | Completed turnarounds | Failures | Timeouts | Worst delay |
|---|---:|---:|---:|---:|
| Tier 0 current plan | 3 | 0 | 0 | 109 s |
| GSE candidate `vehicle:3:3` | 3 | 0 | 0 | 109 s |

The GSE candidate is generated and privately accepted, but ties the current
plan. The planner correctly selects no action. This fixture does not prove
material automatic recovery because reactive dispatch already completes all
objectives. The 300-second delay materiality threshold was not changed.

Runtime surface closure and vehicle-outage replay tests pass at their recorded
event boundary. Accepted recovery action replay passes, and a second recovery
epoch successfully reconstructs the first accepted action. A same-seed
REACTIVE/SHADOW run produces equal metrics, event history, and final snapshot;
SHADOW records planning evaluations and applies no action.

The Release `airside_tests` target builds with the configured project-local
toolchain. Focused recovery, dispatcher, turnaround replay, and parity tests
passed. Full CTest, Python discovery, Release validation from a fresh build,
and Goal49 matrix validation have not been run.

## KAUO boundary

KAUO remains `DATA_BLOCKED`. No production KAUO simulation was run for this
work. Recovery tests use synthetic scenarios only; no KAUO geometry, routes,
stands, permissions, or clearances were added or inferred. Readiness was not
changed.

## Remaining completion gates

Goal49 remains incomplete until the automatic GSE fixture demonstrates a
material benefit and authoritative execution, and the remaining requested
action implementations and positive/negative fixtures pass. The eight-family
120-serial/120-parallel matrix, REACTIVE/SHADOW matrix parity, serial/parallel
parity, prediction-versus-realized analysis, planner work metrics, full safety
audit, fresh full validation, final evidence bundle, ZIP/hash verification,
and completion report are also outstanding.

## Implementation checkpoint preceding this report

Before authoring this report, local `HEAD`, the fetched Goal49 remote-tracking
ref, and live `ls-remote` all equaled:

```text
6a0e04c29c82584680fe2e8364fd2a53e8067aec	refs/heads/goal49-selective-integrated-recovery-rebuild-14d6306
```
