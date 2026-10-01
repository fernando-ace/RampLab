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

`FleetSimulation` runs one `AutonomySimulation` and `ReferenceController` per typed `VehicleId`, but advances all active members on one fixed simulation clock. Each member gets its own route, estimator, sensor clocks/RNG, fault schedule, and health state. Fleet traffic logic consumes estimator position/velocity; ground truth is reserved for post-step collision and minimum-separation measurement. `load_fleet_scenario` loads the shared vehicle scenario and scenario-defined missions/faults. The `airside_fleet` executable reports aggregate and individual mission metrics and typed traffic events.

The coordinator predicts constant-velocity closest approach over four simulated seconds and adds a 35 m approach buffer (45 m release hysteresis). A conflict uses a shared route edge resource when missions overlap on an edge, a shared intersection-node resource when they converge at a graph node, and a vehicle-pair fallback for geometric conflicts elsewhere. Per-resource contenders are gathered across all active pairs before the tick's reservation is granted, then sorted by simulation timestamp, lower integer mission priority, and vehicle ID; completed vehicles receive effective highest priority and remain stationary blockers. Request, grant, defer, wait, release, and collision decisions are timestamped events. This graph-derived reservation is activated by proximity prediction, rather than reserving the full route in advance. The coordinator is single-threaded; worker execution only parallelizes independent whole fleet runs.

The checked-in `autonomy_fleet.yaml` routes three vehicles through shared Gate A2 approaches to distinct destinations. On the Windows Release build, seed 42 ran 3,313 fixed-step ticks to 66.28 simulated seconds in 0.035 s wall time: all 3 missions completed, 0 timeouts, 0 collisions, 28.608 m minimum separation, 821.015 m total distance, 8.66 cumulative traffic-wait seconds, 2 pair-reservation requests, and 1 contention. Tug 3 waited from 12.38 s to 21.04 s, then continued and completed. The two-resource reservation table queues competing requests and transfers ownership on release; focused tests cover priority/ID ordering and exclusive release.

The `autonomy_fleet_fault.yaml` experiment injects a 14 s GNSS dropout into `tug_02` while it contends with the other two vehicles. Seed 42 completed all 3 missions in 74.64 simulated seconds with 0 collisions and 23.906 m minimum separation; `tug_02` recorded one degraded-mode entry and safely completed. The run recorded two contentions and 24.88 cumulative waiting seconds.

`autonomy_fleet_independent.yaml` validates three spatially separated routes: seed 42 completed all 3 missions in 32.36 simulated seconds with zero reservations, zero waiting, and zero collisions (82.467 m minimum separation). `autonomy_fleet_narrow_segment.yaml` uses a constrained graph where traffic from the depot and Gate A3 merges onto the same North-South segment; seed 42 deferred `tug_02` to `tug_01` at 25.12 s, released the edge at 37.50 s, and completed all 3 missions in 119.12 simulated seconds with one contention, 12.38 waiting seconds, one near-conflict event, one traffic-forced safety stop, zero collisions, and 15.228 m minimum separation. Fleet results count newly predicted vehicle-pair conflicts and traffic-induced safety stops, and emit `near_conflict`, `entered_conflict`, and `forced_safety_stop` events. This tests merging access to a shared narrow edge; it does not test opposing head-on traffic, which is not safely passable on the current graph without turnouts or alternate routing.

Fleet tests cover three-member stepping, unique IDs, scenario faults, route-resource contention, collision/separation checks, and repeat-seed event ordering. A 24-run seeded faulted-fleet matrix (seeds 42–65) matched exactly between serial execution and four workers for digest, collisions, waiting, distance, and mission completions; all 24 runs completed three missions with zero collisions. The ROS bridge accepts `--vehicle-id` and places every bridge topic under `/ramplab/<id>`; non-default vehicle TF frame IDs also carry the vehicle ID, and camera/LiDAR TCP ports must be unique per process. The bridge package suite reports 16 tests, zero failures. `verify_multi_vehicle_isolation.py` launched two bridge processes together and observed distinct command subscriptions and sensor topics: a 1.5 m/s command moved `fleet_alpha` 15.651 m while zero commands left `fleet_beta` within 0.002 m over 12 simulated seconds. One observed run counted 234/239 raw odometry messages and 121/120 LiDAR scans, and verified one bridge subscriber per command topic, one global `/clock` publisher (`fleet_beta` used `--no-global-clock`), and distinct per-vehicle TF chains. The probe sets Fast DDS to UDPv4 because the local Fast DDS 3.6.2 shared-memory path crashes when starting the second participant. These are independent bridge simulations, not one ROS-controlled fleet simulation.

The Unreal Editor target was built with UE 5.8.3 and MSVC 14.44. `-RampLabFleetValidation` selects the three-vehicle scenario. The latest runtime log recorded 3 vehicles, 3/3 missions completed, 0 timeouts, 8.66 s waiting, 1 contention, 0 collisions, and 28.608 m minimum separation. The viewer renders stable per-ID mesh actors with goal and moving/waiting/stopped/completed labels. This is local simulator runtime evidence, not physical vehicle validation.

The coordinator now builds a deterministic wait-for graph on each step. A directed cycle emits typed deadlock-detected and recovery events, increments `deadlock_count`, and commands each newly involved vehicle to stop; the stop remains active through that vehicle's existing mission timeout, so recovery prioritizes a controlled safe timeout over breaking the cycle by entering another conflict. `FleetDeadlockTest` covers a three-vehicle cycle and excludes a fourth vehicle that is merely blocked behind it. A deterministic search over 160 seeded three-mission combinations found no cycle; both checked-in scenarios also report zero deadlocks and complete all missions, so the integrated recovery branch has not yet been observed in a full fleet run. `airside_fleet --csv FILE --json FILE` exports aggregate metrics plus per-vehicle results, and JSON also includes typed traffic events. Resources still activate only after proximity prediction; there is no rerouting around stopped vehicles or reservation aging. ROS validation uses independent bridge processes, not one coordinated ROS fleet. No hardware or real-airport performance was tested.

## Reproduction

```powershell
cmake --build build-final-msvc --config Release
ctest --test-dir build-final-msvc -C Release --output-on-failure
.\build-final-msvc\Release\airside_autonomy.exe --scenario scenarios\autonomy_tug.yaml --seed 42
.\build-final-msvc\Release\airside_fleet.exe --scenario scenarios\autonomy_fleet.yaml --seed 42 --csv results\fleet.csv --json results\fleet.json
.\build-final-msvc\Release\airside_fleet.exe --scenario scenarios\autonomy_fleet_fault.yaml --seed 42 --json results\fleet_fault.json
.\build-final-msvc\Release\airside_fleet.exe --scenario scenarios\autonomy_fleet_independent.yaml --seed 42
.\build-final-msvc\Release\airside_fleet.exe --scenario scenarios\autonomy_fleet_narrow_segment.yaml --seed 42
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
