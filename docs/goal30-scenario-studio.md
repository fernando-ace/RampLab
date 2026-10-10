# Goal 30: RampLab Scenario Studio

## What changed

Scenario Studio is the local landing page for `tools/ops_dashboard/server.py`. The existing Goal 20 dashboard remains at `/dashboard`. The Studio edits the canonical Goal 24A dataset inputs, checks relationships and supported controls, invokes Goal 24B generation and validation, runs the generated scenario through the native RampLab CLI and Goal 19 artifact adapter, and compares completed runs with Goal 19.

The Airport panel also reads the Goal 23 release scenario catalog as a reference. Those native catalog entries keep their own YAML packages and mappings; they are not presented as editable KAUO inputs. The Studio's editable datasets are the canonical KAUO prototype, the Goal 27 two-aircraft KAUO prototype, and the synthetic regression package.

The user can edit existing flights, supported fleet fields, turnaround requirements, the random seed, and mapped disruptions. Saved configurations are versioned local JSON. A ZIP export contains generated `scenario.json`, `manifest.json`, `identity-map.json`, and `support-matrix.json`. Completed runs include simulator metrics, events, Goal 19 artifacts, and the generated package. The Results view links completed scenarios to the Goal 29 Unreal viewer launcher.

## Airport-data and support boundaries

KAUO is an illustrative calibrated simulation prototype. Runway references are FAA sourced; taxiway, apron, and stand geometry are approximate. The selected KAUO package represents an illustrative arrival/departure pair, one representative aircraft archetype, one assumed stand, and an abstract two-minute pushback-preparation service. Public airport material does not establish these as observed flights, actual stand positions, measured service times, or an actual equipment inventory.

The current KAUO mapping supports one route closure, on `KAUO-TWY-A-RAMP-CONNECTOR`. The selected KAUO package has no mapped vehicles, so vehicle outages are unavailable there. The Goal 27 and synthetic packages expose their own mapped vehicles and locations. Timed vehicle recovery, stand outages, weather, and adding a new flight are not offered. Vehicle outages last through the run; service delays require an existing generated service task. Unsupported equipment remains visible as read-only metadata.

## Native KAUO run evidence

The baseline and closure variant were generated and run in the native simulator with seed 42, then compared by Goal 19. The only variant change was a 45-minute closure of the mapped taxiway route. Both runs completed the same single turnaround and two runway operations.

| Measure | Baseline | Closure variant | Change |
| --- | ---: | ---: | ---: |
| Taxi distance | 1,806 m | 3,834 m | +2,028 m |
| Taxi time | 364 s | 772 s | +408 s |
| Arrival taxi distance | 436 m | 1,450 m | +1,014 m |
| Completed turnarounds | 1 | 1 | 0 |
| Completed runway operations | 2 | 2 | 0 |
| Surface reroute events | 0 | 0 | 0 |
| Exported collision counts | 0 | 0 | 0 |

Goal 19 labels the taxi-distance/time changes as regressions and reports no safety regression because both run exports record zero collisions and no new safety failures. These are simulated observations for the selected assumptions, not an airport safety determination. The closure was present before route planning; the simulator took a longer path without emitting a post-assignment reroute event.

The Goal 29 viewer launcher passes the completed run's generated `scenario.json`. The UE 5.8 C++ core built, but the editor target could not build because the worktree does not contain the required `CesiumForUnreal` plugin. The viewer runtime was therefore not launched; the Studio now returns a readable build error and server output identifies the missing plugin.

## Screenshots

All images are 1920 × 1080 browser captures.

1. [Airport overview](screenshots/goal30/01-overview.png)
2. [Supported map route](screenshots/goal30/02-selected-route.png)
3. [Taxiway closure editor](screenshots/goal30/03-disruption-editor.png)
4. [Validation and baseline diff](screenshots/goal30/04-validation-diff.png)
5. [Native baseline and variant comparison](screenshots/goal30/05-baseline-variant-compare.png)
6. [Variant closure on the airport map](screenshots/goal30/06-variant-map-closure.png)

Supplemental states: [completed run list](screenshots/goal30/03-baseline-results.png) and [saved variant dialog](screenshots/goal30/06-saved-variant.png).

## Run and verify

Start the local Studio from the repository root:

```powershell
python tools/ops_dashboard/server.py
```

The root path opens Scenario Studio; `/dashboard` opens the original experiment dashboard. The Studio stores configurations and run records under the current user's local application data. A first run builds `airside_cli` if needed.

Focused Studio and integration checks:

```powershell
python -m unittest tools.ops_dashboard.test_scenario_studio -v
python -m unittest tools.airport_scenario_generation.test_generation tools.airport_scenario_generation.test_run_artifacts -v
python -m unittest discover -s tools/experiment_analysis -p "test_*.py" -v
python -m unittest discover -s tools/ops_dashboard -p "test_*.py" -v
ctest --test-dir build-goal30 -C Release --output-on-failure
```

The browser verification used the real local UI to validate, save, run, and compare the KAUO baseline and closure variant. Implementation and browser verification ran in an isolated worktree; the primary checkout was untouched.
