# Unreal Engine Integration Contract

## Intended shape

```text
RampLab C++ Core
      |
      +-- SimulationSnapshot
      |
      `-- SimulationEventRecord
                 |
                 v
        Unreal Adapter Module
                 |
                 v
          Mirror Unreal Actors
```

The recommended Goal 3 implementation is an Unreal plugin with two modules:

1. a plain C++/third-party module that statically compiles or links RampLab using the same MSVC runtime and Unreal build configuration;
2. an Unreal-facing adapter module that translates RampLab values into Unreal types and owns Actors/components.

A standalone DLL is not recommended for the first integration because it introduces ABI, runtime-library, deployment, and exception-boundary concerns before they are needed. A plugin-contained static integration keeps the C++ boundary explicit while allowing later replacement with a C ABI/shared library if process separation becomes valuable.

## Authority rules

- RampLab owns simulation truth and `SimTime`.
- Unreal never calls entity state transitions directly.
- Unreal Actors mirror IDs and state; they are not the domain entities.
- Rendering tick rate never determines simulation event timing.
- Input from Unreal becomes a future validated engine command, not direct mutation.

## Adapter lifecycle

1. Load or receive a validated `Scenario`.
2. Construct `Simulation` and register an adapter event sink.
3. Take an initial snapshot and create mirror Actors keyed by strong IDs.
4. Advance the engine according to the chosen simulation-control policy, independent of render frames.
5. Consume structured events for animations, VFX, alerts, and UI.
6. Consume snapshots for current state, recovery, and reconciliation.
7. Destroy mirror Actors only when authoritative state/events require it.

The first adapter may advance one event at a time on the game thread for simplicity. Faster-than-real-time or background execution requires an explicit thread-safe handoff of copied snapshots/event batches; the current engine itself is single-threaded.

## Coordinates

RampLab uses airport-local meters: `+x` east and `+y` north. The adapter defines a transform into Unreal centimeters and axes, for example:

```text
UnrealPosition = OriginCm + AxisTransform(Vec2Meters * 100.0)
```

The adapter supplies height and orientation. RampLab must not include `FVector`, `FTransform`, `UObject`, or Unreal headers.

## Vehicle visuals

For a traveling vehicle, `VehicleJourneySnapshot` provides the ordered route and timed segments. At render time Unreal finds the segment containing the current authoritative simulation time, calculates a normalized fraction, and interpolates between node coordinates. This is visual interpolation only; arrival remains the scheduled RampLab event.

## Event use

Examples:

- `VehicleAssigned` selects a target and prepares an animation.
- `VehicleDeparted` starts travel visuals.
- `ServiceStarted` triggers service animation/VFX.
- `RoadClosed` changes road material or signage.
- `AircraftReadyForPushback` updates UI and actor animation.
- `AircraftDeparted` removes or transitions the mirror Actor.

Events should not be used as the only recovery mechanism. After missed records or adapter startup, take a fresh snapshot.

## Before implementation

- Perform a real Visual Studio 2022/MSVC build with the Unreal-supported toolset.
- Decide whether C++ exceptions cross the module boundary; preferably catch/report them inside the plain C++ adapter.
- Define Unreal module ownership and shutdown order so sinks never outlive `Simulation`.
- Add a small coordinate-transform test fixture.
- Decide the simulation-control policy: manual stepping, fixed simulation-time budget, or background worker with copied output.

No Unreal project, plugin, headers, or generated files are part of this milestone.
