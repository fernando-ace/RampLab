# RampLab experiment analysis

This standard-library Python tool reads existing RampLab experiment output and creates a readable run summary or a control-versus-comparison assessment. It is isolated from the simulator and does not alter output formats.

## Inputs

- `experiment.json` from `airside_experiment`, including its `runs` array. When sibling `runs.csv` exists, it is joined by ordinal to add the full exported KPI set.
- `runs.csv` or `summary.csv` from the experiment writer. Rows are summarized by the arithmetic mean for each supported numeric metric; for `summary.csv`, distribution-statistic columns are treated as numeric KPIs as present.
- JSON Lines event streams, counted by their `type` or `event` field while retaining order for determinism comparisons.
- A JSON object containing numeric RampLab fields, useful for compact or historical summaries.

Missing columns are omitted. The tool does not invent values or treat absent fields as zero. When an input contains multiple runs, the displayed KPI values are means across rows. Use a one-run experiment output when you need a run-level comparison.

## Commands

From the repository root:

```powershell
python tools/experiment_analysis/analyze.py results\small_validation\experiment.json
python tools/experiment_analysis/analyze.py control\experiment.json disruption\experiment.json --output comparison.md --json comparison.json
python tools/experiment_analysis/analyze.py repeat-a\experiment.json repeat-b\experiment.json --determinism --output determinism.md --json determinism.json
python -m unittest discover -s tools\experiment_analysis -v
```

The first path is the baseline. Shared numeric KPIs receive an absolute difference and a percentage difference when the baseline is nonzero. Zero-baseline percentages are `null`. KPI impact direction is defined in the source metric catalog; safety/throughput findings receive dedicated treatment. Ranking is stable, with a fixed tie-break order.

The safety assessment distinguishes collision changes, reduced minimum separation, newly introduced failures/timeouts, incomplete turnarounds, and unavailable evidence. Absence of collision fields results in an unknown safety status. No single KPI can establish overall safety.

Determinism mode first checks input-file byte identity, then compares normalized numeric metrics and ordered run/event records. It reports changed metric fields and record counts. Different JSON metadata can contain wall-clock values; the comparison focuses on exported structured results.

## JSON output

Comparison JSON uses `schema_version: 1`, baseline/comparison identity and source, a `metrics` array (`baseline`, `comparison`, `difference`, nullable `percent_difference`, `impact`, `category`, and `unit`), deterministic `operational_impact` ordering, safety findings, and compatibility information. Determinism mode adds a `determinism` object. This is suitable as a future dashboard input without duplicating comparison logic.

## Current export limits

The current experiment writer exports run-level operational and safety KPIs in `runs.csv`; `experiment.json` has a smaller subset plus turnaround/task detail. Its batch executor intentionally discards event history, so event streams are supported when supplied separately but cannot be reconstructed from current batch files. The schema has no explicit recovery-duration KPI, and current exports do not provide every scenario-level task or timeout detail. A multi-run aggregate cannot establish per-seed deterministic equivalence; use repeated one-run outputs for that assessment. Historical formats with different field names are only analyzed when their names match supported output fields.
