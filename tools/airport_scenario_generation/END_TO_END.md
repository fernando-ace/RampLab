# End-to-end example

The input fixture is the checked-in Goal 24A synthetic source package. Goal 24A builds its canonical package; Goal 24B resolves its IDs and produces the existing RampLab scenario format; the Goal 23 CLI runs the native simulator and exports normal metrics and events.

```powershell
python -m tools.airport_data_ingestion build tools/airport_data_ingestion/examples/synthetic/manifest.json --output results/goal24b/canonical.json
python -m tools.airport_scenario_generation generate results/goal24b/canonical.json --mapping tools/airport_scenario_generation/mapping.synthetic.json --output results/goal24b/disruption
python -m tools.airport_scenario_generation generate results/goal24b/canonical.json --mapping tools/airport_scenario_generation/mapping.synthetic.json --output results/goal24b/control --without-disruptions
python tools/release/ramplab.py run --scenario-file results/goal24b/control/scenario.json --mode control --seed 42 --output results/goal24b/runs/control
python tools/release/ramplab.py run --scenario-file results/goal24b/control/scenario.json --mode control --seed 42 --output results/goal24b/runs/control-repeat
python tools/release/ramplab.py run --scenario-file results/goal24b/disruption/scenario.json --seed 42 --output results/goal24b/runs/disruption
python tools/release/ramplab.py run --scenario-file results/goal24b/disruption/scenario.json --seed 42 --output results/goal24b/runs/disruption-repeat
python tools/experiment_analysis/analyze.py results/goal24b/runs/control/experiment.json results/goal24b/runs/disruption/experiment.json --output results/goal24b/analysis.md --json results/goal24b/analysis.json
python -m tools.airport_scenario_generation.determinism --control results/goal24b/runs/control --control-repeat results/goal24b/runs/control-repeat --disruption results/goal24b/runs/disruption --disruption-repeat results/goal24b/runs/disruption-repeat --output results/goal24b/determinism.json
python -m tools.evidence_bundle --control results/goal24b/runs/control --disruption results/goal24b/runs/disruption --determinism-report results/goal24b/determinism.json --output results/goal24b/evidence
python tools/ops_dashboard/server.py
```

For the generated scenario in Unreal, use `python tools/release/ramplab.py run --scenario-file results/goal24b/disruption/scenario.json --seed 42 --output results/goal24b/runs/unreal --unreal` on a machine with UE 5.8 installed. The current viewer uses the same native loader, and the scenario file override selects the generated configuration.

In the dashboard at `http://127.0.0.1:8765`, load each run's `experiment.json`, `aircraft.csv`, and `events.jsonl` files. Inspect Overview, Aircraft, Disruptions, Safety, and Run analysis. Open Event replay and exercise play/pause, restart, step, seek, and speed. Replay follows recorded events and does not synthesize movement.

The Goal 19 comparison reports control-versus-disruption impact. The Goal 24B determinism command separately compares both repeated metric files and both ordered event logs in the four-check format Goal 22 validates. Each release run preserves native outputs and writes `experiment.json`, `runs.csv`, and `aircraft.csv`; generated runs embed the scenario document, manifest, identity map, and support matrix in `experiment.json`, which Goal 22 copies byte-for-byte into the evidence bundle.

## Limits and future calibration work

The fixture deliberately contains a tug outage that cannot be modeled because Goal 24B has no validated tug vehicle class. The manifest reports that unsupported outage. Its route closure does resolve to a real geometry edge and is recorded as a native road event. A route event occurring after the fixture's flight operations may not change a flight's path; the event is represented honestly and no effect is claimed unless measured in the simulator output.

Goal 26 calibrates the KAUO runway layer to FAA NASR runway-end coordinates. Surveyed taxiway centerlines, pavement/apron boundaries, stand coordinates, route identity mapping, a real schedule source, verified equipment inventory and spawn locations, realistic turnaround durations and dependencies, aircraft movement calibration, and operational validation against observed data remain unavailable. This Goal 24B synthetic mapping remains synthetic and is not KAUO calibration or real-airport fidelity.
