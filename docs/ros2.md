# Native Windows ROS 2 integration

Goal 6B adds an optional native ROS 2 Lyrical adapter for the deterministic tug autonomy simulation. ROS 2 remains outside the normal RampLab build graph: `airside_sim`, `airside_autonomy`, the CLIs, experiment runners, and Unreal viewer do not depend on ROS headers, libraries, or tools.

## Installed environment

The verified Windows installation is `C:\dev\ros2_lyrical`, using its own Pixi environment. RampLab intentionally has no Pixi manifest. Start every ROS 2 command through this installation and source its PowerShell setup before invoking ROS 2:

```powershell
Set-Location C:\dev\ros2_lyrical
pixi run powershell -NoProfile -ExecutionPolicy Bypass -Command "Set-Location 'C:\Users\Ferna\OneDrive\Documents\ChatGPT\RampLab'; . 'C:\dev\ros2_lyrical\local_setup.ps1'; ros2 --help"
```

For commands that use the built workspace, source its overlay after the installation setup:

```powershell
. 'C:\dev\ros2_lyrical\local_setup.ps1'
. 'C:\Users\Ferna\OneDrive\Documents\ChatGPT\RampLab\ros2_ws\install\local_setup.ps1'
```

The native build uses the installed VS 2026/MSVC toolchain and CMake 4.4.3. Pixi's CMake 3.28 does not recognize the installed Visual Studio 18 generator. With the ROS Pixi environment active, run:

```powershell
Set-Location C:\Users\Ferna\OneDrive\Documents\ChatGPT\RampLab
. 'C:\dev\ros2_lyrical\local_setup.ps1'
. 'C:\Users\Ferna\OneDrive\Documents\ChatGPT\RampLab\ros2_ws\scripts\build.ps1'
```

`build.ps1` puts the verified CMake 4.4.3 binary directory first on `PATH`, then builds the bridge and controller with colcon. Override `-CMakeBin` if CMake 4.4 is installed elsewhere. The bridge conversion suite has ten GTest cases plus one CTest harness result:

```powershell
Set-Location C:\Users\Ferna\OneDrive\Documents\ChatGPT\RampLab\ros2_ws
colcon test --packages-select ramplab_ros2_bridge --merge-install --ctest-args -C Release
colcon test-result --all --verbose
```

The tests can also be run in one activated Pixi invocation using the setup sequence above.

## Workspace packages

```text
ros2_ws/src/
├── ramplab_ros2_common/       # WGS84/local ENU conversion
├── ramplab_ros2_bridge/       # ROS messages <-> existing autonomy simulation
└── ramplab_ros2_controller/   # independent rclcpp executable
```

The bridge imports the already-built RampLab autonomy libraries; this is an adapter, not a new simulation implementation. The external controller links ROS packages only. It does not include or link RampLab simulation, ground-truth, obstacle-state, estimator, or controller internals. It consumes the route, fused odometry, estimator health, and LiDAR messages and publishes `cmd_vel`.

## Topics and QoS

All tug topics are under `/ramplab/tug1`. Sensor rates below are configured simulation rates; measured rates came from an 8-second external `rclpy` observation during a 1× real-time run.

| Topic | Type | Direction | Configured simulation rate / observed rate (simulation time unless noted) | Frame |
|---|---|---|---:|---|
| `/clock` | `rosgraph_msgs/msg/Clock` | bridge publishes | 50 / 50.00 Hz | — |
| `/ramplab/tug1/scan` | `sensor_msgs/msg/LaserScan` | bridge publishes | 10 / 10.00 Hz | `lidar` |
| `/ramplab/tug1/imu` | `sensor_msgs/msg/Imu` | bridge publishes | 50 / 49.13 Hz* | `imu` |
| `/ramplab/tug1/odom` | `nav_msgs/msg/Odometry` | bridge publishes | 20 / 20.03 Hz | `odom`, child `base_link` |
| `/ramplab/tug1/filtered_odom` | `nav_msgs/msg/Odometry` | bridge publishes | 50 Hz simulation updates | `odom`, child `base_link` |
| `/ramplab/tug1/estimator_health` | `std_msgs/msg/UInt8` | bridge publishes | 50 Hz simulation updates | 0 uninitialized, 1 healthy, 2 degraded, 3 unsafe, 4 invalid |
| `/ramplab/tug1/estimator_diagnostics` | `std_msgs/msg/Float64MultiArray` | bridge publishes | 50 Hz simulation updates | `[position_sigma_m, heading_sigma_rad, gnss_age_s, accepted, rejected, stale, gate_activations, last_gnss_nis, wheel_health, gnss_recovery, wheel_inconsistencies, wheel_transitions, reacq_attempts, reacq_successes, candidate_rejections, wheel_downweighted, degraded_time_s, max_gnss_nis]` |
| `/ramplab/tug1/gnss` | `sensor_msgs/msg/NavSatFix` | bridge publishes | 5 / 5.00 Hz | `gnss` |
| `/ramplab/tug1/camera/image_raw` | `sensor_msgs/msg/Image` | bridge publishes Unreal RGB frames | up to 20 Hz | `camera`, `bgra8` |
| `/ramplab/tug1/camera/camera_info` | `sensor_msgs/msg/CameraInfo` | bridge publishes with each image | up to 20 Hz | `camera`, zero-distortion pinhole model |
| `/ramplab/tug1/route` | `nav_msgs/msg/Path` | bridge publishes once, transient-local | mission route | `map` |
| `/ramplab/tug1/cmd_vel` | `geometry_msgs/msg/Twist` | controller publishes; bridge subscribes | not stamped / measured 20.05 Hz wall rate | — |
| `/tf` | `tf2_msgs/msg/TFMessage` | bridge publishes | filtered odometry updates | `odom` → `base_link` |
| `/tf_static` | `tf2_msgs/msg/TFMessage` | bridge publishes | static | `map` → `odom`; `base_link` → `gnss`, `imu`, `wheel_odom`, `lidar`, `camera` |

The measured wall rates at factor 1 were approximately 50.93, 10.05, 49.29, 20.10, and 4.99 Hz respectively for clock, scan, IMU, odometry, and GNSS. The probe enforces strictly increasing simulation stamps and checks rates within 35% of configured values. `*`IMU's measured rate is lower because the best-effort depth-one stream may drop samples while the Windows processes are scheduled; its simulation timestamp span is 49.13 Hz and the wall observation was 49.29 Hz.

Sensor streams use best-effort depth one. The path is reliable, transient-local, depth one; command velocity is best-effort volatile depth one; clock is reliable depth ten. The live topic graph showed the external controller as the `cmd_vel` publisher and the bridge as its subscriber.

## Time, frames, and coordinates

The bridge publishes `/clock` at the 20 ms simulation step, before publishing the measurements for that step. Measurement headers use that same simulation clock, never wall time. The monitor confirmed strictly increasing clock and sensor stamps. `geometry_msgs/Twist` has no header timestamp; command freshness is measured by the bridge's simulation-time receive stamp. Non-finite velocity values are rejected, and finite speed/yaw values are clamped to scenario limits.

`map` is the local east/north tangent plane. `odom` is identity-aligned to `map`. TF has one owner: the bridge publishes static `map → odom`, dynamic `odom → base_link` from `/filtered_odom`, and static `base_link → gnss`, `imu`, `wheel_odom`, `lidar`, and `camera` transforms using the sensor extrinsics from the loaded scenario YAML. Raw wheel `/odom` does not publish TF. Filtered pose covariance maps east/north/yaw into ROS 6×6 pose covariance; speed variance is in twist covariance and unmodeled axes carry large variances. GNSS converts local ENU to WGS84 about the KAUO reference (32.6151667°, -85.4340000°, 208.27 m ellipsoid height); `NavSatFix` reports horizontal variance on its diagonal.

### Unreal RGB transport

When the Unreal camera component connects, it streams each captured 320×180 BGRA8 frame over a localhost TCP connection to the bridge (`127.0.0.1:39010` by default). The bridge accepts only loopback clients and publishes the paired `Image` and `CameraInfo` with the camera simulation timestamp and frame ID. Configure the same port in the Unreal component's `TransportPort` and bridge's `--camera-port` option. The versioned `RLSN` frame header carries timestamp, sequence, dimensions, byte count, and horizontal FOV; the bridge rejects malformed frames and retains only the newest complete frame if several arrive between simulation ticks. A disconnected camera leaves the image topics quiet while other sensors and the controller continue. This local socket is a transport adapter; it does not make Unreal authoritative over vehicle state.

LiDAR defaults to the deterministic core scan. To let the ROS controller consume Unreal's scene geometry rays, run the bridge with `--lidar-source unreal` (optional `--lidar-port`, default 39011) and connect the Unreal LiDAR component's `TransportPort` to the same port. Unreal streams `RLLD` v1 records containing simulation timestamp, sequence, angular bounds/resolution, range limits, and float32 ranges. The receiver is loopback-only, validates the full scan, and publishes geometry-derived messages on the existing `/ramplab/tug1/scan` topic. In this mode, the bridge waits for the first Unreal scan before advancing its simulation clock, then paces it from simulation time; the external ROS controller therefore receives the actual Unreal scan through its existing safety subscription. The core estimator and RampLab vehicle state remain driven by the authoritative portable simulation. The default `synthetic` source preserves headless/ROS behavior when Unreal is absent.

## Run the external mission

Use two PowerShell terminals. In each, start from `C:\dev\ros2_lyrical`, invoke `pixi run powershell -NoProfile -ExecutionPolicy Bypass`, then set the RampLab directory and source the installation and workspace overlay:

```powershell
Set-Location C:\Users\Ferna\OneDrive\Documents\ChatGPT\RampLab
. 'C:\dev\ros2_lyrical\local_setup.ps1'
. '.\ros2_ws\install\local_setup.ps1'
```

Terminal 1 runs the separately compiled external process:

```powershell
ros2 run ramplab_ros2_controller ramplab_ros2_controller --ros-args -p use_sim_time:=true
```

Terminal 2 starts the simulation-backed bridge:

```powershell
ros2 run ramplab_ros2_bridge ramplab_ros2_bridge --scenario scenarios\autonomy_tug.yaml --seed 42 --realtime-factor 1
```

The independent live-topic/rate probe is:

```powershell
python .\ros2_ws\scripts\verify_live_topics.py --duration 8 --factor 1
```

## Deterministic sensor fault validation

The bridge loads fault entries from the same autonomy scenario YAML as the headless simulation. It publishes faulted `SensorFrame` values through the existing ROS conversion boundary: GNSS dropout omits `NavSatFix`, range faults change `LaserScan.range_max`, odometry faults change the measured increments/speed, delayed messages retain their original simulation-time header stamp, and `/clock` keeps advancing. The controller remains an independent process and receives only ROS route/sensor messages; the ROS graph has no ground-truth topic.

Build the core first, then rebuild the native ROS overlay after core changes:

```powershell
Set-Location C:\dev\ros2_lyrical
pixi run powershell -NoProfile -ExecutionPolicy Bypass -Command "Set-Location 'C:\Users\Ferna\OneDrive\Documents\ChatGPT\RampLab'; . 'C:\dev\ros2_lyrical\local_setup.ps1'; . '.\ros2_ws\scripts\build.ps1'"
```

The script below launches the separately compiled controller and bridge as distinct processes and observes `/clock`, GNSS, LiDAR, raw/filtered odometry, estimator covariance/health/gates, TF, and `cmd_vel`. Its staged scenario checks GNSS dropout and covariance growth, LiDAR range degradation, wheel slip, biased GNSS plus IMU degradation and gate activation, combined faults, and a severe outage that causes the external controller to command zero speed. It also confirms the graph has no ground-truth topic and one dynamic TF publisher:

```powershell
Set-Location C:\dev\ros2_lyrical
pixi run powershell -NoProfile -ExecutionPolicy Bypass -Command "Set-Location 'C:\Users\Ferna\OneDrive\Documents\ChatGPT\RampLab'; . 'C:\dev\ros2_lyrical\local_setup.ps1'; . '.\ros2_ws\install\local_setup.ps1'; python .\ros2_ws\scripts\verify_fault_topics.py --scenario scenarios\autonomy_ros2_fault_validation.yaml --seed 42 --fault-seed 7019 --factor 1"
```

The final staged probe passed over 34.000 simulation seconds and published 1,628 filtered odometry samples. Reported radial position uncertainty increased from 0.300 to 0.540 m during GNSS loss; the biased-GNSS interval triggered 54 gate activations. The measured odometry/command-model speed ratio was 0.750 during 25% slip versus 0.965 nominal. The severe-loss interval produced 321 zero-speed controller commands. The requested 34 s horizon ended as `TIMEOUT` by design; collisions were zero, GNSS dropout fixes were zero, and the ROS graph had no ground-truth topic.

The same two-process probe can regression-check the Goal 6B command watchdog by stopping only the external controller at simulation time 5 s. The probe requires a command-timeout activation after controller termination and a zero-speed finish without collision. Windows process scheduling can also produce an earlier startup timeout if the controller has not published its first command yet:

```powershell
Set-Location C:\dev\ros2_lyrical
pixi run powershell -NoProfile -ExecutionPolicy Bypass -Command "Set-Location 'C:\Users\Ferna\OneDrive\Documents\ChatGPT\RampLab'; . 'C:\dev\ros2_lyrical\local_setup.ps1'; . '.\ros2_ws\install\local_setup.ps1'; python .\ros2_ws\scripts\verify_fault_topics.py --scenario scenarios\autonomy_tug.yaml --max-sim-seconds 12 --kill-controller-at 5 --factor 1"
```

The clean external Depot → Gate A2 run with the estimator completed successfully at 66.660 s simulation time, travelled 309.459 m, had 0.799 m mean and 3.465 m maximum route error, 4.793 m minimum obstacle clearance, zero collisions, and final speed 0.135 m/s. It published 667 scans, 3334 IMUs, 1334 raw odometry messages, and 334 GNSS messages. The 2× probe observed 3326 filtered-odometry messages and one `/tf` publisher (`ramplab_bridge`). Two startup command timeouts at 3.640 s and 5.680 s cleared when fresh controller commands arrived.

Reproduce this clean separate-process mission and frame/graph check with:

```powershell
Set-Location C:\dev\ros2_lyrical
pixi run powershell -NoProfile -ExecutionPolicy Bypass -Command "Set-Location 'C:\Users\Ferna\OneDrive\Documents\ChatGPT\RampLab'; . 'C:\dev\ros2_lyrical\local_setup.ps1'; . '.\ros2_ws\install\local_setup.ps1'; python .\ros2_ws\scripts\verify_fault_topics.py --scenario scenarios\autonomy_tug.yaml --seed 42 --fault-seed 7019 --factor 2 --max-sim-seconds 80 --clean-mission"
```

## Command timeout and determinism

The bridge defaults to a 0.500 simulation-second command timeout. On expiry it supplies zero target speed and yaw rate to the unchanged tug dynamics, which decelerate the tug under the scenario's limit. In the watchdog regression the external controller was terminated at 5.000 s; the bridge logged expiry at 6.380 s (last-command age 0.520 s), counted one post-termination timeout, and ended its 15 s observation at speed 0.000 m/s with zero collisions.

The standalone deterministic simulator and its seeded tests remain deterministic and ROS-independent. With an external process, DDS delivery, Windows scheduling, sensor sample reception, and command callback timing depend on process scheduling; therefore the ROS-controlled trajectory digest is not expected to match the built-in controller. The built-in seed-42 reference is 66.36 s, 309.76 m, 0.73 m mean route error, zero collisions, digest `1488735950019943363`. No lockstep transport was implemented.

## Fused estimator interface

The bridge adapts the core estimate to `/ramplab/tug1/filtered_odom` (`nav_msgs/msg/Odometry`) with simulation-time header stamps and planar pose/speed covariance. `/ramplab/tug1/estimator_health` is a `std_msgs/msg/UInt8` state code: 0 uninitialized, 1 healthy, 2 degraded, 3 unsafe, 4 invalid. The 18 diagnostic values preserve the original first eight fields and append wheel health (0 nominal, 1 suspect, 2 degraded), GNSS recovery (0 tracking, 1 inconsistent, 2 reacquiring, 3 recovered), wheel inconsistency and transition counts, reacquisition attempt and success counts, rejected-candidate and downweighted-wheel counts, degraded time, and maximum GNSS NIS. The controller consumes filtered odometry and the health code; it does not recompute localization from GNSS or raw wheel odometry.

The bridge alone publishes dynamic `odom → base_link` from filtered odometry. Its static publisher owns `map → odom`, `base_link → lidar`, and `base_link → imu`. Raw `/odom` remains a sensor stream and has no associated TF broadcast. The live probe checks these frame edges and confirms a single `/tf` publisher.

Goal 9 validation on 2026-09-29 rebuilt the native overlay and passed all 11 bridge conversion/harness tests. The staged fault probe ran to its requested 34 s horizon with zero collisions, observed health states healthy/degraded/unsafe, covariance growth from 0.300 to 0.540 m, 36 GNSS gate activations, no fixes during dropout, 12 m LiDAR range, and measured/command-model speed ratio 0.750 during 25% slip versus 0.998 nominal. A separate 80 s Goal 9 reacquisition probe observed degraded wheel health, six reacquisition-success updates, 4,001 filtered odometry samples, and zero collisions; it ended in `TIMEOUT` at the observation horizon, so it verifies the sensor, estimator, and graph behavior rather than mission completion. Both probes found no ground-truth topic and one dynamic TF publisher.

## Validation and limits

The bridge accepts `--vehicle-id tug_01` to set its ROS namespace to `/ramplab/tug_01`; the default remains `tug1`. Run one bridge process per vehicle and choose unique `--camera-port` and `--lidar-port` values. `--no-global-clock` disables that process's global `/clock` publisher so exactly one bridge owns `/clock` when independent bridge processes share a ROS graph. The namespace unit test rejects IDs containing path separators or punctuation.

`ros2_ws/scripts/verify_multi_vehicle_isolation.py` starts two bridge processes concurrently, publishes different commands to their namespaced `cmd_vel` inputs, and checks namespaced odometry, LiDAR, and TF outputs. In one 12-second run, the commanded vehicle moved 15.651 m and the zero-command vehicle moved 0.002 m; both streams published sensor data, each command topic had one bridge subscriber, only one bridge published global `/clock`, and the two TF frame chains remained separate. Non-default IDs prefix the bridge's TF frames with the vehicle ID; default `tug1` retains the existing frame IDs. On this Windows Fast DDS 3.6.2 setup, the default shared-memory transport crashes when starting the second participant; the probe sets the documented `FASTDDS_BUILTIN_TRANSPORTS=UDPv4` transport setting for both bridges and its rclpy observer. Reproduce it from the ROS workspace with:

```powershell
pixi run powershell -NoProfile -ExecutionPolicy Bypass -Command "Set-Location 'C:\Users\Ferna\OneDrive\Documents\ChatGPT\RampLab'; . 'C:\dev\ros2_lyrical\install\setup.ps1'; . '.\ros2_ws\install\setup.ps1'; python .\ros2_ws\scripts\verify_multi_vehicle_isolation.py"
```

This verifies namespaced command/sensor separation across two independent bridge simulations; it does not connect both bridges to one coordinated fleet simulation.

The environment check found ROS 2 Lyrical, `rclcpp`, `ament_cmake`, colcon-core 0.17.1, all six requested message interfaces, and the Fast DDS RMW. `ros2 --help`, C++ demo talker/listener processes, and a separately compiled native `rclcpp` node succeeded. The normal RampLab build remains usable without this installation.

This is a planar synthetic mission, not surveyed airport-road geometry or hardware-calibrated sensing. The GNSS/odometry estimate and geometric route follower are deliberately modest; the LiDAR rule can stop but cannot route around a blockage. ROS 2 middleware scheduling is not deterministic lockstep. The camera stream is observational and does not feed the controller. There is no Nav2, Gazebo, or multi-vehicle stack in this milestone.
