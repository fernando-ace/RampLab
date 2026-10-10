# Goal 29: KAUO visual digital twin

## Scope and claim

Goal 29 turns the existing generated KAUO operational overlay into a geospatially grounded Unreal presentation. It keeps the deterministic RampLab core and Goal 28 live session authoritative. The airport remains an **approximate KAUO operational digital twin**, not a surveyed or validated airport digital twin. Only the airport reference point, runway endpoints, runway identifiers, and published runway dimensions have authoritative airport-source backing in the current geometry package.

## Cesium integration

The viewer uses the project-local Cesium for Unreal 2.29.1 plugin, Cesium World Terrain ion asset 1, and Bing Maps Aerial ion asset 2. The runtime creates one `ACesiumGeoreference`, explicitly assigns it to the terrain tileset, then attaches the aerial raster overlay to that tileset. Cesium credits remain enabled. Terrain is reported connected only after Cesium raises its tileset-loaded event; imagery is reported configured and is visually verified separately because a loaded terrain tileset alone does not prove that imagery rendered.

The ion credential key is `RAMPLAB_CESIUM_ION_TOKEN`. Unreal resolves it in this order:

1. Current repository root `.local.env`.
2. Primary checkout root `.local.env`, found through Git's linked-worktree `gitdir` and `commondir` metadata.
3. The inherited process environment.
4. Current project `.env.local` or `Config/CesiumIon.local.ini`.
5. Primary checkout project `.env.local` or `Config/CesiumIon.local.ini`.

The Goal 29 worktree does not copy the primary checkout credential. In the inspected checkout, the root `.local.env` was absent; the existing ignored secret was in the primary checkout's `unreal/RampLabViewer/.env.local`. The worktree-safe resolver finds that file directly through Git metadata. The documented launcher validates read access to ion assets 1 and 2 without printing the credential, then builds and starts the viewer. Runtime status logs contain the credential source and connection state only, never its value.

Validate token discovery and asset permissions from the repository root with:

```powershell
powershell -ExecutionPolicy Bypass -File .\unreal\RampLabViewer\Scripts\ValidateCesiumAccess.ps1
```

Launch the standalone daylight viewer from any working directory with:

```powershell
powershell -ExecutionPolicy Bypass -File .\unreal\RampLabViewer\Scripts\LaunchRampLabViewer.ps1
```

The launcher honors `UE_EDITOR` if Unreal Engine is installed elsewhere. Optional Unreal arguments can be supplied with `-UnrealArguments`; `-SkipBuild` skips the editor target build after the core build.

## Georeference and coordinate chain

The scene continues to use the established coordinate chain: WGS 84 at the FAA KAUO airport reference point, airport-local ENU metres in simulation, local east/north mapped through the configured 90 degree local-X bearing, and Unreal centimetres. Cesium receives longitude, latitude, and approximate ellipsoid height in that order. The current configured origin is latitude `32.61511111`, longitude `-85.4340000`, height `208.22 m`; local offsets are zero and scale is one. Runway endpoints remain the FAA NASR source of geometry and no hidden coordinate correction is introduced.

## Visual layers

- Runway pavement uses source-derived runway lengths, widths, centers, and bearings. The viewer adds dashed centerlines, edge lines, and simplified threshold bars. These markings are presentation approximations; no surveyed marking plan is represented.
- Taxi routing follows the existing approximate airport graph. A narrow yellow center indication makes routing readable without drawing fabricated pavement edges; graph paths are not claimed as surveyed taxiway geometry.
- The apron bounds remain generated operational geometry. Their approximate chart-graticule outline is drawn over the basemap rather than filled, so the source imagery remains visible. Existing approximate terminal/FBO and hangar footprints have differentiated walls, roof masses, and a simple glazed facade to read more clearly at demonstration distance.
- Aircraft and service vehicles remain primitive-based, asset-safe representations positioned and rotated from the synchronized simulation snapshot. Status colors and labels are visualization only and do not affect simulation state.
- The daylight presentation uses the existing Unreal sky/atmosphere, directional sun, and sky-light fill. Cesium supplies terrain and aerial context around the airport.

The public/generated geometry package classifies its building locations and sizes as approximate. The runway identifiers and nominal dimensions come from source records; threshold stripe count, marking form, taxiway surface, apron stands, FBO facade, and hangar massing are not authoritative. Aerial context is provider data, not project-owned airport survey data.

## Cameras and synchronized operation

The control panel offers Overview, Apron, Runway, Follow Aircraft, and Follow Vehicle. Follow cameras update their focus from actual active aircraft surface positions and observed vehicle positions in the current simulation snapshot, including Goal 28's live file mirror. Keyboard camera movement remains W/A/S/D, Q/E, and mouse-wheel zoom. The live mirror remains at 10 Hz and Unreal remains a presentation consumer; no control or safety semantics were changed.

## Validation and final evidence

Validation completed on the Goal 29 worktree based on Goal 28 commit `5ec0d560337fae20fb0d704a7ff9d07368a74dca`:

- Full Release CMake build with tests enabled; CTest passed **186/186**.
- Requested airport ingestion, scenario generation/geospatial, KAUO Goals 25–27, run artifacts, experiment analysis, operations dashboard, and evidence-bundle Python suites passed **88 tests** (5 skipped by their normal environment guards). The real-bundle integration was then invoked directly against the built Release CLI and passed.
- `BuildRampLabCore.ps1` and the Unreal 5.8 `RampLabViewerEditor Win64 Development` target built successfully after the final presentation change.
- The credential validator found the existing primary-checkout local credential without copying it and received HTTP 200 for Cesium ion asset metadata requests for both terrain asset 1 and imagery asset 2. Runtime logs confirm the explicit KAUO georeference and terrain `OnTilesetLoaded` event. The final captures visibly show Cesium aerial imagery under the generated airport layer.
- The Goal 28 live-session runner produced deterministic control/repeat exports, analysis, and its synchronized interactive session. Unreal consumed the actual generated KAUO scenario and live state; its log recorded `sim_time=15 s`, `session=paused`, `dashboard stream 10 Hz`, **2 synchronized aircraft**, and **3 synchronized vehicles**. Later capture sessions read the same saved live state after the complete runner had advanced it.
- Runtime telemetry sampled roughly **30–47 FPS** across the live demonstration and final camera captures. No frame over 500 ms was recorded in those samples; the logged maximum frame delta was 0.4 s. Cesium terrain was connected and the imagery overlay configured in each capture session.
- Final 1280×800 captures, generated by the final Unreal implementation:
  - Overview/live Goal 28 scene: `results/goal29/goal28-live/interactive-runtime/run-001/KAUOOverview-final-approved.png`
  - Apron: `results/goal29/goal28-live/interactive-runtime/run-001/KAUOApron-final-approved.png`
  - Runway: `results/goal29/goal28-live/interactive-runtime/run-001/KAUORunways-final-approved.png`

Visual review confirms terrain imagery and runway geometry are in the expected airport frame, but this work does not claim survey-grade registration. The imagery and operational runway layer show a visible residual on the order of tens of metres in the overview capture. The approximate apron/building footprints and taxi graph have visibly greater local uncertainty and can diverge from the imagery; the graph is intentionally shown as route-center guidance without invented pavement edges. The aircraft and vehicles remain small primitive representations. No streaming stall was observed during the captured sessions. Cesium credits are enabled by the runtime overlay.

## Remaining limitations

The generated geometry is not a survey, no precise airport building footprints or surveyed taxiway centerlines are claimed, and primitive aircraft/vehicle models are intentionally lightweight. Cesium streaming depends on internet connectivity and token permissions. Any geometry-to-imagery residual is to be reported from the final captures without adjusting the airport frame to hide it.
