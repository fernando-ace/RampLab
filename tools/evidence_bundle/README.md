# RampLab Engineering Evidence Bundle

Generate a portable package from existing experiment output directories. The tool never runs or controls the simulator. It uses the Python standard library and Goal 19's parser and KPI catalog.

## Usage

From the repository root:

    python -m tools.evidence_bundle --input path\to\experiment-output --output path\to\bundle
    python -m tools.evidence_bundle --control path\to\control --disruption path\to\disruption --output path\to\bundle

Each input directory may contain experiment.json (or summary.json/metrics.json), runs.csv, summary.csv, aircraft.csv, and events.jsonl. JSON or run/summary CSV is required. The output directory must be new or empty.

## Bundle contents

- README.md contains experiment identity, operational KPIs and comparison changes.
- manifest.json records source paths, byte sizes, SHA-256 hashes, recognized and missing artifacts, creation time, scenario/seed/run identity, and statuses.
- kpis.json contains observed metrics and, for two inputs, absolute and percentage changes. Percentage change is unavailable when the baseline is zero.
- timeline.json preserves each JSONL file's source line and recorded event order.
- validation.json reports readability, identity, seed/run count, aircraft totals, safety evidence, event parseability, missing evidence, and copied-file hashes.
- index.html is a static local report with no CDN or network dependency.
- evidence/ contains byte-for-byte copies of the recognized source files.

Safety remains INSUFFICIENT DATA unless collision and minimum-separation evidence are both present. Recorded zeros mean only that no collisions were observed in those exported fields; positive collision or safety failure KPIs are reported as FAIL. This is not a real-airport safety determination. A control/disruption comparison is not repeat-run determinism evidence. The tool does not reconstruct motion from events.

## Synthetic demonstration and tests

The Goal 20 fixtures are synthetic examples conforming to the merged experiment JSON, run CSV, aircraft CSV, and JSONL schemas. Generate the example comparison with:

    python -m tools.evidence_bundle --control tools\ops_dashboard\fixtures\control --disruption tools\ops_dashboard\fixtures\disruption --output tools\evidence_bundle\demo_bundle_final

Tests use temporary output directories:

    python -m unittest tools.evidence_bundle.test_bundle -v
