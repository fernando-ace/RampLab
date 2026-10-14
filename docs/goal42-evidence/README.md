# Goal 42 portable evidence

This folder is the self-contained evidence set for the Goal 42 KAUO geometry and event-replay prototype. Start with `../goal42-authoritative-kauo-digital-twin.md`, then inspect:

- `geometry/kauo_goal42.geometry.json`: normalized KAUO geometry, per-class provenance, and 94-edge polygon-overlap diagnostic.
- `sources/`: original ALDOT FeatureServer GeoJSON query, layer metadata, and acquisition manifest.
- `coordinate-transform.json`: source CRS, KAUO ENU origin, Unreal axis mapping, and known-point assertions.
- `scenario-generation/mapping.kauo_goal42.json`: profile that connects the canonical KAUO package to Goal 42 geometry.
- `run/`: Goal 42 control/disruption and repeat run artifacts, generated scenarios, structured decisions, canonical event histories, Goal 19 analysis, and validation. The `run/` name refers to this completed Goal 42 run; the runner is reused from Goal 41.
- `live-session/`: a Goal 42 configured disruption executed through the simulator-backed `LiveSession` API, including session state and completed run artifacts; this does not establish browser UI behavior.
- `unreal-adapter-*.jsonl`: deterministic point samples transformed from canonical simulator events to Unreal coordinates. Missing poses are counted and not synthesized. The Unreal project itself was not validated in-editor.
- `validation.json`: test/build/run/browser/Unreal status summary.
- `SHA256SUMS.txt`: SHA-256 ledger for portable files other than the ledger and final ZIP.
- `goal42-portable-evidence.zip` and `.sha256`: final bundle and archive digest.

## Evidence interpretation

ALDOT polygons are pavement inventory extents, not certified taxiway centerlines. The simulator still uses the inherited approximate route graph. Aircraft and ground-vehicle event poses are authoritative simulator snapshot samples at event times; they are not observed airport positions. Unreal adapter coordinates are deterministic transforms of those samples, not evidence of successful Unreal rendering. No visual Dashboard proof is included because Chrome CDP evaluation timed out in the available session.

All run schedules, fleet composition, task requirements, durations, and the pushback service delay are illustrative simulation assumptions. No operational-use or certification claim is made.
