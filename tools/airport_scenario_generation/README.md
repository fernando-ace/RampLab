# Airport scenario generation (Goal 24B)

This adapter consumes a valid Goal 24A canonical JSON package and emits an existing RampLab scenario document as deterministic JSON (JSON is valid YAML for the native `yaml-cpp` scenario loader). It does not ingest raw airport files or add simulator operational behavior. The checked in synthetic example is explicitly synthetic and is not calibrated to an airport.

## Workflow

From the repository root:

```powershell
python -m tools.airport_data_ingestion build tools/airport_data_ingestion/examples/synthetic/manifest.json --output results/goal24b/canonical.json
python -m tools.airport_scenario_generation validate results/goal24b/canonical.json --mapping tools/airport_scenario_generation/mapping.synthetic.json
python -m tools.airport_scenario_generation generate results/goal24b/canonical.json --mapping tools/airport_scenario_generation/mapping.synthetic.json --output results/goal24b/generated
python -m tools.airport_scenario_generation inspect results/goal24b/generated
python tools/release/ramplab.py run --scenario-file results/goal24b/generated/scenario.json --seed 42 --output results/goal24b/run
```

The release `run` command also retains its five catalog choices. `--scenario-file` routes a generated file into the same C++ simulator and native metrics/event outputs. The release runner also writes Goal 19-compatible `experiment.json`, `runs.csv`, and `aircraft.csv` views while retaining the native `simulator-metrics.*` files and ordered `events.jsonl`. For generated scenarios, the experiment view embeds the generation manifest, identity map, and support matrix so Goal 22 evidence retains the canonical provenance. Generate a matched no-disruption control with `generate ... --without-disruptions`; this option changes only whether canonical disruption records are applied.

The `tools.airport_scenario_generation.determinism` command compares control metrics, control events, disruption metrics, and disruption events from two seeded runs of each generated scenario. Its report uses the repeat-run contract expected by Goal 22.

For geospatial inputs, `geospatial.py` converts WGS-84 geographic coordinates to and from airport-local ENU meters. Scenario XY uses the same ENU convention; Unreal maps meters to centimeters and negates north for its south-positive Y axis. KAUO's FAA runway-end calibration and numerical report are reproducibly rebuilt with `python -m tools.airport_scenario_generation.calibrate_kauo`. See [KAUO_CALIBRATION.md](KAUO_CALIBRATION.md) for CRS, source classification, measured errors, and the limits of approximate taxiway/apron geometry.

When UE 5.8 is available, add `--unreal` to the release `run --scenario-file ...` command to build and open the existing viewer with `-RampLabScenarioFile=<generated scenario path>`. The viewer uses the same C++ scenario loader and advances the generated scenario at an accelerated 60x operator playback speed.

The output directory contains:

- `scenario.json`: native RampLab scenario configuration, validated by the simulator loader at execution.
- `manifest.json`: package, mapping and geometry hashes; source provenance; deterministic epoch and seed; entity counts; warning list; and output hashes.
- `identity-map.json`: canonical flight, aircraft, gate, equipment, turnaround requirement, and disruption identifiers mapped to simulator identifiers.
- `support-matrix.json`: the feature support contract included with this generated scenario.

Service semantics follow the current native simulator: fueling dispatches to the fuel-truck capability; other mobile tasks use generic baggage-delivery dispatch. Generated vehicle labels such as `catering` are retained, but they do not add a distinct simulator capability or performance model.

Repeated generation with identical package bytes, mapping, geometry, seed, and disruption option produces identical output files. No timestamp or machine path is embedded in generated artifacts. The scenario epoch is the earliest flight operation timestamp or canonical disruption timestamp, including when generating a matched no-disruption control. This keeps flight schedules aligned across control and disruption variants. UTC instants become rounded integer seconds from that epoch; local timezone interpretation is not performed here.

## Validation and inspection

`validate PACKAGE --mapping MAP` checks the Goal 24A schema/version, valid status, required sections, relationships, UTC timestamps, geometry references, and mapping types. Required services without a mapping fail generation. `validate --generated DIRECTORY` checks entity and disruption references plus output hashes. `inspect DIRECTORY` reports source hashes, mapped entity counts, warnings, file names, and scenario SHA-256.

Use the native release output with Goal 19, Goal 20, and Goal 22. For example, run the generated scenario twice from the release CLI, compare the native `simulator-metrics.json` outputs and ordered `events.jsonl`, then analyze/package those real run directories. The dashboard accepts their JSON, CSV, and JSONL files through **Load run files** and its event replay controls.

See [MAPPING.md](MAPPING.md), [SUPPORT_MATRIX.md](SUPPORT_MATRIX.md), and [END_TO_END.md](END_TO_END.md) for mapping policy and known gaps.
