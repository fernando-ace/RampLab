# Goal 36 evidence package

This bundle records a synthetic comparison of the three surface aircraft routing modes. Its source scenarios are identified with SHA-256 in [experiment.json](experiment.json), configured copies vary only the routing policy, and the same seeds run serially and in parallel.

## Contents

- `experiment.json` — policy/scenario definitions, seeds, per-run artifact digests, per-agent objective completion timestamps, aggregate KPI tables, and serial/parallel checks.
- `results.csv` — compact per-scenario and per-policy KPI summary.
- `config/experiment_config.json` — matrix dimensions and source/configuration hashes.
- `config/scenarios/` — exact scenario copies used for each policy.
- `runs/<scenario>/seed-<seed>/<policy>/` — `metrics.json`, `events.jsonl`, and structured `decisions.json` for every run. Coordinated decisions include shared traffic, candidate/system costs, conflict details, arbitration rule, and active safety constraints.
- `SHA256SUMS.txt` — hash ledger for every other file in this evidence directory.

The six scenario labels are roles assigned to existing RampLab synthetic fixtures. `fairness_contention` reuses the shared bottleneck scenario; it is not a separate long-duration starvation benchmark. See [the Goal 36 report](../goal36-coordinated-multi-agent-operations.md) for measured results and limitations.

Reproduce from repository root:

```powershell
python tools/goal36/run_matrix.py --cli build-goal36-vs18/Release/airside_cli.exe --workers 4
```
