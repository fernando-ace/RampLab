# Goal49 corrective toolchain and validation checkpoint

## Remote state at task start

The initial live gate output is preserved in `git-preflight.txt`. The required
Goal49 branch tip was `ef8a7e235e98639fea0552fdb7b041e1275eb7a5`, based on
`origin/main` at `14d63061ef6884733209de5311e2362d7972bbc2`. A staged patch on
local `work` was preserved as a stash and not applied to the Goal49 branch.

## Toolchain

```text
cmake version 3.31.10
1.13.0.git.kitware.jobserver-pipe-1
g++ (Debian 14.2.0-19) 14.2.0
Python 3.12.14
```

Configured PATH:

```text
/workspace/ramplab-venv/bin:$PATH
```

## Fresh Release validation (2026-10-18)

Fresh configure:

```text
cmake -S . -B build-goal49-fresh -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
```

Result: passed. The complete Release build completed successfully.

The first full CTest run had one stale assertion in
`SimulationTest.RecoveryEpochTracksOnlyProcessedSurfaceClosureInShadowMode`.
The runtime correctly performed private planning and retained Tier 0; the test
expected the old `private_replay_evaluator_unavailable` skip. The assertion
was updated to match the existing planner behavior. The focused test passed,
and the subsequent full CTest run passed **248/248**.

Python discovery:

```text
python3 -m unittest discover -s tools -p 'test_*.py'
```

Result: **114 tests, 113 passed, 1 failed**. The failure is
`airport_scenario_generation.test_goal42_geometry.Goal42GeometryTests.test_goal42_geometry_is_reproducible_and_loadable_by_existing_generator`,
which compares generated Goal42 geometry against its checked-in output. The
failing test and generator are byte-for-byte unchanged from `origin/main`;
classify this as a historical Goal42 reproducibility failure, not a Goal49
change. It remains unresolved and is not hidden or rewritten.

This validation checkpoint does not establish Goal49 completion. The material
automatic GSE case, additional action implementations, focused fixture gate,
experiment matrix, safety audit, and final evidence package remain open.
