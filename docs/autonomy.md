# Continuous ground-vehicle autonomy

RampLab autonomy is a standalone deterministic C++ library alongside the operational discrete-event engine. `airside_autonomy` depends on the existing graph and A* router, but the operational service vehicle state machines and schedules remain unchanged. `airside_autonomy_scenario` is the YAML boundary; `airside_autonomy_experiment` executes independent fixed-step runs and retains final metrics only.

```text
Airport map + deterministic A* route
                |
                v
       Autonomy simulation
  truth -> sensors -> SensorFrame
                         |
                         v
                    controller
                         |
                         v
       bounded command -> vehicle model
```

The controller receives `SensorFrame` and `MissionState` values. It does not receive `VehicleState`, the mutable simulation, obstacle geometry, or any ground-truth accessor. Collision detection and mission metrics remain in the simulator. `AutonomySnapshot` is a read-only value intended for visualization.

## Goal 8: 2D state estimation

The core estimator is `StateEstimator2D` in `airside_autonomy`; it has no ROS dependency. At each simulation timestamp the faulted sensor frame is validated and consumed, then only the resulting estimate and health are exposed to the controller:

```text
sensor measurements
        ↓
health / timestamp validation
        ↓
EKF prediction + GNSS / IMU / wheel updates
        ↓
estimated state + covariance + health
        ↓
controller
```

The state is `[east_m, north_m, yaw_rad, forward_speed_mps]`. East/north use the existing local Cartesian map with east `+x`, north `+y`, radians counter-clockwise from east. ROS geographic conversion uses the existing KAUO WGS84 origin: 32.6151667° N, 85.4340000° W, 208.27 m ellipsoid height. The simulator already supplies GNSS in this local plane, so the core does not create a second conversion.

The motion prediction uses the last wheel-speed observation and IMU yaw rate at the simulation interval:

```text
x' = x + v cos(ψ) Δt
y' = y + v sin(ψ) Δt
ψ' = wrap(ψ + gyro_z Δt)
v' = wheel-speed scalar measurement update
```

IMU gyro is integrated using measurement timestamps; IMU heading is a scalar angle update with normalized residual. The reported wheel speed updates forward speed; the cumulative wheel distance and relative heading remain sensor observations and wheel-speed faults naturally change the dead-reckoning rate. If heading IMU data is unavailable at a new wheel sample, relative wheel heading is used as a lower-confidence yaw observation. During wheel-message loss, the last estimated speed continues the motion prediction while covariance grows. No IMU acceleration is treated as perfect speed. Startup requires a valid GNSS fix and a fresh IMU heading; until then the estimate is explicitly uninitialized and the controller commands a stop.

The covariance uses the linearized motion Jacobian `F` (`F[0,2] = -v sin(ψ) Δt`, `F[1,2] = v cos(ψ) Δt`, `F[0,3] = cos(ψ) Δt`, `F[1,3] = sin(ψ) Δt`) and `P' = F P Fᵀ + Q`. Scalar updates use the Joseph covariance form. It is symmetrized after updates and diagonal values are bounded nonnegative; this is a small fixed-size implementation, not a general-purpose matrix library. The default YAML tuning is under `estimator:` in `scenarios/autonomy_tug.yaml`: initial variances 4.0 m², 0.04 rad², 1.0 (m/s)²; per-second process terms 0.04 m²/s, 0.0025 rad²/s, 0.16 (m/s)²/s; GNSS σ 0.5 m; IMU heading σ 0.02 rad and yaw-rate σ 0.02 rad/s; wheel speed σ 0.1 m/s and relative heading σ 0.05 rad. The scenario's simulated GNSS accuracy is used when it implies more uncertainty than the configured minimum.

GNSS gating computes the two-dimensional normalized innovation squared using the predicted east/north covariance and measurement covariance. The gate is 9.21034 (99% chi-square quantile, 2 degrees of freedom); accepted position measurements use Joseph-form scalar updates. A measurement more than 0.5 simulation seconds old or future-dated is rejected and counted rather than applied at the current time. There is no history rewind; a timestamp that is not newer than the last consumed sample is ignored. Gate activation is event-recorded without per-fix event spam; counters retain accepted, rejected, gate, and stale totals.

Health reports uninitialized, healthy, degraded, unsafe, or invalid. Degraded begins if radial position uncertainty reaches 3 m, heading uncertainty reaches 0.5 rad, the last accepted absolute position fix is at least 0.25 s old, or IMU/wheel data is not yet available. Unsafe begins at 8 m radial position uncertainty, 1.2 rad heading uncertainty, or 20 s without an accepted GNSS fix. Nonfinite state/covariance or covariance magnitude above 1e12 is invalid. Degraded operation may continue; uninitialized, unsafe, and invalid states command controlled zero speed. If GNSS returns after unsafe dead reckoning but remains statistically inconsistent, the NIS gate keeps rejecting it and the tug remains stopped; the filter does not hard-reset position or resume on an unverified jump. Recovery occurs when returned measurements are consistent with the propagated estimate. These thresholds describe what the filter reports, not its actual error.

The experiment metrics use truth only in a separate evaluation path to calculate mean/RMS/maximum/final position error, heading error, uncertainty maxima, health durations, initialization time, gate counters, and uncertainty safety stops. Truth is never passed to the estimator or used by the controller. Run data is paired by seed in both estimator modes; every scenario has a fused row and a `_legacy` row. Do not interpret the covariance as calibrated confidence outside these simulated models.

## Vehicle model and time

The tug uses airport-local meters (`+x` east, `+y` north), radians, and seconds. Its fixed time step is 0.02 s. For command speed `v_cmd`, speed changes by at most configured acceleration or deceleration per step and remains in `[0, maximum_speed]`. The yaw-rate command is clamped to its configured limit. The midpoint heading is used for integration:

```text
v_next = clamp(v + clamp(v_cmd - v, -deceleration*dt, acceleration*dt), 0, max_speed)
theta_mid = theta + yaw_rate*dt/2
x_next = x + v_next*cos(theta_mid)*dt
y_next = y + v_next*sin(theta_mid)*dt
theta_next = wrap(theta + yaw_rate*dt)
```

The model is kinematic. It omits tire slip, steering geometry, grade, and suspension. It never sleeps or consults wall-clock time for simulation behavior.

## Routing and controller

The mission resolves the named Depot and Gate A2 nodes from `map_scenario`, calls the existing deterministic A* implementation, and converts route nodes to waypoints. The reference controller follows those waypoints in order with a bounded heading-rate command and reduces speed for turns and goal approach. Fused mode uses the estimate in `SensorFrame`; setting `estimator.enabled: false` retains the Goal 7 wheel dead-reckoning plus 15% GNSS blend for paired regression and comparison.

An independent safety check looks for LiDAR returns within 0.20 rad of the forward axis and inside the configured stopping envelope; it requests zero target speed and counts the transition into an emergency stop. Collision checking separately treats the tug and obstacles as circles and records the first intersecting obstacle/time. Mission success requires being within goal tolerance at low speed; otherwise the result is collision, timeout, or controller failure.

## Sensors

Sensor schedules use simulation time and retain their most recent measurement between updates. Per-sensor clocks have independent deterministic random streams for jitter and packet loss. Each delivered sample carries its sensor ID, measurement timestamp, delivery timestamp, sequence, frame ID, and explicit validity state. The scheduler does not consult wall-clock time.

| Sensor | Default rate | Values and noise |
|---|---:|---|
| GNSS | 10 Hz default | Local x/y with configurable constant bias and independent Gaussian horizontal noise; accuracy field is 1.96σ. |
| IMU | 50 Hz default | Heading, yaw rate, and longitudinal acceleration with independently configured Gaussian noise; no drift or gravity model. The fixed 20 ms simulation tick caps headless production at 50 Hz. |
| Wheel odometry | 50 Hz default | Integrated distance, speed, and heading change with configured Gaussian distance/speed noise; no wheel quantization or slip. |
| 2D LiDAR | 10 Hz | 181 beams over 180°, configurable range limits and Gaussian range noise. Headless ray/circle intersections return the nearest hit in beam order. |
| RGB camera metadata | 20 Hz | Headless timestamped 320×180 frame metadata only; Unreal SceneCapture supplies pixels independently. |

`scenarios/autonomy_tug.yaml` retains the earlier 5/50/20/10 Hz rates for Goal 1–9 behavior. Unspecified configuration rates default to the table above. Each sensor accepts `<sensor>_phase_s`, `<sensor>_latency_s`, `<sensor>_jitter_s`, `<sensor>_packet_loss_probability`, and `<sensor>_stale_after_s` under `sensors:`. Jitter must be smaller than one sample interval. Configured latency is simulation time; messages exceeding the stale threshold are marked stale and dropped before estimator/controller consumption. Each sensor pose is configured with `<sensor>_extrinsic_x_m`, `_y_m`, `_z_m`, and `_yaw_rad`; the same scenario values place Unreal components and populate ROS static TF.

Frames use east `+x`, north `+y`, up `+z`, angles in radians counter-clockwise from east, and meters. `base_link` is at the tug mesh pivot (body center). Sensor frames are `gnss`, `imu`, `wheel_odom`, `lidar`, and `camera`; headless GNSS/IMU/odometry/LiDAR samples include IDs, sequence and source frame metadata. Nominal Unreal/ROS sensor poses relative to `base_link` are LiDAR `(3.40, 0, -0.30) m` and camera `(3.40, 0, 0) m`; GNSS is `(0,0,0.20) m` and IMU/wheel odometry are centered. ROS publishes these `base_link` static transforms. The local ENU plane maps to Unreal through the existing placement adapter.

Unreal LiDAR uses CPU visibility-channel raycasts, a 181-ray horizontal sweep over 180 degrees, 0.1–30 m range, and max-range values for misses. Configured obstacle actors block the visibility channel; the tug is ignored. It can stream timestamped ranges to ROS, where `--lidar-source unreal` publishes geometry-derived scans to the same `LaserScan` topic consumed by the external safety controller. The core estimator remains portable and ROS-independent. The RGB SceneCapture uses a 90-degree horizontal FOV and a 320×180 RGBA8 render target, captured at 20 Hz simulation time. Both producers keep simulation timestamp, sequence and frame ID. Camera images can be sent over the local TCP transport to ROS as BGRA8 `Image` plus calibrated `CameraInfo`; the controller does not use images. Camera bytes are not copied into automated test buffers or core JSONL recordings. In the direct Unreal sensor-validation run, the camera's first GPU readback contained 57,600 pixels, 36,720 non-black pixels, and a stable in-run digest; LiDAR progressed from 0 to 1+ geometry hits as the tug approached the configured obstacle. Unreal reproducibility is not claimed.

The machine-readable observation recording is JSON Lines. Run the headless mission with `--record-sensors results\sensors.jsonl`; it writes delivered observations and their metadata, including compact numeric LiDAR ranges and camera frame metadata (320×180 dimensions and FOV by default), and reports a deterministic stream digest. It excludes image pixels. Repeated seeded recordings are compared byte-for-byte in the core test suite. Camera frames are metadata-only in headless runs; Unreal owns image production. There is no playback controller yet; the JSONL file supports offline comparison/replay tooling.

## Deterministic fault injection and health policy

Faults are applied to newly generated sensor values inside `AutonomySimulation`, after the sensor model and before the `SensorFrame` reaches either the built-in controller or the ROS bridge. They never modify vehicle state, obstacle geometry, collision checks, or ground truth. The normal RampLab libraries and executables remain ROS-independent.

Fault windows use simulation time and the half-open interval `[start_s, start_s + duration_s)`. A run's sensor RNG uses the simulation seed; added randomized fault noise and packet-loss decisions use the explicit fault seed (`--fault-seed`, or a deterministic derivation from the simulation seed). Serial and parallel runs own independent simulation and fault RNG state. Multiple faults execute in YAML list order. Dropout or burst loss clears pending delivery for that sensor; overlapping odometry scale faults multiply their remaining scale factors; overlapping delays use the greatest active delay. Noise and bias compose in list order.

Supported `faults` entries are:

| Sensor | `type` | Parameters |
|---|---|---|
| `gnss` | `dropout` | No valid fixes for the interval. ROS omits the fix publication. |
| `gnss` | `noise` | `magnitude` is added one-sigma Gaussian position noise in meters on each axis. |
| `gnss` | `bias` | `x_m` and `y_m` are position offsets in meters. |
| `lidar` | `dropout` | No scans are delivered for the interval. |
| `lidar` | `range_limit` | `magnitude` is the reduced maximum range in meters. |
| `lidar` | `obstruction` | `angle_min_deg` and `angle_max_deg` mask that relative scan sector to max-range values; no obstacles are invented. |
| `imu` | `dropout` | No IMU messages are delivered. |
| `imu` | `noise` | `magnitude` is added one-sigma noise in radians to heading and yaw rate. |
| `imu` | `bias` | `magnitude` radians are added to heading and yaw rate. |
| `odometry` | `scale` | `magnitude` is the fractional loss of reported distance and speed, in `[0,1]`; cumulative distance remains continuous across fault boundaries. |
| `odometry` | `drift` | `magnitude` is accumulating meters per second of distance error. |
| any sensor | `delay` | `magnitude` is a simulation-time delay in seconds. Measurements wait in a FIFO and retain their original header timestamp. |
| any sensor | `packet_loss` | `probability` in `[0,1]` is sampled once per newly generated message with the seeded fault RNG. |
| any sensor | `burst_loss` | Every message in the configured interval is dropped. |

For example:

```yaml
faults:
  - {sensor: gnss, type: dropout, start_s: 20.0, duration_s: 5.0}
  - {sensor: lidar, type: range_limit, start_s: 30.0, duration_s: 10.0, magnitude: 8.0}
  - {sensor: odometry, type: scale, start_s: 32.0, duration_s: 8.0, magnitude: 0.15}
  - {sensor: gnss, type: packet_loss, start_s: 40.0, duration_s: 12.0, probability: 0.10}
```

The YAML loader rejects unknown sensor/type names, non-finite or invalid intervals, unsupported sensor/type combinations, out-of-FOV obstruction sectors, and invalid ranges/scales. Fault transition records include schedule, activation/deactivation, first drop/delay, and unavailable/recovered transitions; message loss and delay are also retained as aggregate counters to keep event output bounded.

The controller tracks message ages against `SensorFrame.timestamp_s`. Sensor ages above 0.25 s (GNSS), 0.10 s (IMU and odometry), or 0.20 s (LiDAR) enter degraded mode. A GNSS gap up to the configured 3.0 s localization timeout can use IMU/odometry dead reckoning. GNSS, IMU, or odometry older than 3.0 s commands zero speed. LiDAR older than the configured 0.5 s perception timeout also commands zero speed. The vehicle model then decelerates under its existing configured limit. Health timeouts are scenario fields under `simulation`; ROS controller parameters with the same names use the corresponding message timestamps and `/clock`. Recovered measurements allow motion to resume.

The robustness CSV/JSON reports include configured faults, simulation and fault seeds, mission result, completion and route/clearance metrics, collision/emergency/safety counts, degraded-sensing stop time, fault transition times, final pose/speed, sensor counts, estimator errors/uncertainty/health durations/gate counters, and uncertainty stops. Every scenario runs fused and legacy modes for the same seeds; `runs.csv` and `runs.json` export direct measurements rather than a composite score. `command_timeouts` is zero for built-in runs and is measured separately by the ROS bridge watchdog.

## Headless use

Build using the normal Release configuration. Run the deterministic default mission:

```powershell
.\build-msvc\Release\airside_autonomy.exe --scenario scenarios\autonomy_tug.yaml --seed 42
```

Add `--record-trajectory results\tug.csv` to stream a diagnostic CSV with ground truth, GNSS estimate, heading, speed, command, route error, and minimum measured LiDAR range. Recording is off by default. Normal runs retain no trajectory history.

Run the independent stopping fixture with `airside_autonomy --scenario scenarios\autonomy_safety_stop.yaml --seed 42`. It deliberately blocks the initial forward envelope; the expected report is a safe timeout with at least one emergency stop and zero physical collisions.

## Experiments

The autonomy experiment executor has its own result type and bounded `std::jthread` pool; it does not place autonomy data into the operational `RunResult`. Each worker owns a scenario, controller, simulation RNG, and fault RNG. Output is sorted in stable ordinal order; sensor scans and trajectory samples are discarded. Run the checked-in robustness matrix or select one case:

```powershell
.\build-final-msvc\Release\airside_autonomy_experiment.exe --experiment experiments\autonomy_robustness.yaml --workers 4
.\build-final-msvc\Release\airside_autonomy_experiment.exe --experiment experiments\autonomy_robustness.yaml --case severe_safe_stop --workers 1 --output results\severe_fault
```

The 24-case matrix includes a no-fault baseline, 2/5/10 s GNSS gaps, two GNSS noise levels and bias, LiDAR dropout/range/masking, IMU noise/dropout, two wheel-slip scales, delayed and lost packets, and combined faults including an uncertainty-triggered safe stop. Every scenario has a same-seed fused row and `_legacy` row, for 144 runs across three seeds. Results export raw measurements to `runs.csv` and `runs.json`; there is no composite score. The older `autonomy_noise_validation.yaml` remains available for its 0.1/0.5/1.5 m GNSS-noise sweep.

The Goal 10 matrix `experiments/autonomy_sensor_goal10.yaml` runs the Unreal-validation route headlessly with baseline, GNSS-outage, and LiDAR-obstruction cases in legacy and fused modes over two seeds (12 rows). On the final Release build all 12 runs reached the fixture's 20 s timeout with zero collisions; average generated counts per run were 200 GNSS, 970 IMU, 974 wheel-odometry, and 201 LiDAR messages. The baseline run averaged 18,439 LiDAR hit returns. Serial and four-worker outputs matched byte-for-byte for `runs.csv`, `runs.json`, and `estimator_diagnostics.csv` (SHA-256: `96673DCC60392DFDC180542486CE0751836C09F4CA26B428B2EF1F21A9B9FFDB`, `361EF2F4B4B869B90AE318EA6B8B4DB4A3F49F940FD2289DE1B4D70FF958EE3A`, and `40E692DC9AB32330FA43B4A46AA7ACD4DDB1D6361D1A03509F9D9388173E1B9A`). A seeded headless run with JSONL recording produced 2,745 metadata/payload records, including 401 camera metadata records, in 0.18 s wall time for 20 s simulation (about 111× simulated time). A paired same-machine Release workload comparison used `autonomy_tug.yaml`, seed 42, one warm-up followed by 10 alternating runs per build; both commits produced identical mission results and trajectory digest. Median wall time rose from 25.86 ms at Goal 9 to 27.70 ms at Goal 10 (+7.1%; throughput ratio 0.934×), with the Goal 9 maximum (117.44 ms) showing a noisy outlier. The separately observed Unreal capture run reached about 70.8 frames/s while using a 10 Hz, 181-ray LiDAR and 20 Hz 320×180 camera. Unreal sensor CPU capture/transport latency is instrumented in its periodic runtime logs; a paired UE baseline latency comparison remains unmeasured.

## Goal 8 measured comparison

The final Release matrix ran 144/144 rows. All 63 rows corresponding to the original 21 Goal 7 cases in legacy mode completed with zero collisions. The table reports averages across seeds 42–44 and compares direct measures; the worst high-slip and combined cases below are deliberate unsafe-stop timeouts.

| Scenario | Fused outcome / time (s) | Legacy outcome / time (s) | Fused / legacy position RMSE (m) | Fused / legacy mean route error (m) | Fused / legacy minimum clearance (m) |
|---|---:|---:|---:|---:|---:|
| Clean baseline | Success / 66.21 | Success / 66.29 | 0.22 / 0.25 | 0.78 / 0.76 | 4.82 / 4.79 |
| GNSS dropout 10 s | Success / 66.25 | Success / 74.28 | 0.23 / 0.25 | 0.72 / 0.70 | 4.73 / 4.81 |
| GNSS noise 2 m | Success / 66.23 | Success / 66.38 | 0.29 / 0.52 | 0.77 / 0.74 | 4.80 / 4.73 |
| GNSS bias 8 m | Success / 66.26 | Success / 67.35 | 0.24 / 4.49 | 0.72 / 1.72 | 4.71 / 5.66 |
| Wheel slip 10% | Success / 66.31 | Success / 66.41 | 0.46 / 0.50 | 0.62 / 0.58 | 4.58 / 4.51 |
| Wheel slip 25% | Timeout / 240.00 | Success / 66.57 | 19.82 / 1.04 | 10.14 / 0.34 | 6.51 / 4.15 |
| GNSS noise + 10% slip | Success / 66.55 | Success / 66.45 | 1.24 / 0.64 | 0.20 / 0.57 | 3.89 / 4.47 |
| GNSS outage + IMU loss + 25% slip | Timeout / 240.00 | Success / 76.53 | 18.36 / 2.07 | 11.62 / 0.34 | 6.51 / 4.20 |
| Severe localization uncertainty | Success / 82.59 | Success / 89.33 | 15.82 / 11.18 | 1.05 / 0.66 | 4.79 / 4.74 |

Every row had zero collisions, including the fused timeouts. At 25% wheel slip and the combined outage/slip/IMU fault, 3/3 fused runs entered one estimator safety stop and stayed stopped through the 240 s limit; the legacy controller completed. In the severe all-localization-sensor outage, both modes stopped and recovered after sensors returned, but fused maximum actual position error averaged 67.20 m versus 22.61 m legacy even though final errors were 0.26 m versus 0.09 m. This exposes a real limitation: covariance does not capture persistent wheel scale bias, and the NIS gate can keep rejecting GNSS after the estimate diverges. The controller favors a no-collision stop over an unverified pose jump.

## Goal 7 validation snapshot

On the 2026-09-29 Windows Release build, seed 42 no-fault autonomy completed in 66.36 s, traveled 309.76 m, had 0.734 m mean and 3.243 m maximum route error, 4.687 m minimum clearance, 0 collisions, and trajectory digest `1488735950019943363`. Those values match the pre-fault baseline. In the paired 3-seed matrix, all 63 runs completed without collision. The 2 s GNSS dropout preserved the seed-42 completion time; the 5 s and 10 s gaps triggered health stops and completed in 68.06 s and 74.24 s for seed 42. An 8 m GNSS bias completed in 67.38 s with a 6.480 m maximum route error. The severe 12 s LiDAR/GNSS/IMU dropout entered one controlled stop, remained stopped for 11.58 s due to degraded sensing, resumed after recovery, and completed in 78.78 s with 0 collisions. These are specific synthetic scenario measurements, not hardware performance claims.

## Goal 9 wheel health and GNSS reacquisition

The fused estimator compares accumulated wheel distance with GNSS displacement in straight-travel windows of at least 4 s and 2 m. Windows spanning a GNSS gap over 5 s or heading change over 0.65 rad are skipped. Ratios outside 0.76–1.28 add inconsistency evidence: the first window marks wheel health `Suspect`, two consecutive bad windows mark it `Degraded`, and three consistent good windows restore `Nominal`. Suspect wheel speed updates are downweighted 9× and degraded updates 10,000×. A degraded wheel channel, or a suspect channel with at least three recorded inconsistencies, reports estimator health `Unsafe` so the controller chooses a safe stop. The check requires vehicle motion; it cannot independently calibrate wheel scale while stationary.

GNSS fixes more than 2 m from the estimate or beyond the NIS gate are rejected and buffered. Eight mutually consistent fixes (maximum 3 m spread and no pairwise jump over 5 m) start reacquisition. Position correction is limited to 0.75 m per fix; yaw and speed are not reset. Health stays unsafe until the estimate is within 1 m of the candidate center after at least three correction steps. When GNSS has been unavailable for 6 s and odometry is also unavailable or degraded, the estimator predicts a controlled stop using the configured 1.5 m/s² deceleration. This is a simulator model parameter, not a vehicle braking guarantee.

The 2026-09-29 Release matrix has 35 cases × 3 seeds × fused/legacy = 210 rows. Serial and four-worker runs produced identical `runs.csv`, `runs.json`, and `estimator_diagnostics.csv` SHA-256 hashes. There were zero collisions: 183 missions succeeded and 27 ended in safe `TIMEOUT` (including deliberate stop/uncertainty cases). The severe GNSS/IMU/odometry outage now stopped and recovered with fused position RMSE 0.48–0.63 m and maximum actual position error 0.95–1.26 m across seeds, compared with legacy RMSE 11.17–11.18 m. The combined GNSS outage, IMU loss, and 25% wheel scale fault reacquired GNSS once successfully in each seed, completed in 76.74–77.04 s, and had 9.23–9.59 m maximum actual error with zero collisions. The 25% scale-only sweep selected safe timeouts in all three fused seeds: scale error was not considered verified after it ended because the vehicle was no longer moving to provide another wheel/GNSS comparison. Detailed wheel and recovery counters are written to `estimator_diagnostics.csv` beside the run results.

The checked-in matrix includes 0–30% wheel scale sweeps, slip with noisy GNSS, slip with GNSS or IMU loss, combined outage/reacquisition, and severe uncertainty-stop cases. The current diagnostics record final wheel/recovery state, inconsistency/transition/downweight counts, reacquisition attempts/successes/candidate rejections, degraded time, maximum position uncertainty and GNSS NIS, safety-stop time, recovery latency, and GNSS accepted/rejected/gate counts.

## Unreal and ROS2 boundaries

Unreal remains a non-authoritative consumer. It advances the same fixed-step controller/sensor model and renders its read-only snapshots; it does not move the vehicle along a hand-authored spline or feed renderer state back to the controller. Airport placement remains governed by the existing KAUO transform.

The core and CLI have no ROS2 dependency. The optional native Windows ROS 2 Lyrical bridge and separately running external controller are implemented and validated; see [ros2.md](ros2.md) for the exact Pixi activation sequence, topics, frames, timing, mission results, and command-timeout behavior.

### Goal 10 Unreal sensor runtime validation

Run `RampLabViewerEditor Win64 Development` with the checked-in Unreal project, then launch the standalone game with `-RampLabSensorValidation -log`. This selects `autonomy_sensor_validation` at 1× playback so the render tick samples the obstacle instead of advancing past it. The run uses the production Unreal LiDAR/camera components and the same authoritative core simulation. Sensor TCP connections are nonblocking: connection completion is polled on later captures, and an unavailable receiver cannot stall the game thread. Packet transmission starts only after connection succeeds; connection failures are retried after 0.5 simulation seconds.

On the installed UE 5.8.3 / MSVC 14.44 toolchain, the rebuilt standalone run completed its intended 20 s observation horizon as `TIMEOUT`, traveled 87.21 m, and recorded zero collisions. The Unreal game thread remained responsive at approximately 39.1 frames/s with 44 actors while Cesium streamed asynchronously. LiDAR produced 187 captures (9.35 Hz effective), 181 rays per scan and 546 geometry hits, at 0.3227 ms mean capture cost. The 320×180 camera produced 311 captures (15.55 Hz effective), at 0.3667 ms mean capture cost in the no-receiver run. These measured rates are below their configured 10 Hz and 20 Hz because the Unreal runtime schedules captures on render/game ticks. Earlier scene readback measured 57,600 pixels with 36,720 nonblack pixels and a stable nonzero digest, confirming rendered geometry in the camera observation. A live bridge run also logged LiDAR returns rising to 15 as the vehicle approached the obstacle.

With the external ROS 2 controller and bridge listening, both Unreal TCP streams connected and the bridge consumed 156 LiDAR scans, 972 IMU samples, 974 odometry samples, 197 GNSS fixes, and 295 camera frames in one 20 s run. The controller issued commands through the run; it logged no LiDAR stops. That bridge mission also reached the deliberately bounded 20 s `TIMEOUT` with 87.21 m traveled, 6.471 m minimum obstacle clearance, zero collisions, zero emergency stops, and zero command timeouts. These results validate local process-to-process transport and the bounded synthetic scenario; the 20 s timeout is the fixture horizon, not a successful destination mission. A separate live ROS topic probe was not captured during that run, so the message totals are the bridge's own publication/consumption counters rather than an independent topic-rate measurement. Without a receiver, nonblocking mode keeps capture responsive but naturally delivers no packets.

## Goal 11 multi-vehicle fleet layer

`FleetSimulation` runs one `AutonomySimulation` and `ReferenceController` per typed `VehicleId`, but advances all active members on one fixed simulation clock. Each member gets its own route, estimator, sensor clocks/RNG, fault schedule, and health state. Geometric traffic-conflict prediction consumes estimator position/velocity. Opposing-edge reservations additionally use simulated ground-truth occupancy to confirm that a vehicle physically cleared the reserved edge; the vehicle controller does not receive this coordinator signal. Ground truth also feeds post-step collision and minimum-separation measurement. `load_fleet_scenario` loads the shared vehicle scenario and scenario-defined missions/faults. The `airside_fleet` executable reports aggregate and individual mission metrics and typed traffic events.

The coordinator predicts constant-velocity closest approach over four simulated seconds and adds a 35 m approach buffer (45 m release hysteresis). A conflict uses a shared route edge resource when missions overlap on an edge, a shared intersection-node resource when they converge at a graph node, and a vehicle-pair fallback for geometric conflicts elsewhere. Per-resource contenders are sorted by simulation timestamp, lower integer mission priority, then vehicle ID. For routes that traverse one shared edge in opposite directions, the coordinator books that edge before movement; the loser waits in a holding bay and the reservation remains until the owner clears the segment and the geometric conflict has ended, or the owner has completed beyond the edge. A retreating member leaves normal resource requesting while it reverses, while pair-conflict checks continue to stop nearby traffic; the core releases its reservations only after vehicle geometry clears the edge or intersection footprint. Request, grant, defer, wait, release, and collision decisions are timestamped events. The coordinator is single-threaded; worker execution only parallelizes independent whole fleet runs.

The checked-in `autonomy_fleet.yaml` routes three vehicles through shared Gate A2 approaches to distinct destinations. On the Windows Release build, seed 42 ran 3,313 fixed-step ticks to 66.28 simulated seconds in 0.035 s wall time: all 3 missions completed, 0 timeouts, 0 collisions, 28.608 m minimum separation, 821.015 m total distance, 8.66 cumulative traffic-wait seconds, 2 pair-reservation requests, and 1 contention. Tug 3 waited from 12.38 s to 21.04 s, then continued and completed. The two-resource reservation table queues competing requests and transfers ownership on release; focused tests cover priority/ID ordering and exclusive release.

The `autonomy_fleet_fault.yaml` experiment injects a 14 s GNSS dropout into `tug_02` while it contends with the other two vehicles. Seed 42 completed all 3 missions in 74.64 simulated seconds with 0 collisions and 23.906 m minimum separation; `tug_02` recorded one degraded-mode entry and safely completed. The run recorded two contentions and 24.88 cumulative waiting seconds.

`autonomy_fleet_independent.yaml` validates three spatially separated routes: seed 42 completed all 3 missions in 32.36 simulated seconds with zero reservations, zero waiting, and zero collisions (82.467 m minimum separation). `autonomy_fleet_narrow_segment.yaml` uses a constrained graph where traffic from the depot and Gate A3 merges onto the same North-South segment; seed 42 deferred `tug_02` to `tug_01` at 25.12 s, released the edge at 37.50 s, and completed all 3 missions in 119.12 simulated seconds with one contention, 12.38 waiting seconds, one near-conflict event, one traffic-forced safety stop, zero collisions, and 15.228 m minimum separation. `autonomy_fleet_opposing.yaml` sends `tug_01` and `tug_02` through a 120 m edge in opposite directions from off-road holding bays, alongside an independent third mission. For seed 42, `tug_02` was deferred to `tug_01` at time 0, waited 36.92 s, and received the edge after `tug_01` cleared it. All 3 missions completed in 73.86 simulated seconds with zero timeouts, zero collisions, and 18.013 m minimum separation. Seeds 42–51 all completed 3/3 with zero collisions and minimum separation between 17.669 and 18.013 m. Fleet results count newly predicted vehicle-pair conflicts and traffic-forced safety stops, and emit `near_conflict`, `entered_conflict`, and `forced_safety_stop` events.

Fleet tests cover three-member stepping, unique IDs, scenario faults, route-resource contention, collision/separation checks, and repeat-seed event ordering. A 24-run seeded faulted-fleet matrix (seeds 42–65) matched exactly between serial execution and four workers for the complete `FleetMetrics` value, including per-vehicle results and typed events; all 24 runs completed three missions with zero collisions. A separate 10-seed opposing-edge matrix (seeds 42–51) matched complete results between serial and four-worker execution, with 3/3 missions and zero collisions for every run. The deterministic digest includes aggregate metrics, per-vehicle trajectory/sensor digests and final states, and full event timestamps/participants/resources. The ROS bridge accepts `--vehicle-id` and places every bridge topic under `/ramplab/<id>`; non-default vehicle TF frame IDs also carry the vehicle ID, and camera/LiDAR TCP ports must be unique per process. The bridge package suite reports 16 tests, zero failures. The generalized `verify_multi_vehicle_isolation.py` probe launched three bridge processes together: 1.5, 0, and 0.75 m/s commands produced 15.001, 0.000, and 8.485 m displacement over 12 simulated seconds; all three produced odometry and LiDAR, each command topic had one subscriber, one bridge published global `/clock` (`fleet_beta` used `--no-global-clock`), and the three TF chains were distinct. The probe sets Fast DDS to UDPv4 because the local Fast DDS 3.6.2 shared-memory path crashes when starting multiple participants. These are independent bridge simulations, not one ROS-controlled fleet simulation.

The Unreal Editor target was clean-built with UE 5.8.3 and MSVC 14.44 after rebuilding the linked C++ core. `-RampLabFleetValidation` selects the three-vehicle scenario and exits after its runtime horizon. The verified run exited with code 0 after 3 vehicles completed 3/3 missions, 0 timeouts, 8.66 s waiting, 1 contention, 0 collisions, and 28.608 m minimum separation. The viewer renders stable per-ID mesh actors with goal and moving/waiting/stopped/completed labels. The debug panel reports live fleet counts, per-vehicle state, waiting time, reservations, contention, near-conflicts, forced stops, deadlocks, collision count, and minimum separation; Recent Events displays typed traffic decisions. This is local simulator runtime evidence, not physical vehicle validation.

The coordinator builds a deterministic wait-for graph on each step and exposes current dependencies with blocker, resource, and simulation-time wait duration. Cycle detection is separate from persistence: only a cycle that remains for the configured `deadlock_persistence_s` is counted as a deadlock. Recovery selection sorts by higher mission priority, greater accumulated wait, then ascending vehicle ID. The three candidates in `autonomy_fleet_deadlock.yaml` have equal priority; `tug_01` yielded because it had waited longest when the cycle was detected (the ID rule resolves exact ties).

Retreat reverses the selected vehicle along its recorded motion samples to the entry of the route segment it had entered. The reverse command keeps the vehicle's road-facing heading, uses a signed longitudinal speed capped at 35% of the configured maximum (at most 1 m/s), and uses the ordinary acceleration, braking, yaw, obstacle, collision, and separation checks. It stops at the previous route waypoint and holds there until the contested reservation owner has geometrically cleared the resource, the conflict has ended, and the yielding vehicle has stopped. Then the core replans from its current position to the original mission goal. A member gets at most two retreat attempts; a retreat also fails after 60 simulated seconds or 10 seconds without progress. If the history has no safe retreat path, the simulation records `retreat_failed` and continues under its safe-timeout behavior.

The recovery stream includes selection, start, progress, completion, resource release, mission resumption, and failure events. Retreat start/progress/completion records include position, target, and distance. Snapshots expose `normal`, `waiting`, `selected`, `retreating`, `holding`, `resumed`, and `failed` states. ROS fleet state carries the recovery state, resource, target, progress, and per-member attempt count. The Unreal fleet panel shows the same recovery fields and the Recent Events list renders the retreat lifecycle.

Seed 42 formed the three-vehicle cycle at 5.68 s, detected it at 5.78 s, and selected `tug_01`. It reversed along its recorded Holding A approach by 19.64 m of path distance (14.01 m start-to-completion displacement), released the contested resource after clearance, resumed its route, and completed the original mission. The run completed 3/3 missions in 53.38 simulated seconds with zero safe timeouts, zero collisions, 8.362 m minimum separation, one deadlock detected/resolved, one recovery attempt, one retreat, one post-retreat replan, no current wait dependencies, and zero outstanding reservations. Four identical runs matched exactly between serial and four-worker execution. A slow-start unrecoverable-cycle test records one failed recovery attempt and exits through controlled timeouts without retrying indefinitely. The final C++ Release test suite passed 140/140 tests. These are simulator results, not real-world vehicle performance claims.

Fleet scenarios accept deterministic `road_events` with `time_seconds`, numeric `edge_id`, and `available`. A closure updates every vehicle's private graph before that step's movement, reroutes active routes that use the edge, and holds a vehicle stopped when no alternate is available. Reopening triggers a retry for vehicles held by that closure. The `autonomy_fleet_dynamic_closure.yaml` fixture closes edge 1 at 1 s and reopens it at 20 s while two missions are active: seed 42 completed 2/2 missions in 82.62 simulated seconds after one closure replan; the unaffected mission completed at 28.32 s. There were zero timeouts or collisions, 4.084 m minimum separation, and 511.007 m total route distance. Serial and four-worker replay tests compare complete metrics for eight identical seeded runs. Unreal's runtime validation executed this same scenario with 2/2 completed, zero collisions, and the same minimum separation and digest.

The `autonomy_fleet_congested.yaml` stress fixture runs eight vehicles simultaneously through four independent opposing-road corridors. Seed 42 completed 8/8 missions in 93.58 simulated seconds with zero timeouts and collisions, 19.888 m minimum separation, 273.32 s cumulative traffic wait, 8 reservation requests, 4 contentions, and 4 near-conflict events. Eight identical seeded runs matched exactly between serial and four-worker execution, including event histories and final vehicle results. This exercises larger-fleet contention but does not force a cyclic wait-for deadlock.

Resource acquisition remains FIFO by simulation-time request timestamp, with mission priority and vehicle ID breaking same-time ties. This bounds starvation for persistent waiters but allows an older low-priority request to precede a later higher-priority request; `starvation_preventions` counts releases where FIFO protects that older waiter from a later higher-priority request. The fleet CSV/JSON exports include recovery, reroute, closure-replan, wait, and fairness measurements. Seed 42 export validation parsed both formats: two JSON runs had digest `5579921075300406608`, the JSON retained all five key retreat lifecycle events, and the 39-column CSV reported one retreat and zero outstanding reservations.

The optional `ramplab_ros2_fleet_bridge` publishes identity-keyed `/ramplab/fleet/state` and `/ramplab/fleet/traffic_events` streams from the deterministic fleet core, alongside the existing per-vehicle command/sensor bridges. The Goal 12 bridge suite passed 16/16 tests. The final retreat probe received 1,417 state snapshots over 35 wall seconds while an Unreal runtime ran concurrently, and observed `tug_01`'s selected, started, completed, resource-released, and resumed events; the event coordinates show 14.01 m of physical retreat displacement. The multi-process isolation probe passed with three independent namespaces, 224/234/241 odometry samples and 117/120/121 LiDAR scans; commands produced 14.918/0.009/8.621 m displacement, each command topic had one subscriber, one bridge published `/clock`, and the three TF chains were distinct. These are independent bridge simulations, not one ROS-controlled fleet simulation.

The Unreal core libraries and `RampLabViewerEditor` target built successfully with MSVC 14.44. A visible standalone `-game -windowed -RampLabGoal12RecoveryValidation` run loaded the seeded scenario and logged 3/3 completed, 0 timeouts, 0 collisions, 8.362 m minimum separation, one deadlock resolved, one retreat, zero outstanding reservations, and the same deterministic digest as the C++ run. At completion the runtime's Recent Events panel data included selection, start with the Holding A target, completion with 19.6 m path progress, resource release, and mission resumption. The panel shows each vehicle's recovery state/resource/target/progress/attempts and preserves these lifecycle entries as ordinary reservations continue. The visible closure run also completed 2/2 missions with zero timeouts or collisions and 4.084 m minimum separation. These results validate the Unreal integration and UI data path; no physical vehicle or real-airport performance was tested.

## Goal 13 deterministic fleet dispatch

`FleetDispatcher` now runs inside `FleetSimulation`, so changing service demand uses the same fixed-step vehicle movement, graph routing, route reservations, collision checks, road closures, and recovery manager as assigned-mission fleets. Fleet YAML describes dispatch vehicles and their capability lists plus a request queue. Requests have typed IDs and task kind, required capability, origin/destination, simulation-time release, priority, optional deadline, service duration, and optional sensor faults. The lifecycle is queued, assigned, en-route, servicing, completed, or failed/timed out. Vehicles report current request, capability, idle/busy/recovering/unavailable state, task count, distance, busy/idle time, and utilization.

Each fixed simulation step releases due requests and dispatches compatible work. The dispatcher rejects busy, unavailable, degraded, and recovering candidates; filters capability before cost; checks current graph route availability; and minimizes A* travel distance from the vehicle's current road node to the request origin. Requests rank by `base_priority + floor((simulation_time - release_time) / aging_interval)`, then earlier release time and ascending request ID. Exact route-cost ties use ascending stable vehicle ID. Candidate-evaluation events include capability/route rejection or route distance; assignment events include the effective priority and the reason `minimum_available_route_cost;stable_vehicle_id_tie_break`. Decisions do not use wall-clock time or unordered iteration order.

A vehicle fault or safe mission timeout marks it unavailable, relinquishes its task, and requeues the unfinished request with no progress transfer. The replacement starts from its own physical state and reruns the task route through normal safety checks. If every attempt fails or no compatible route becomes available, the bounded dispatch horizon marks remaining work failed; it never counts as completed. Reservations stay held while a vehicle physically occupies a resource and are released after geometric clearance. Dynamic completion is reported only when all requests are terminal and no vehicle is still active.

The scenarios are:

- `autonomy_dispatch_dynamic.yaml`: 3 vehicles and 4 requests released at 0, 400, 800, and 1,200 simulation seconds. Seed 42 completed 4/4 requests in 1,347.30 simulated seconds, with 0 failures, 0 collisions, 78.202 m minimum separation, 1,148.498 m distance, 0 reassignments, 0 wait cycles, and 0 outstanding reservations. The fuel request produced incompatible-candidate events for the baggage/tug vehicles; only `fuel_01` was assigned.
- `autonomy_dispatch_fairness.yaml`: 7 queued/runtime-arriving tasks on one tug. Seed 42 assigned `high_00` first. Eight aging activations raised `low_waiting` to effective priority 3; it was assigned at 16.08 s before the later `high_08` and `high_10` requests at 20.10 and 24.12 s. All 7 completed, with 0 collisions and 0 reservations left. Average/max queue wait was 7.766/16.08 s.
- `autonomy_dispatch_reassignment.yaml`: a prolonged GNSS dropout safely timed out `baggage_faulted` at 240 s. `baggage_recovery` was unassigned, requeued, and assigned to `baggage_backup`, which completed it at 268.38 s. Seed 42 completed 1/1 requests with exactly 1 reassignment, 0 collisions, 82.517 m minimum separation, and 0 outstanding reservations.

Fleet JSON exports now include dispatch metrics, request snapshots and assignment events, vehicle tasks/utilization, and simulation horizon. CSV has a fixed 63-column schema with fleet, vehicle, and request record types. Seed 42 JSON parsed for all three scenarios; CSV parsed with 63 fields on every row and fleet counts agreeing with request rows. Repeating the dynamic scenario produced digest `10855768608942898120` twice. Four identical dynamic runs compared exactly between serial and four-worker `execute_fleet_runs`; the full `FleetMetrics` values and digests matched, demonstrating per-run dispatcher isolation. The complete Release C++ suite passed 152/152 tests, including assignment, capability, distance, tie, aging, runtime release, failure/reassignment, closed-road, and experiment-scheduling coverage. Representative Goal 11/12 fleet tests remained green in the same suite.

The ROS fleet adapter publishes read-only request lifecycle snapshots and `dispatch_events` alongside existing identity-keyed fleet state. The ROS bridge package suite passed 16/16 tests. The live dispatch probe received 737 fleet snapshots and 17 dispatch events, observed 4/4 requests complete with 4 assignments and zero unfinished work, and saw capability rejection events. The existing three-process namespace/isolation probe also passed: each command topic had one subscriber, `/clock` had one publisher, and all three TF chains remained namespaced.

The Unreal `RampLabViewerEditor` target built successfully with UE 5.8.3/MSVC 14.44. A visible `-game -windowed -RampLabGoal13DispatchValidation` run completed 4/4 requests with the same C++ digest `10855768608942898120`, zero failures/collisions/reservations, and logged request release, assignment, completion, and the dispatch summary from the live Recent Events/UI path. The operations panel shows active vehicle-to-task assignments, queued request state/priority/wait, completed/total work, assignment/reassignment counts, aging activations, and queue wait metrics.

After the reassignment scenario was added, the editor target rebuilt and a windowed `-RampLabGoal13ReassignmentValidation` run completed 1/1 requests after exactly one reassignment, with zero collisions and reservations. The Recent Events panel recorded `baggage_recovery` being reassigned to `baggage_backup` and then completed at 268.38 s.

The model remains a synthetic software simulation. Service duration is a deterministic stopped-vehicle interval; it does not model payload physics, staff, aircraft turnaround resources, actual airport operating policies, or validate real airport efficiency or autonomous-vehicle performance. Assignment cost is route distance to the request origin; it does not predict time-dependent congestion beyond the current graph's road availability. Physical vehicle, real ROS controller, and airport acceptance remain outside these results.

## Reproduction

```powershell
cmake --build build-final-msvc --config Release
ctest --test-dir build-final-msvc -C Release --output-on-failure
.\build-final-msvc\Release\airside_autonomy.exe --scenario scenarios\autonomy_tug.yaml --seed 42
.\build-final-msvc\Release\airside_fleet.exe --scenario scenarios\autonomy_fleet.yaml --seed 42 --csv results\fleet.csv --json results\fleet.json
.\build-final-msvc\Release\airside_fleet.exe --scenario scenarios\autonomy_fleet_fault.yaml --seed 42 --json results\fleet_fault.json
.\build-final-msvc\Release\airside_fleet.exe --scenario scenarios\autonomy_fleet_independent.yaml --seed 42
.\build-final-msvc\Release\airside_fleet.exe --scenario scenarios\autonomy_fleet_narrow_segment.yaml --seed 42
.\build-final-msvc\Release\airside_fleet.exe --scenario scenarios\autonomy_fleet_opposing.yaml --seed 42
.\build-final-msvc\Release\airside_fleet.exe --scenario scenarios\autonomy_fleet_deadlock.yaml --seed 42 --json results\goal12_deadlock.json --csv results\goal12_deadlock.csv
.\build-final-msvc\Release\airside_fleet.exe --scenario scenarios\autonomy_fleet_dynamic_closure.yaml --seed 42
.\build-final-msvc\Release\airside_fleet.exe --scenario scenarios\autonomy_fleet_congested.yaml --seed 42
.\build-final-msvc\Release\airside_autonomy_experiment.exe --experiment experiments\autonomy_robustness.yaml --workers 4
.\build-final-msvc\Release\airside_autonomy_experiment.exe --experiment experiments\autonomy_robustness.yaml --case severe_estimator_uncertainty_stop --workers 1 --output results\goal8_severe
.\build-final-msvc\Release\airside_experiment.exe --experiment experiments\small_validation.yaml --workers 4 --output results\goal8_operational_regression --quiet
Set-Location C:\dev\ros2_lyrical
pixi run powershell -NoProfile -ExecutionPolicy Bypass -Command "Set-Location 'C:\Users\Ferna\OneDrive\Documents\ChatGPT\RampLab'; . 'C:\dev\ros2_lyrical\local_setup.ps1'; . '.\ros2_ws\scripts\build.ps1'"
pixi run powershell -NoProfile -ExecutionPolicy Bypass -Command "Set-Location 'C:\Users\Ferna\OneDrive\Documents\ChatGPT\RampLab'; . 'C:\dev\ros2_lyrical\local_setup.ps1'; . '.\ros2_ws\install\local_setup.ps1'; python .\ros2_ws\scripts\verify_fault_topics.py --scenario scenarios\autonomy_tug.yaml --seed 42 --fault-seed 7019 --factor 2 --max-sim-seconds 80 --clean-mission"
python .\ros2_ws\scripts\verify_fault_topics.py --scenario scenarios\autonomy_ros2_fault_validation.yaml --seed 42 --fault-seed 7019 --factor 1
powershell -ExecutionPolicy Bypass -File .\unreal\RampLabViewer\Scripts\BuildRampLabCore.ps1
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat' RampLabViewerEditor Win64 Development "-Project=$PWD\unreal\RampLabViewer\RampLabViewer.uproject" -WaitMutex -NoHotReload
```

The Unreal Editor target command and selected toolchain are in [unreal-integration.md](unreal-integration.md); standalone ROS topics and live validation are in [ros2.md](ros2.md).

## Known limits

- The mission uses a small synthetic road graph and circle obstacles, not surveyed service-road geometry.
- The route follower is a simple deterministic geometric controller; very large GNSS noise can produce a timeout.
- The safety rule stops on close forward returns; it does not perform obstacle avoidance or replan around dynamic obstacles.
- Sensor and fault models are illustrative abstractions; they are not hardware-calibrated or certified distributions.
- This is a small 2D EKF, not SLAM or a production-certified localization stack; simulated sensor models are not hardware calibrated.
- Fault scheduling is deterministic in simulation time, while ROS 2/DDS reception and Windows process scheduling remain wall-clock nondeterministic.
- Unreal visualization displays snapshots from the live deterministic autonomy simulation; Cesium remains subject to its existing local credential and network requirements.
