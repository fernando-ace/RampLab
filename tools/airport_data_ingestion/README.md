# RampLab airport data ingestion (Goal 24A)

This standalone, standard-library Python CLI turns local airport operations CSV/JSON files into a validated, normalized, deterministic canonical JSON package. It does not generate scenarios or connect to RampLab, ROS, Unreal, feeds, APIs, or services. The example data is synthetic and does not describe actual airport operations.

From the repository root:

```powershell
python -m tools.airport_data_ingestion validate tools/airport_data_ingestion/examples/synthetic/manifest.json --report validation.json
python -m tools.airport_data_ingestion normalize tools/airport_data_ingestion/examples/synthetic/manifest.json --output airport-data.json
python -m tools.airport_data_ingestion build tools/airport_data_ingestion/examples/synthetic/manifest.json --output airport-data.json
python -m tools.airport_data_ingestion inspect airport-data.json
python -m unittest tools.airport_data_ingestion.test_ingestion -v
```

The manifest is JSON with `schema_version: "1.0"`, an `airport` object (`airport_id`, IANA `timezone`, and optional name/IATA/ICAO), optional `dataset_id`/`description`, and `files` mapping table names to relative CSV or JSON paths. Flights, aircraft, and gates are required; equipment, turnaround requirements, and disruptions are optional. Source paths cannot escape the manifest directory. JSON sources are arrays of objects (or an object containing `records`); CSV uses its header row.

Commands `validate`, `normalize`, and `build` accept `--report` for deterministic machine-readable validation JSON. `normalize` and `build` require `--output`; neither writes a package when errors block validation. `inspect` summarizes an existing package. Exit codes are 0 for valid input, 1 for validation errors, and 2 for malformed CLI/package input. Findings include stable codes and source row context when available.

Records and findings are sorted deterministically; JSON uses sorted keys, UTF-8, indentation, and a final newline. Timestamps are converted to UTC ISO-8601 (`Z`); naive local times use the manifest timezone, and DST ambiguous/nonexistent times require an explicit offset. IDs have surrounding whitespace trimmed and collisions fail. Durations are nonnegative minutes. Source provenance stores relative filename, format, record count, and SHA-256 of raw bytes. Identical input bytes and manifest produce byte-identical package and report; CSV and JSON equivalents have different provenance hashes, so compare operational sections for semantic equivalence.

See [SCHEMA.md](SCHEMA.md) for the canonical contract and [RAMPLAB_MAPPING.md](RAMPLAB_MAPPING.md) for deferred Goal 24B integration. Known limits: CSV/JSON only; route IDs are structurally retained but cannot be checked against simulator geometry; timestamp handling rejects DST fold/gap cases rather than guessing; no geometry, execution, or internal RampLab mapping is implemented.
