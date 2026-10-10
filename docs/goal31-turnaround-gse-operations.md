# Goal 31: Turnaround and GSE Operations

## Worktree and Goal 29/30 baseline

This worktree was created from `62a635c` (`Add local RampLab Scenario Studio`), which contains the Goal 29 KAUO viewer and Goal 30 Scenario Studio. The primary checkout was not modified. Goal 30's reported editor-build failure is consistent with the repository setup: `CesiumForUnreal` 2.29.1 is downloaded by `unreal/RampLabViewer/Scripts/InstallCesium.ps1` into `unreal/RampLabViewer/Plugins/CesiumForUnreal/`, and `.gitignore` excludes that directory. It is therefore neither Git-tracked nor shared automatically between worktrees. The project does not reference an absolute plugin path; Unreal discovers it as a project plugin. The existing bootstrap script pins the release and verifies its checksum, so installing it in each checkout is the safe reusable bootstrap. No plugin or ion credential is vendored. This execution environment has no Unreal Editor, PowerShell, or installed plugin, so an editor build and synchronized GSE capture cannot be performed here.

## Turnaround task model

Turnaround services remain explicit `ServiceTask` values on an aircraft. Each task carries a stable task ID, service type, duration, earliest start, prerequisites, status, required resource, assigned vehicle, and request/start/completion timestamps. The event scheduler starts tasks only after their dependencies complete; tasks without dependency edges can proceed concurrently. Scenario loading already checks missing task references and cyclic dependency graphs.

The native service vocabulary now includes baggage unload, baggage load, fueling, catering, cabin cleaning, deboarding, potable water, lavatory service, ground power, pushback, passenger stairs, air start, maintenance inspection, chocks, and abstract pushback preparation. Service types are reusable simulator concepts, not airport-specific values. Pushback preparation is kept distinct from a tug dispatched for a pushback task.

Mobile capabilities include a pushback tug, baggage vehicle/cart, fuel truck, catering truck, lavatory truck, potable-water truck, mobile GPU, passenger stairs, and air-start unit. These use the existing deterministic fleet dispatcher and its route planner, reservations, closures, rerouting, safety checks, outage handling, and reassignment. No per-task vehicle is created. Mobile tasks with no matching vehicle remain queued. Cabin cleaning, deboarding, maintenance inspection, chocks, and pushback preparation use the existing finite abstract crew capacity model; stationary ground power is currently represented as a routed mobile GPU when configured as a vehicle.

## Assumptions and configuration

Durations on task records override configured service defaults. The scenario generator supplies demonstration defaults in seconds: fueling 720, baggage unload 480, baggage load 600, catering 600, cabin cleaning 600, lavatory service 300, potable water 240, ground power 120, pushback 180, stairs 90, air start 120, maintenance inspection 300, chocks 30, deboarding 300, and pushback preparation 120. These are **simulation assumptions**, not measured KAUO times. Scenario source values are **configuration-derived**. FAA runway data and the public KAUO airport reference remain **source-backed**; service positions, GSE counts, schedules, and service durations are not.

Fuel and baggage keep their existing service locations. Other categories use the aircraft stand node unless the airport graph defines a dedicated staging node in the existing fleet routing model. These positions are operational abstractions, not surveyed aircraft service points or real aircraft geometry.

## Events, KPIs, and limits

Existing task-ready, dispatch, wait, start, complete, reassignment, failure, and turnaround-readiness event records carry aircraft, turnaround, task, service, and vehicle references where available. Existing Goal 19 turnaround analysis consumes task timings, waits, service time, turnaround completion, and departure delay. These observations identify associated wait and blocking work; they do not prove causal attribution beyond recorded task dependencies and timing.

The schema remains canonical package version 1.0. This change extends the supported equipment vocabulary and simulator service mapping without changing the on-disk major version. Goal 24A still validates equipment type values; Goal 24B rejects unknown simulator service/equipment mappings. Scenario YAML validation rejects unknown task services and invalid dependencies.

The Studio now offers mapped service-type and duration edits plus add/remove controls for mapped fleet records; generated package changes are covered by an integration test. Goal 31 is not fully integrated in this checkout yet: the Studio does not add/remove turnaround tasks, edit templates or equipment requirements directly, or show a live simulation task timeline. There is no Goal 31 KAUO multi-turnaround resource-constrained comparison package, and evidence bundling does not yet expose a dedicated GSE utilization/bottleneck report. No Goal 31 screenshots or baseline/constrained experiment results are claimed. These are known remaining gaps.

## Cesium dependency status

The exact reusable fix is the existing pinned per-checkout installer and the linked-worktree credential lookup implemented for Goal 29; copying the ignored plugin or `.local.env` into another worktree is unnecessary and unsafe. The project-local plugin is installed outside Git only when the installer has been run in that worktree. In this environment neither the plugin nor an Unreal installation is present, so Unreal visualization remains blocked here. The non-Unreal simulator integration keeps the authoritative GSE positions in simulation snapshots and does not synthesize independently animated vehicles.
