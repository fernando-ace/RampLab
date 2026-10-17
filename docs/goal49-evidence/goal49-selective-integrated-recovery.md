# Goal 49 — Selective Integrated Airport Disruption Recovery

## Status: INCOMPLETE

This branch contains the first reusable planning core, but Goal49 is not complete. The core is deliberately not reported as productionized: it is not connected to the `Simulation` event loop, has no `REACTIVE`/`RECOVERY_SHADOW`/`INTEGRATED_RECOVERY` run modes, does not execute recovery actions, and has no private simulator fork for counterfactual candidate evaluation. The required 120-run matrix, targeted fleet/hold fixtures, prediction-versus-realized validation, safety audit, and operator recommendation workflow have therefore not been run. There is no experimental evidence that any new recovery action improves operational outcomes.

## Git base

- Starting branch: `work` (clean checkout)
- Current `origin/main` at fetch: `14d63061ef6884733209de5311e2362d7972bbc2`
- Goal49 branch: `goal49-selective-integrated-recovery`
- Goal49 base: `14d63061ef6884733209de5311e2362d7972bbc2`
- Lost Goal48 commits were not used.

## Implemented planner core

`include/airside/operations/recovery.hpp` and `src/operations/recovery.cpp` add:

- An owned `RecoveryState` that contains a copied `SimulationSnapshot`, revealed disruptions, wait-age signals, and current plan identifier. It holds no mutable engine references.
- Recovery modes and triage result types, plus deterministic fingerprinting of operational state. Diagnostic-only aircraft taxi distance is excluded. The fingerprint contract is limited to fields explicitly hashed by this planner; it is not a claim of full simulator-state equivalence.
- Typed action records for GSE reassignment, stand reassignment, task resequencing, and bounded controlled holds.
- Snapshot-based eligibility checks for service compatibility, idle vehicle state, task status/dependencies, scheduled aircraft before stand reassignment, open/unoccupied gate, and bounded hold duration.
- Stable bounded candidate enumeration with a current-plan baseline, deterministic action tie order, and depth/tier/candidate limits.
- An evaluator callback boundary for an isolated counterfactual evaluator, a documented outcome ordering, and a materiality gate. The callback is caller-supplied; this branch does not yet provide the simulator clone or isolation needed for production evaluation.

The planner core currently has no simulator action-application adapter. In particular, the checks cannot establish route feasibility, vehicle reservation safety, service access, downstream departure feasibility, or live task commitments. They must not be treated as authoritative action validation.

## Verification

- Release configure and build: passed (`build-goal49`, GNU C++ 14.2, Release).
- Focused recovery tests: 6 passed.
- Full CTest: 235/235 passed after correcting three initial test failures.
- Python KAUO/airport evidence tests: 10 passed using `unittest` (`test_goal47`, Goal44 topology audit, Goal42 replay).
- `pytest` was unavailable in both configured Python interpreters, so no pytest command ran.
- No dashboard/operator tests were run; no UI/operator code changed.
- The build emitted warnings in pre-existing files and one aggregate-initializer warning in `recovery.cpp`; no build errors.

These checks validate the current planner primitives and existing regression suite only. They do not validate integrated recovery behavior.

## KAUO boundary

KAUO remains `DATA_BLOCKED`. This work does not alter readiness logic, promote Goal47 taxiway evidence, synthesize stands or routes, or run production KAUO simulation. Goal47/Goal42 evidence files were not modified in this branch.

## Required remaining work before Goal49 can be called complete

1. Connect triage and planning epochs to revealed disruption events and suppress duplicate fingerprints.
2. Supply isolated deterministic private evaluation using simulator semantics, and apply selected typed actions through authoritative validated APIs.
3. Implement actual shadow and integrated modes and prove shadow output parity.
4. Validate all supported disruption classes and all four action types, including beneficial/harmful GSE and hold decisions, fairness, stability, compound recovery, and safe impossible recovery.
5. Add the synthetic scenario family, execute the three-mode 120-run serial/parallel matrix and targeted fixtures, and record outcome/work/safety/prediction evidence.
6. Complete relevant Python, operator, scenario, and evidence-bundle test suites; classify any environment or unrelated failures.
7. Produce the complete evidence bundle and integrity-verified ZIP after experiments.
8. Finish implementation and experiments, then make a final completion commit and verify it remotely. The current incomplete planner-core commit is pushed and its remote tip matches; this does not satisfy Goal49 completion publication.

No Goal50 recommendation is made because the required experiments have not been completed.
