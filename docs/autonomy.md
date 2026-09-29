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

The mission resolves the named Depot and Gate A2 nodes from `map_scenario`, calls the existing deterministic A* implementation, and converts route nodes to waypoints. The controller follows those waypoints in order with a bounded heading-rate command and reduces speed for turns and goal approach. It uses GNSS to correct a local position estimate, integrates wheel-odometry distance along the IMU heading between fixes, and uses the IMU yaw measurement as heading. GNSS corrections use a fixed 15% complementary blend.

An independent safety check looks for LiDAR returns within 0.20 rad of the forward axis and inside the configured stopping envelope; it requests zero target speed and counts the transition into an emergency stop. Collision checking separately treats the tug and obstacles as circles and records the first intersecting obstacle/time. Mission success requires being within goal tolerance at low speed; otherwise the result is collision, timeout, or controller failure.

## Sensors

Sensor schedules use simulation time and retain their most recent measurement between updates. Noise comes from one `std::mt19937_64` seeded per run; there is no global or wall-clock RNG.

| Sensor | Default rate | Values and noise |
|---|---:|---|
| GNSS | 5 Hz | Local x/y with configurable constant bias and independent Gaussian horizontal noise; accuracy field is 1.96σ. |
| IMU | 50 Hz | Heading, yaw rate, and longitudinal acceleration with independently configured Gaussian noise; no drift or gravity model. |
| Wheel odometry | 20 Hz | Integrated distance, speed, and heading change with configured Gaussian distance/speed noise; no wheel quantization or slip. |
| 2D LiDAR | 10 Hz | 181 beams over 180°, configurable range limits and Gaussian range noise. Headless ray/circle intersections return the nearest hit in beam order. |

The initial obstacle geometry uses circles. There are no graphics raycasts, camera images, SLAM, or map updates. Unreal mirrors these circles and the simulated sensor values.

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

The robustness CSV/JSON reports include configured faults, simulation and fault seeds, mission result, completion and route/clearance metrics, collision/emergency/safety counts, degraded-sensing stop time, fault transition times, final pose/speed, and generated/delivered/dropped/delayed counts per sensor. `command_timeouts` is zero for built-in runs; it is measured separately by the ROS bridge watchdog. No aggregate robustness score is computed.

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

The matrix includes a no-fault baseline, 2/5/10 s GNSS gaps, two GNSS noise levels and bias, LiDAR dropout/range/masking, IMU noise/dropout, two wheel-slip scales, delayed and lost packets, and three combined cases including a severe recoverable safe stop. Each row is a paired seed against the baseline and exports `runs.csv` and `runs.json`. Output contains raw measurements rather than a composite score. The older `autonomy_noise_validation.yaml` remains available for its 0.1/0.5/1.5 m GNSS-noise sweep.

## Goal 7 validation snapshot

On the 2026-09-29 Windows Release build, seed 42 no-fault autonomy completed in 66.36 s, traveled 309.76 m, had 0.734 m mean and 3.243 m maximum route error, 4.687 m minimum clearance, 0 collisions, and trajectory digest `1488735950019943363`. Those values match the pre-fault baseline. In the paired 3-seed matrix, all 63 runs completed without collision. The 2 s GNSS dropout preserved the seed-42 completion time; the 5 s and 10 s gaps triggered health stops and completed in 68.06 s and 74.24 s for seed 42. An 8 m GNSS bias completed in 67.38 s with a 6.480 m maximum route error. The severe 12 s LiDAR/GNSS/IMU dropout entered one controlled stop, remained stopped for 11.58 s due to degraded sensing, resumed after recovery, and completed in 78.78 s with 0 collisions. These are specific synthetic scenario measurements, not hardware performance claims.

## Unreal and ROS2 boundaries

Unreal remains a non-authoritative consumer. It advances the same fixed-step controller/sensor model and renders its read-only snapshots; it does not move the vehicle along a hand-authored spline or feed renderer state back to the controller. Airport placement remains governed by the existing KAUO transform.

The core and CLI have no ROS2 dependency. The optional native Windows ROS 2 Lyrical bridge and separately running external controller are implemented and validated; see [ros2.md](ros2.md) for the exact Pixi activation sequence, topics, frames, timing, mission results, and command-timeout behavior.

## Known limits

- The mission uses a small synthetic road graph and circle obstacles, not surveyed service-road geometry.
- The route follower is a simple deterministic geometric controller; very large GNSS noise can produce a timeout.
- The safety rule stops on close forward returns; it does not perform obstacle avoidance or replan around dynamic obstacles.
- Sensor and fault models are illustrative abstractions; they are not hardware-calibrated or certified distributions.
- Health checks and complementary GNSS/odometry behavior are not a probabilistic fusion stack or EKF.
- Fault scheduling is deterministic in simulation time, while ROS 2/DDS reception and Windows process scheduling remain wall-clock nondeterministic.
- Unreal visualization displays snapshots from the live deterministic autonomy simulation; Cesium remains subject to its existing local credential and network requirements.
