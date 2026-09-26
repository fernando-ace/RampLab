# Headless experiments

## Purpose

`airside_experiment` runs reproducible local parameter sweeps through the same authoritative `Simulation` class used by `airside_cli` and the Unreal viewer. It is intended for capacity and disruption what-if studies, not for distributed execution or optimization.

```text
Experiment YAML -> loader/validation -> ExperimentDefinition
                                             |
Scenario YAML -> scenario loader -> immutable base Scenario
                                             |
                              stable Cartesian cases and seeds
                                             |
                                 bounded worker pool
                                             |
                         independent Simulation per requested run
                                             |
                         RunResult -> statistics -> CSV/JSON
```

The reusable `airside_experiment` library contains the definition, typed overrides, case generator, executor, result model, statistics, and writers. It has no yaml-cpp or Unreal dependency. `airside_experiment_yaml` is the only experiment layer that reads YAML. The executable is a thin CLI adapter.

## Build and run

Build with the normal standalone workflow:

```powershell
cmake -S . -B build
cmake --build build --config Release
.\build\Release\airside_experiment.exe --experiment experiments\capacity_sweep.yaml
```

Useful options:

```powershell
# Validate and report the expanded workload without running it.
.\build\Release\airside_experiment.exe `
  --experiment experiments\small_validation.yaml --dry-run

# Override file settings for one invocation.
.\build\Release\airside_experiment.exe `
  --experiment experiments\capacity_sweep.yaml `
  --workers 4 --output results\local-capacity --quiet

# Sort the final factual comparison table.
.\build\Release\airside_experiment.exe `
  --experiment experiments\capacity_sweep.yaml `
  --sort p95-turnaround
```

`--workers` and `--output` take precedence over the experiment file. `--quiet` suppresses the definition, progress, comparison, and completion text; it does not suppress output files. Progress is rate-limited rather than printed once per run.

## YAML schema

This is the checked-in small validation experiment:

```yaml
name: small_validation
scenario: ../scenarios/baseline.yaml

seeds:
  start: 40
  count: 3

parameters:
  fleet.fuel_trucks: [1, 2]

workers: auto

outputs:
  directory: ../results/small_validation
```

Scenario and output paths are resolved relative to the experiment file. Parameter-map insertion order defines Cartesian-product order. The rightmost axis varies fastest. Cases receive stable one-based IDs such as `case_0001`; runs are ordered by case, seed order, and replication number.

Unknown fields in `parameters` are rejected. Supported typed overrides are:

| Parameter | Type | Meaning |
|---|---:|---|
| `fleet.fuel_trucks` | positive integer | Fuel-truck fleet size. |
| `fleet.baggage_carts` | positive integer | Baggage-cart fleet size. |
| `service_durations_seconds.fueling` | positive integer | Fuel service duration. |
| `service_durations_seconds.baggage` | positive integer | Baggage service duration. |
| `vehicle_speed_mps.fueling` | positive number | Fuel-vehicle speed metadata. Edge traversal time remains authoritative. |
| `vehicle_speed_mps.baggage` | positive number | Baggage-vehicle speed metadata. |
| `aircraft_schedule.arrival_offset_seconds` | integer | Offset applied to every scheduled arrival. |
| `aircraft_schedule.departure_offset_seconds` | integer | Offset applied to every scheduled departure. |
| `disruptions.road_closure.enabled` | boolean | Retain or remove the base scenario's closure event. |
| `disruptions.road_closure.time_seconds` | nonnegative integer | Move the base closure event to this time. |

Fleet overrides rebuild fresh vehicle domain values with stable numeric IDs. Schedule overrides rebuild fresh aircraft and service-task values. Every case starts from a copy of the immutable loaded base scenario; workers never mutate a shared scenario.

The current closure overrides intentionally operate on an existing closure in the base scenario. They do not invent an edge selection. A closure-time override is invalid when the base scenario has no closure.

## Seeds and replications

Seeds are always explicit. Accepted forms are:

```yaml
seeds: 42
```

```yaml
seeds: [1, 5, 42]
```

```yaml
seeds:
  start: 1
  count: 100       # expands to 1 through 100
  replications: 1
```

```yaml
seeds:
  values: [7, 11]
  replications: 3
```

Duplicate seed values, empty seed sets, zero replications, overflowed ranges, duplicate parameter axes, and duplicate values within an axis are rejected. A replication deliberately repeats the same seed and configuration; `replication` remains part of run identity.

## Worker model and determinism

The executor creates a bounded, fixed-size pool of `std::jthread` workers. Workers claim indices from one atomic work cursor, copy and override the base scenario, construct an independent `Simulation`, and write to that request's unique result slot. No engine-wide lock is used. The caller waits on a condition variable and reports progress periodically.

`workers: auto` uses `std::thread::hardware_concurrency()` and reserves one reported logical processor when more than two are available. It uses one worker on machines reporting one or two processors. The pool is also capped at the number of requested runs. This is a logical-concurrency policy; RampLab does not infer physical core topology.

Completion order may vary, but output does not: results are sorted by the stable run ordinal before aggregation or serialization. Thread IDs and scheduling order are never written. Execution durations and the metadata completion timestamp are observational values and can differ; all simulation values are independent of worker scheduling.

The simulation engine's random generator belongs to each `Simulation` instance and is seeded by the run request. There is no global random generator, metrics store, scenario mutation, or singleton in the standalone core. The automated suite repeatedly compares deterministic projections from one-worker and multi-worker execution.

## Run and summary results

`RunResult` retains final metrics only:

- case ID, parameter values, seed, replication, and scenario name;
- simulated and wall-clock execution durations;
- average turnaround, departure delay, and service waiting time;
- delayed and total aircraft counts;
- fuel-truck and baggage-cart utilization;
- per-aircraft turnaround, departure delay, and service waiting time.

Batch simulations use `SimulationHistoryPolicy::Discard`. They do not retain the text event log, structured event history, or snapshots. The default policy remains `Retain`, preserving existing CLI and Unreal behavior. Experiment execution retains one compact `RunResult` per requested run so it can write deterministic run rows and calculate distributions.

For each configuration, continuous metrics provide count, mean, minimum, maximum, population standard deviation, median, P50, P90, and P95. Mean and population variance use Welford's stable online update. Median averages the two middle values for an even-sized sample. Percentiles use the nearest-rank definition: sorted element `ceil(p * n)`, with a one-based rank. Delayed outcomes provide mean delayed-aircraft count and probability that at least one aircraft is delayed.

## Output files

Files are buffered and written after simulation execution so the throughput measurement does not include per-run disk writes.

- `runs.csv`: one stable-order row per simulation, including parameters, seed, primary metrics, and per-run execution milliseconds.
- `summary.csv`: one row per case with distribution statistics, utilization means, mean delayed count, and probability of any delay.
- `experiment.json`: schema version, source files, UTC completion time, RampLab version, reported hardware concurrency, actual pool size, case/seed/replication/run counts, elapsed execution time, seed values, parameter definitions, and output filenames.

The generated `results/` tree is ignored. Keep experiment definitions under version control, but do not commit routine generated result directories.

## Development benchmark

`experiments/development_benchmark.yaml` expands to 5,000 runs. It is a development throughput sanity check, not a scientific benchmark. Run the same definition serially and in parallel:

```powershell
.\build\Release\airside_experiment.exe `
  --experiment experiments\development_benchmark.yaml `
  --workers 1 --output results\benchmark-1 --quiet

.\build\Release\airside_experiment.exe `
  --experiment experiments\development_benchmark.yaml `
  --workers 4 --output results\benchmark-4 --quiet
```

Use `execution_seconds` in each `experiment.json` for wall time and `run_count / execution_seconds` for simulations per second. Speedup is `serial_seconds / parallel_seconds`; parallel efficiency is `speedup / worker_count`. Results depend on the build, machine load, CPU power policy, and thermal conditions. Do not assume linear scaling.

## Validation and limitations

`experiments/small_validation.yaml` expands to six quick runs for CI or local equivalence checks. `capacity_sweep.yaml` and `disruption_resilience.yaml` are small operational examples. The tests cover loading errors, product ordering and IDs, typed application, stable statistics, exactly-once execution, deterministic output ordering, repeated parallel equivalence, history discard, and baseline/high-capacity regressions.

Graceful Ctrl+C cancellation is not implemented in this milestone. Interrupting the process can leave an incomplete output directory if it interrupts final serialization; use a fresh output directory for the next run. Output files are otherwise created only after all simulations finish.

The executor is local and in-process. There are no network workers, database, REST API, cloud services, optimization algorithms, sensors, or Unreal dependencies. A later Unreal feature can consume the existing `RunResult` and `CaseSummary` value models to visualize comparisons without moving execution or statistics into UObjects.
