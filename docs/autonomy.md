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

## Headless use

Build using the normal Release configuration. Run the deterministic default mission:

```powershell
.\build-msvc\Release\airside_autonomy.exe --scenario scenarios\autonomy_tug.yaml --seed 42
```

Add `--record-trajectory results\tug.csv` to stream a diagnostic CSV with ground truth, GNSS estimate, heading, speed, command, route error, and minimum measured LiDAR range. Recording is off by default. Normal runs retain no trajectory history.

Run the independent stopping fixture with `airside_autonomy --scenario scenarios\autonomy_safety_stop.yaml --seed 42`. It deliberately blocks the initial forward envelope; the expected report is a safe timeout with at least one emergency stop and zero physical collisions.

## Experiments

The autonomy experiment executor has its own result type and bounded `std::jthread` pool; it does not place autonomy data into the operational `RunResult`. Each worker owns a scenario, RNG, controller, and simulation. Output contains final per-run metrics in stable ordinal order; sensor scans and trajectory samples are discarded. Example noise experiment:

```powershell
.\build-msvc\Release\airside_autonomy_experiment.exe --experiment experiments\autonomy_noise_validation.yaml --workers 1
.\build-msvc\Release\airside_autonomy_experiment.exe --experiment experiments\autonomy_noise_validation.yaml --workers 4
```

The definition uses paired seeds at GNSS σ values 0.1 m, 0.5 m, and 1.5 m. It reports completion rate, mean duration, P95 maximum route error, minimum clearance, collisions, wall time, and runs per second. Results are measurements of this build and host, not a hardware-independent performance guarantee.

## Unreal and ROS2 boundaries

Unreal remains a non-authoritative consumer. It advances the same fixed-step controller/sensor model and renders its read-only snapshots; it does not move the vehicle along a hand-authored spline or feed renderer state back to the controller. Airport placement remains governed by the existing KAUO transform.

ROS2 was not installed in the inspected Windows environment (no `ros2` command, ROS environment variables, or common install roots). The core and CLI therefore have no ROS2 dependency. To implement and validate that optional bridge, an installed ROS2 development environment compatible with the MSVC build is required, including `rclcpp`, `sensor_msgs`, `geometry_msgs`, `nav_msgs`, and `rosgraph_msgs`. No external controller, topic, `/clock`, or command-timeout behavior is claimed.

## Known limits

- The mission uses a small synthetic road graph and circle obstacles, not surveyed service-road geometry.
- The route follower is a simple deterministic geometric controller; very large GNSS noise can produce a timeout.
- The safety rule stops on close forward returns; it does not perform obstacle avoidance or replan around dynamic obstacles.
- Sensor models are illustrative and do not claim hardware calibration or statistically validated distributions.
- Unreal visualization displays snapshots from the live deterministic autonomy simulation; Cesium remains subject to its existing local credential and network requirements.
