# Goal 33: Operational Data, Replay, and Plan Mode

Goal 33 adds an operational-data layer around the Goal 30 Scenario Studio and
Goal 32 experiment/run interfaces, using the Goal 31 simulator foundation. It imports schedules, creates native RampLab scenarios, replays
timestamped observations, accepts local live updates, and compares plans with
observations. The simulator remains the source of simulated outcomes; imported
facts and assumptions retain separate provenance.

## Architecture

The implementation is split into four small layers in `tools/ops_dashboard`:

- `operational_time.py` parses offset-aware and airport-local timestamps,
  resolves timezone rules, and normalizes instants to UTC.
- `operational_schedule.py` reads CSV, JSON, and JSONL schedules, applies
  reusable mapping profiles, reports quality findings, and produces stable
  normalized dataset identities.
- `operational_store.py` persists normalized datasets and derives
  simulation-ready scenario inputs through the canonical Goal 24B adapter.
- `operational_replay.py` orders and applies observed events, exposes replay
  controls and plan-vs-actual summaries, and manages append-oriented live
  sessions and freshness.

Scenario creation hands off to the existing Scenario Studio registry and native
RampLab run path. Counterfactuals are variants of that generated scenario and
are executed by the same simulator. Goal 32's experiment builder consumes the
opened generated scenario and continues to use the existing experiment engine.

## Canonical data, profiles, and provenance

Schedule records preserve airport, operational date, flight and aircraft
identifiers, aircraft type, schedule times, stand, optional observed times,
status, source row, and source metadata. Missing input stays unknown unless a
profile explicitly supplies a default. Mapping profiles name source columns,
canonical targets, timestamp formats, timezone interpretation, required
fields, and explicit defaults. The demonstration profile is stored at
`tools/ops_dashboard/fixtures/operational/kauo-demo-schedule-profile.json`.

Quality findings include severity, field, source row, value, and a stable code.
The import UI shows findings before the user opens the operational day.
Normalized records retain source values and identifiers alongside normalized
ones. Dataset metadata includes source-file digest, profile, normalized digest,
operational date, and generation/version fields.

## Timezone handling

Input may be UTC, carry an explicit UTC offset, or be declared airport-local by
the mapping profile. Local values use the configured IANA timezone and are
converted to UTC for storage and ordering. Ambiguous or nonexistent local
times are surfaced as quality errors rather than silently guessed. The host
machine timezone is not used as an implicit operational timezone. KAUO's
timezone is configuration/profile data, not a hardcoded engine-wide rule.

## Plan workflow

In Scenario Studio, the Operations area supports file import, profile selection,
mapping and validation, normalized schedule inspection, stand occupancy, and
scenario generation. The generated schedule is adapted to the existing
canonical Goal 24A/24B data and opened in the existing Scenario Studio. A user
can run the native baseline and inspect Goal 19 output, then create Goal 32
experiments from that scenario.

Schedule facts remain scheduled facts. Inputs needed only to make a runnable
simulator scenario are marked as assumptions and surfaced in the UI. In the
synthetic KAUO demonstration, a five-minute pushback-preparation duration is an
explicit synthetic simulator assumption; it is not presented as an observed
turnaround service. If required information is unknown and no explicit
assumption is configured, scenario creation reports the missing information
instead of fabricating it.

The schedule view includes flight rows and a stand timeline. Overlapping
occupancy is flagged; the importer does not silently reassign a stand.

## Replay and Plan vs Actual

Replay input is versioned event data with event ID, type, event time, entity,
payload, and source. Events are normalized and ordered by operational timestamp
with a stable tie-break. Replay supports play, pause, step, seek, and speed
controls independently of browser wall-clock timing. State derived from an
event remains observed; unrecorded intermediate movement is unknown unless
simulated separately.

Plan-vs-actual output compares only fields present in both sources, reports
time variances where computable, and leaves absent actual values unknown. It
does not infer delay causes. Freshness distinguishes current, delayed, stale,
unknown, and future-dated observations; future timestamps are not reported as
current.

The checked-in event fixture is explicitly synthetic demonstration data. It is
not a record of real KAUO operations. See
`tools/ops_dashboard/fixtures/operational/kauo-synthetic-events.json`.
In the browser workflow, that fixture showed AX201 arriving at 07:04 versus a
07:00 schedule (+4 minutes), departing at 07:53 versus 07:45 (+8 minutes),
and AX202 arriving at 08:04 versus 08:00 (+4 minutes). AX202's departure and
AX203's actuals remain unknown. These are values in the synthetic fixture,
not historical KAUO observations.

A separate local live-update browser check submitted two synthetic AX201
events in reverse operational order (service start at 07:11, then arrival at
07:04), saw the second event marked late, closed the session, and converted it
to replay history. The entries are test observations and do not represent
airport feed data; see `docs/screenshots/goal33/09-live-session-recorded-replay.png`.

## Counterfactuals

A user can create a simulated variant from the operational schedule or replay
baseline, choose a point in time, and add a disruption such as a taxiway
closure. The variant is executed by RampLab's normal scenario/run pipeline.
Its metrics are simulation results and are labeled as counterfactual; they do
not replace or alter the observed event history.

## Local live-update interface

The local server exposes versioned operational endpoints for creating a live
session, appending events, reading state, closing the session, and exporting its
recording. Each accepted event retains both its operational timestamp and
receipt timestamp. Event IDs provide idempotency. Late events are accepted into
the append history and replayed in deterministic event-time order; receive
order is not treated as operational order. Malformed events are rejected.

This is a local live-update-capable interface. No airline or airport feed is
connected by this goal, and endpoint availability is not evidence of live
airport telemetry.

## Identity, export, and Goal 32 integration

Stable hashes cover source bytes, normalized data, replay ordering, and
simulation inputs. Scenario/run provenance links a generated scenario to its
dataset and profile. Normalized schedules, mapping profiles, event histories,
replay timelines, Plan-vs-actual summaries, and live-session recordings are
exportable in versioned machine-readable formats.

The existing Goal 32 experiment interface is used without copying or
reimplementing its sweep engine. The imported schedule's generated scenario is
the experiment baseline; registered simulator-backed parameters and native
simulations produce results. An experiment based on a schedule therefore
inherits its source dataset and explicit assumptions.

The browser-verified integration experiment used the imported synthetic KAUO
schedule (`2026-10-12`, dataset `ops_bb2d85a392da7bca2738`) and its generated
`Plan · KAUO · 2026-10-12` scenario. It varied route-closure start
(`12:10Z`, `12:40Z`) and duration (15, 30 minutes), across seeds 42 and 43,
and included the unchanged baseline: 5 configurations and 10 native runs at
4 workers. All 10 runs completed with valid artifacts; there were no failed,
invalid, or cached runs. The highest-ranked tested configuration was the
unchanged baseline: mean departure delay 1.50 minutes, taxi time 1,092 seconds,
taxi distance 5,418.49 metres, zero reported collision/safety-failure metrics,
and 3 departures. One tested 30-minute closure beginning at 12:40Z changed
taxi time to 1,296 seconds and distance to 6,432.24 metres while the other
reported metrics matched the baseline. The ranking is only within this small
tested parameter space. Run records have second-level timestamps; this small
integration sweep is not used as a parallel performance benchmark.

## Demonstration and validation

The CSV schedule fixture deliberately uses external-style column names and is
marked synthetic. The event fixture is also synthetic. A real native RampLab
Plan run and counterfactual run were executed from the imported schedule; their
metrics come from simulator artifacts, not mock values. The Plan run
(`884a640e16d847cda72f5e8e9fc09400`) completed all 3 turnarounds and reported
5,418 metres of taxi distance and 1,092 seconds of taxi time. The native
counterfactual run (`945e2264ba364599b368fd171e763610`) also completed. Screenshots
for import/mapping, quality, Plan, stand occupancy, replay, Plan vs Actual,
counterfactual, the imported-schedule Goal 32 experiment builder and results,
and a recorded local live session are under `docs/screenshots/goal33/`.

Relevant validation includes operational data/store unit tests, the existing
Scenario Studio and experiment tests, Python compilation, JavaScript syntax
checks, native scenario generation and simulation, and CTest. The exact
results are recorded with the Goal 33 completion report and local commit.

In this worktree, Release CTest discovered and passed 186 tests. The combined
Goal 30/32 dashboard, Scenario Studio, experiment, Goal 24A/24B, Goal 19,
Goal 22, and Goal 33 Python suites passed 122 tests. A separate Goal 28
live-session integration run passed all 4 tests against the Release CLI. One
additional environment-sensitive Goal 24A test assumes timezone data is absent;
it passes when run with the `tzdata` module deliberately unavailable. This
environment has timezone data installed for Goal 33's DST conversion.
Python compilation, JavaScript syntax, and `git diff --check` passed.

## Goal 31 Integration

Goal 33 was developed on the Goal 32 branch and is now integrated after Goal 31
and Goal 32 became the canonical `main` foundation. Scenario creation uses the
current canonical generator and native simulator path, including Goal 31's
turnaround and routed GSE capabilities. The checked-in schedule fixture remains
synthetic and models only its explicit schedule and pushback-preparation
assumption; it does not claim observed GSE availability, service events, or
resource assignments. Operational import and replay remain decoupled from fleet
internals, while optional event payloads and provenance leave room for those
records to be modeled explicitly when a source provides them.

## Known limitations

- The checked-in KAUO schedule and history are synthetic demonstration data,
  not airport operational records.
- No external airport feed, airline connection, or production live service is
  configured.
- Simulation assumptions are limited to what the current canonical generator
  and simulator support; unsupported operational detail remains unknown.
- Replay reproduces the supplied event sequence and known state only. It does
  not claim to reconstruct unrecorded history.
- Timezone conversion depends on timezone database availability. The optional
  `requirements-operational-data.txt` pins `tzdata` for environments that need
  a bundled timezone database.
