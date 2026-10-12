# Goal 35 evidence package

This folder contains the reproducible paired comparison for Goal 35. Every source scenario in this package is a simulator fixture. It supports claims about the measured runs only, not real airport operations.

## Contents

- `config/experiment_config.json` records source and policy-configured scenario hashes, policies, seeds, and the one configuration difference between paired runs.
- `config/scenarios/` contains the exact `SCRIPTED` and `ADAPTIVE` scenario YAML copies used by the runner.
- `runs/<scenario>/seed-<seed>/<policy>/metrics.json` contains native simulator metrics, including departure delay, taxi time and distance, surface wait, queueing, objective counts, separation, collisions, and safe failures.
- `runs/<scenario>/seed-<seed>/<policy>/events.jsonl` contains the simulator event stream.
- `runs/<scenario>/seed-<seed>/<policy>/decisions.json` contains full structured observations, candidate routes and scores, the selected candidate, action, reason, and explanation.
- `results.csv` is the compact policy/scenario comparison table.
- `experiment.json` is the result index and records per-run SHA-256 digests plus serial/parallel replay equality checks.
- `SHA256SUMS.txt` lists hashes for the committed evidence files other than itself.

## Reproduction

From the repository root:

```powershell
python tools/goal35/run_matrix.py --cli build-goal35-msvc/Release/airside_cli.exe --workers 4
```

The runner executes 36 serial runs and 36 replays with four workers. A replay passes only when its normalized metrics and the bytes of its metrics JSON, native event stream, and decision trace match the serial run for the same scenario, seed, and policy. The runner exits with an error if any comparison differs.

## Demonstration trace

In `adaptive_route_choice_demo`, seed 42, aircraft 3's decision is `agent-decision-2` at second 548. ADAPTIVE selects the 280 metre direct route with a score of 285.6 seconds over the 200 metre shared route with a forecast 211 second wait and a score of 415.0 seconds. The paired SCRIPTED trace selects the shared path by shortest planner cost. Aircraft 3 completes its route objective 51 seconds earlier in that paired ADAPTIVE run, while aggregate delay, taxi, distance, and surface-wait metrics are worse for ADAPTIVE across the three-seed fixture comparison. The decision and its consequence can be inspected in the corresponding run JSON files.

The route-choice demo, other scenario files, and schedule data are synthetic. No conclusions about observed or expected airport efficiency follow from these artifacts.
