# Goal 32: Parallel Experiments and Operational Optimization

## Scope and architecture

Goal 32 adds a sweep, scheduler, aggregation, and ranking layer around the existing Goal 30 Scenario Studio pipeline. Each configuration goes through the Goal 24B canonical package generator, mapped scenario generator and validator, the native `airside_cli`, and Goal 19 result analysis. The experiment engine does not make airport-specific dispatch or routing decisions.

The experiment store is local to the Scenario Studio home. Each experiment has a machine-readable `experiment.json`, one isolated directory per run, and an export ZIP containing the definition, generated scenarios, Markdown report, and run provenance. The static dashboard assets add an Experiments view without a new frontend dependency.

## Parameter registry and configuration generation

The registry exposes fields the selected Scenario Studio scenario and Goal 24B generator actually consume. Deterministic simulation seeds are selected in the experiment's separate seed list, not as a scenario parameter axis:

- ISO 8601 flight schedule times, registered from scenario flight records.
- Numeric turnaround service durations and categorical equipment availability, when present in the scenario.
- Route closure start and duration only when a route is explicitly present in the dataset mapping.

Each registry entry describes its adapter, type, units, allowed values or numeric limits, and source field. New scenario fields can use the generic path adapter when the generator consumes them. Unknown, repeated, malformed, timezone-free, or out-of-domain axes fail validation before runs are created. Numeric `start..stop/step` ranges are inclusive and deterministic.

Configuration generation takes the Cartesian product in declared axis order. Configuration IDs hash the unchanged base scenario and typed parameter values; run IDs hash configuration ID and seed. An optional unchanged baseline is a separate configuration. Every configuration is replicated once per unique integer seed. The default matrix limit is 100 runs; larger experiments require the explicit `allow_large` override. Worker counts are bounded at eight.

## Scheduler, identities, cache, and resume

The manager builds the native CLI once before it schedules workers. A bounded `ThreadPoolExecutor` runs each simulation in its own output directory. Workers do not mutate shared scenario inputs. Pause stops new launches, stop cancels queued launches, and a failed run is recorded without discarding completed work.

Run identity includes the canonical dataset scenario, seed, Goal 24B schema, Git revision, mapped dataset and geometry hashes, native CLI binary hash, and hashes of the scenario-generation, release-adapter, analysis, and experiment pipeline sources. The identity is independent of worker scheduling. Completion order is allowed to vary.

Cache reuse is opt-in in the builder. An entry is accepted only when the run identity matches, validation was successful, and hashes match for `experiment.json`, `runs.csv`, `simulator-metrics.json`, and `events.jsonl`. A small real replay verified that one fresh native run was followed by a `CACHED` run with the same identity and result digest.

On resume, completed and cached runs stay completed. Failed, invalid, cancelled, and orphaned running work is retried. If an interrupted run has a partial output directory, it is moved aside with an `interrupted` suffix before the new attempt starts. The partial files are preserved for inspection.

## Metrics, constraints, ranking, and sensitivity

Goal 19 metrics are aggregated by configuration across available seeds with count, mean, median, minimum, maximum, and population standard deviation. Standard deviation is unavailable for a single seed. Missing values stay missing; they are never changed to zero.

Hard constraints evaluate every seed. A known violation yields `INFEASIBLE`; absent constraint metrics yield `INSUFFICIENT DATA`; unfinished or failed seeds yield `INCOMPLETE`. Ranking excludes those states, sorts by the selected objective directions, and breaks ties by configuration ID. Pareto filtering preserves multi-objective tradeoffs without assigning weights. Sensitivity groups report observed parameter associations only and do not claim causality or statistical significance.

## Scenario Studio workflow and reports

The Experiments area supports selecting registered parameters and values, adding deterministic seeds, previewing configuration and run counts, selecting workers and cache reuse, setting objectives and safety constraints, creating and watching an experiment, filtering aggregate versus per-run results, inspecting configuration details, and comparing the selected candidate against the baseline. A generated Markdown report includes the parameter space, outcomes, constraints, ranked configurations, baseline deltas, sensitivity summaries, Pareto candidates, and provenance.

## Real KAUO experiment and parity benchmark

The demonstration uses the `kauo-public-calibration-prototype-2026-10-09` Scenario Studio dataset. KAUO here is an illustrative calibrated simulation prototype; its representative flight, parking, timing, taxiway, and geometry assumptions are not observed airport operations or a validated digital twin.

The sweep varied four mapped closure start times (14:00, 14:15, 14:30, 14:45 UTC), three closure durations (15, 30, 45 minutes), two supported departure schedule times (15:00, 15:15 UTC), and the unchanged baseline. It produced 25 configurations and 75 runs for seeds 42, 43, and 44. Hard constraints required zero aircraft-aircraft collisions, zero aircraft-ground collisions, zero safe failures, and one completed departure.

| Measure | Serial | Four workers |
|---|---:|---:|
| Configurations | 25 | 25 |
| Native simulation runs | 75 | 75 |
| Completed / failed / invalid / cached | 75 / 0 / 0 / 0 | 75 / 0 / 0 / 0 |
| Wall time | 13.514 s | 6.054 s |

Measured speedup was 2.232× on this Windows machine. All 75 run identities and all 75 normalized result digests matched across serial and parallel execution. The highest-ranked feasible configuration was the unchanged baseline, with 1.50 minutes mean departure delay; the best tested variants tied that delay, so no delay improvement was demonstrated. Taxi time varied between 364 and 772 seconds across tested configurations. These are results inside this finite prototype parameter space.

The benchmark output is [goal32-demo-benchmark.json](goal32-demo-benchmark.json). It records experiment IDs, run identities, result digests, success counts, timings, selected configuration, baseline delta, and software provenance. To repeat both sweeps from this checkout, run:

```powershell
$env:RAMPLAB_SCENARIO_STUDIO_HOME = "$env:LOCALAPPDATA\RampLab\Goal32"
python -m tools.ops_dashboard.goal32_benchmark
```

To run one real sweep for progress capture, use `python -m tools.ops_dashboard.goal32_benchmark --workers 4` (or `--workers 1`). The builder default enables validated cache reuse; benchmark mode disables it to ensure the timing includes fresh native runs.

## Failure recovery and validation

Scheduler unit tests inject a controlled fake failure, verify other runs continue, and verify resume retains completed work. A restart recovery test confirms partial outputs are preserved while an interrupted run is retried. Cache validation tests check required result artifacts and identity changes; the cache replay described above used the real simulator.

Validation from the clean Goal 30 baseline:

- Full CTest suite: 186/186 passed.
- Python Goal 24A ingestion, Goal 24B generation and run-artifact, Goal 19 analysis, Goal 20 dashboard, Goal 22 evidence-bundle, Goal 28 live operational control, Goal 30 Scenario Studio, and Goal 32 experiment suites passed (83 tests across the combined dashboard/generator/ingestion/evidence suites, plus 10 Goal 19 analysis tests).
- Goal 28 live-control/replay tests: 4/4 passed against the CLI built from this checkout.
- Python compilation, JavaScript syntax check, and `git diff --check` passed.
- Scenario Studio UI created and completed a real 25-configuration, 75-run KAUO experiment. Screenshots below come from the clean local UI session and real run data.

## Goal 31 Compatibility

Goal 32 was intentionally developed from the clean Goal 30 baseline while Goal 31 was in progress. No Goal 31 implementation was copied into this branch. When Goal 31 is ready to integrate, its generator-backed GSE quantities and turnaround/service durations can be registered with IDs, units, typed validation, and adapters. The Cartesian expansion, seed replication, scheduler, cache, aggregation, ranking, Pareto, reporting, and UI components can remain unchanged. Unsupported or unavailable fields should remain absent from the registry rather than be guessed.

## Screenshots

All six screenshots are from the Scenario Studio and the real local KAUO experiment:

1. [Experiment Builder](screenshots/goal32/01-experiment-builder.png)
2. [Parameter sweep configuration](screenshots/goal32/02-parameter-sweep.png)
3. [Live experiment progress](screenshots/goal32/03-running-progress.png)
4. [Completed results table](screenshots/goal32/04-completed-results.png)
5. [Candidate and baseline comparison](screenshots/goal32/05-candidate-comparison.png)
6. [Observed response and Pareto view](screenshots/goal32/06-sensitivity-pareto.png)

## Known limitations

- The default registry is limited to scenario fields currently consumed by Goal 24B; new domain concepts need a truthful generator-backed adapter before they become selectable.
- The demonstration has one representative aircraft and a small illustrative KAUO dataset. It does not establish operational airport performance or broad optimization quality.
- The tested objective was departure delay. Its best value tied the baseline; the experiment did not find a delay improvement.
- The performance figure is one measured run on one Windows host and is not a general speedup guarantee.
- The report provides descriptive aggregation and parameter grouping, not statistical significance tests or causal analysis.
- Pareto and sensitivity views reflect only the configured objectives, metrics, and tested values.
