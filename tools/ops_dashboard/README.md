# RampLab Scenario Studio and Operator Experiment Dashboard

The local server opens **Scenario Studio** at [http://127.0.0.1:8765](http://127.0.0.1:8765). It is a no-code workspace for editing supported Goal 24A airport packages, validating Goal 24B mappings, generating native RampLab runs, comparing Goal 19 results, and exporting the generated scenario package. Scenario drafts and runs are stored under the current user's local application data directory. The server binds to loopback by default.

The original Goal 20 run inspection dashboard remains at [http://127.0.0.1:8765/dashboard](http://127.0.0.1:8765/dashboard); its upload, analysis, replay, and synthetic demo workflows are unchanged.

The Studio offers the canonical KAUO package, the Goal 27 KAUO prototype, and the Goal 24A synthetic example as source datasets. It does not expose the full Goal 23 scenario catalog as editable templates: those packages use different inputs and mappings. Airport and simulator controls are intentionally limited to fields consumed by the selected Goal 24B generator. In particular, the KAUO map's FAA runway references are authoritative while taxiway, apron, stand placement, and the illustrative service assumption are approximate. The Studio is a scenario-planning prototype, not a live airport operations system or digital twin.

A small local control-room interface for inspecting RampLab outputs, comparing two runs, and stepping through recorded events. It runs on the local machine, uses no database or cloud services, and does not modify source experiment files.

## Launch

From the repository root, with Python 3.10 or newer:

```powershell
python tools/ops_dashboard/server.py
```

Open [http://127.0.0.1:8765](http://127.0.0.1:8765). Stop the server with Ctrl+C. The server binds to loopback by default. Files are read in the browser, sent to this local process, analyzed in a temporary directory, and discarded when the request finishes.

## Architecture

- `server.py` is a dependency-free Python HTTP server. It accepts one or two run bundles, parses files, and returns normalized run data and reports.
- `index.html`, `style.css`, and `app.js` implement the interface and event stepper with no front-end package manager or build step.
- `tools/experiment_analysis/analyze.py` is the Goal 19 analysis module. The dashboard reuses its `load_run`, `summarize`, `compare`, `determinism`, KPI definitions, safety assessment, ranking, and Markdown report code.
- `fixtures/` contains synthetic seed-42 control and disruption output for an offline demo.

## Loading and comparing runs

Select **Load run files** and choose one or more JSON, CSV, or JSONL outputs belonging to a run. The dashboard associates the files selected in one upload as one run. Add a second run with **Add comparison**, then choose it in **Compare with**. For JSON summaries named `experiment.json`, `summary.json`, or `metrics.json`, a sibling `runs.csv` uploaded in the same bundle is joined by Goal 19 where supported. Aircraft CSV rows are shown in entity inspection; JSONL files supply ordered history. Each input reports whether it loaded, failed to parse, or used an unsupported extension. Missing optional formats are listed as not provided.

## Views

- **Operations overview:** selected run, available KPI cards, a small relevant measure comparison, status reasoning, disruption preview, and file coverage.
- **Aircraft & tasks:** rows from an aircraft/entity CSV export, with source columns retained.
- **Disruptions:** exported closure, failure, reroute, reassignment, outage, recovery, and timeout records. Comparison consequences are numeric Goal 19 KPI differences; event proximity is not treated as causation.
- **Safety review:** collision, minimum separation, failure, timeout, and completion measures present in the source.
- **Event replay:** chronological list, play/pause, restart, step, seek, and adjustable speed. This is a timed event log replay only; it does not reconstruct positions or motion.
- **Run analysis:** Goal 19 ranked KPI changes, safety and identity findings, optional repeat-run determinism for structured outputs and supplied JSONL event streams, Markdown report, and machine-readable JSON.

## Safety semantics

No missing collision field is treated as zero. A single run with collision fields present and all zero is labeled **No collisions observed**; a positive count is **Collision detected**. When collision metrics are absent, the safety label is **Insufficient safety data** and the reason is shown. For a comparison, the Goal 19 safety findings are displayed as produced by that module. These summaries describe exported simulation observations and are not a real-airport safety determination.

## Synthetic demo

The **Control** and **Disruption** buttons load illustrative files in `fixtures/control/` and `fixtures/disruption/`. Both are synthetic, seed 42, and follow the committed experiment.json and runs.csv export structures plus aircraft CSV and JSONL event records. The disruption demonstrates increased taxi distance/time, queueing, rerouting, and a service reassignment, with zero recorded collisions in both runs. The fixtures are dashboard examples, not simulator-generated validation outputs.

## Real RampLab demo

The repository-level workflow [docs/goal21-real-operations.md](../../docs/goal21-real-operations.md) generates simulator-backed bundles and checks their cross-artifact consistency. After generation, start the dashboard with:

```powershell
python tools/ops_dashboard/server.py --real-demo-dir results/goal21-real
```

The **Real RampLab runs** banner loads the generated control and road-closure bundles. This acceptance pair uses Goal 18's `mixed_runway_operations` and `mixed_runway_disrupted` scenarios, seed 42. Goal 18 native metrics, aircraft records, and ordered events are preserved, with a thin adapter for Goal 19/20's `experiment.json`, `runs.csv`, and dashboard entity table. Replay uses the simulator's ordered operational event log, not physical-motion playback. The real bundles include aircraft taxi/runway, collision, and minimum-separation measures; missing values in other schemas remain unavailable and safety unknown.

Validate a bundle independently with `python tools/ops_dashboard/validate_real_bundle.py results/goal21-real/disruption`. The runner emits repeated runs and Goal 19 metric/event determinism reports alongside the primary comparison.

## Data limits

- Current batch experiment exports contain aggregate run KPIs and turnaround/task details; the batch executor discards event history. A timeline is available only when a JSONL event stream or embedded event list is supplied.
- Individual aircraft records need a CLI aircraft CSV or another entity CSV. Aggregate `runs.csv` does not provide the same aircraft table.
- Some event streams do not expose an explicit simulation timestamp or affected entity; those fields appear unavailable rather than being inferred.
- Current data has no complete recovery-duration KPI. Disruption consequence is shown only where numeric exported fields support it.
- Not every schema contains completion, delay, runway, taxi, separation, or collision metrics. Missing KPIs remain unavailable, and unsupported fields are not synthesized.
- Goal 19's metric catalog determines which numeric measures can be compared and how KPI changes are ranked. Repeat-run event determinism is available when both uploaded run bundles include JSONL event files. A KPI difference alone does not establish a causal mechanism.

## Adding future outputs

Use the file picker to load local outputs in the formats accepted by Goal 19: `experiment.json` run results and JSON summaries, CSV summaries or runs, and JSONL events. Select related files together. When a future simulator or experiment export provides new names, first add those names to Goal 19's supported metric catalog or export a currently recognized field; this dashboard deliberately does not change simulator schemas or guess field equivalence.

## Tests

Run both the dashboard tests and the existing Goal 19 tests:

```powershell
python -m unittest discover -s tools/ops_dashboard -p "test_*.py" -v
python -m unittest discover -s tools/experiment_analysis -p "test_*.py" -v
```
