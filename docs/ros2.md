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

`build.ps1` puts the verified CMake 4.4.3 binary directory first on `PATH`, then builds the bridge and controller with colcon. Override `-CMakeBin` if CMake 4.4 is installed elsewhere. To run the nine conversion/watchdog cases:

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

The bridge imports the already-built RampLab autonomy libraries; this is an adapter, not a new simulation implementation. The external controller links only ROS packages and the small coordinate-conversion header. It does not include or link RampLab simulation, ground-truth, obstacle-state, or controller internals. It consumes the published route, GNSS, IMU, wheel odometry, and LiDAR messages and publishes `cmd_vel`.

## Topics and QoS

All tug topics are under `/ramplab/tug1`. Sensor rates below are configured simulation rates; measured rates came from an 8-second external `rclpy` observation during a 1× real-time run.

| Topic | Type | Direction | Configured simulation rate / observed rate (simulation time unless noted) | Frame |
|---|---|---|---:|---|
| `/clock` | `rosgraph_msgs/msg/Clock` | bridge publishes | 50 / 50.00 Hz | — |
| `/ramplab/tug1/scan` | `sensor_msgs/msg/LaserScan` | bridge publishes | 10 / 10.00 Hz | `lidar` |
| `/ramplab/tug1/imu` | `sensor_msgs/msg/Imu` | bridge publishes | 50 / 49.13 Hz* | `imu` |
| `/ramplab/tug1/odom` | `nav_msgs/msg/Odometry` | bridge publishes | 20 / 20.03 Hz | `odom`, child `base_link` |
| `/ramplab/tug1/gnss` | `sensor_msgs/msg/NavSatFix` | bridge publishes | 5 / 5.00 Hz | `gnss` |
| `/ramplab/tug1/route` | `nav_msgs/msg/Path` | bridge publishes once, transient-local | mission route | `map` |
| `/ramplab/tug1/cmd_vel` | `geometry_msgs/msg/Twist` | controller publishes; bridge subscribes | not stamped / measured 20.05 Hz wall rate | — |
| `/tf` | `tf2_msgs/msg/TFMessage` | bridge publishes | odometry updates | `odom` → `base_link` |
| `/tf_static` | `tf2_msgs/msg/TFMessage` | bridge publishes | static | `map` → `odom`; `base_link` → `lidar`, `imu` |

The measured wall rates at factor 1 were approximately 50.93, 10.05, 49.29, 20.10, and 4.99 Hz respectively for clock, scan, IMU, odometry, and GNSS. The probe enforces strictly increasing simulation stamps and checks rates within 35% of configured values. `*`IMU's measured rate is lower because the best-effort depth-one stream may drop samples while the Windows processes are scheduled; its simulation timestamp span is 49.13 Hz and the wall observation was 49.29 Hz.

Sensor streams use best-effort depth one. The path is reliable, transient-local, depth one; command velocity is best-effort volatile depth one; clock is reliable depth ten. The live topic graph showed the external controller as the `cmd_vel` publisher and the bridge as its subscriber.

## Time, frames, and coordinates

The bridge publishes `/clock` at the 20 ms simulation step, before publishing the measurements for that step. Measurement headers use that same simulation clock, never wall time. The monitor confirmed strictly increasing clock and sensor stamps. `geometry_msgs/Twist` has no header timestamp; command freshness is measured by the bridge's simulation-time receive stamp. Non-finite velocity values are rejected, and finite speed/yaw values are clamped to scenario limits.

`map` is the local east/north tangent plane. `odom` is currently identity-aligned to `map`. `odom → base_link` is derived from measured wheel-odometry increments (with heading corrected from IMU), rather than the simulation's truth pose. `base_link → lidar` and `base_link → imu` are fixed identity transforms for this planar sensor model. GNSS converts local ENU to WGS84 about the KAUO reference (32.6151667°, -85.4340000°, 208.27 m ellipsoid height); `NavSatFix` reports horizontal variance on its diagonal.

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

The verified external Depot → Gate A2 mission completed successfully: 66.700 s simulation time, 310.063 m travelled, 0.767 m mean route error, 3.348 m maximum route error, 4.724 m minimum obstacle clearance, zero emergency stops, zero collisions, and zero command timeouts. It published 668 scans, 3336 IMUs, 1335 odometry messages, and 334 GNSS messages.

## Command timeout and determinism

The bridge defaults to a 0.500 simulation-second command timeout. On expiry it supplies zero target speed and yaw rate to the unchanged tug dynamics, which decelerate the tug under the scenario's limit. A second run force-terminated the native controller process at about simulation time 5.66 s. The bridge logged expiry at 6.180 s (last-command age 0.520 s), counted one timeout, and ended its 15 s observation with speed 0.000 m/s and zero collisions.

The standalone deterministic simulator and its seeded tests remain deterministic and ROS-independent. With an external process, DDS delivery, Windows scheduling, sensor sample reception, and command callback timing depend on process scheduling; therefore the ROS-controlled trajectory digest is not expected to match the built-in controller. The built-in seed-42 reference is 66.36 s, 309.76 m, 0.73 m mean route error, zero collisions, digest `1488735950019943363`. No lockstep transport was implemented.

## Validation and limits

The environment check found ROS 2 Lyrical, `rclcpp`, `ament_cmake`, colcon-core 0.17.1, all six requested message interfaces, and the Fast DDS RMW. `ros2 --help`, C++ demo talker/listener processes, and a separately compiled native `rclcpp` node succeeded. The normal RampLab build remains usable without this installation.

This is a planar synthetic mission, not surveyed airport-road geometry or hardware-calibrated sensing. The GNSS/odometry estimate and geometric route follower are deliberately modest; the LiDAR rule can stop but cannot route around a blockage. ROS 2 middleware scheduling is not deterministic lockstep. There is no Nav2, Gazebo, camera, or multi-vehicle stack in this milestone.
