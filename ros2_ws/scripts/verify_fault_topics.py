#!/usr/bin/env python3
"""Run the separate ROS controller and bridge, then verify injected sensor effects."""
import argparse
import math
import os
from pathlib import Path
import subprocess
import statistics
import tempfile
import time

import rclpy
from builtin_interfaces.msg import Time
from geometry_msgs.msg import Twist
from nav_msgs.msg import Odometry
from rosgraph_msgs.msg import Clock
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy, DurabilityPolicy
from sensor_msgs.msg import LaserScan, NavSatFix
from std_msgs.msg import UInt8
from std_msgs.msg import Float64MultiArray
from tf2_msgs.msg import TFMessage


def seconds(stamp: Time) -> float:
    return float(stamp.sec) + float(stamp.nanosec) / 1.0e9


class Probe(Node):
    def __init__(self):
        super().__init__("ramplab_fault_topic_probe")
        sensor_qos = QoSProfile(depth=10, reliability=ReliabilityPolicy.BEST_EFFORT)
        reliable_qos = QoSProfile(depth=10, reliability=ReliabilityPolicy.RELIABLE)
        static_qos = QoSProfile(depth=10, reliability=ReliabilityPolicy.RELIABLE,
                                durability=DurabilityPolicy.TRANSIENT_LOCAL)
        command_qos = QoSProfile(depth=10, reliability=ReliabilityPolicy.BEST_EFFORT,
                                 durability=DurabilityPolicy.VOLATILE)
        self.sim_time = 0.0
        self.clock = []
        self.gnss = []
        self.scans = []
        self.odometry = []
        self.filtered_odometry = []
        self.estimator_health = []
        self.estimator_diagnostics = []
        self.dynamic_tf = set()
        self.static_tf = set()
        self.dynamic_tf_publishers = set()
        self.commands = []
        self.expected_speeds = []
        self.expected_speed = 0.0
        self.last_command_time = None
        self.create_subscription(Clock, "/clock", self.on_clock, reliable_qos)
        self.create_subscription(NavSatFix, "/ramplab/tug1/gnss", self.on_gnss, sensor_qos)
        self.create_subscription(LaserScan, "/ramplab/tug1/scan", self.on_scan, sensor_qos)
        self.create_subscription(Odometry, "/ramplab/tug1/odom", self.on_odom, sensor_qos)
        self.create_subscription(Odometry, "/ramplab/tug1/filtered_odom", self.on_filtered_odom, sensor_qos)
        self.create_subscription(UInt8, "/ramplab/tug1/estimator_health", self.on_estimator_health, sensor_qos)
        self.create_subscription(Float64MultiArray, "/ramplab/tug1/estimator_diagnostics", self.on_estimator_diagnostics, sensor_qos)
        self.create_subscription(TFMessage, "/tf", self.on_tf, sensor_qos)
        self.create_subscription(TFMessage, "/tf_static", self.on_static_tf, static_qos)
        self.create_subscription(Twist, "/ramplab/tug1/cmd_vel", self.on_command, command_qos)

    def on_clock(self, msg):
        self.sim_time = seconds(msg.clock)
        self.clock.append(self.sim_time)

    def on_gnss(self, msg):
        self.gnss.append(seconds(msg.header.stamp))

    def on_scan(self, msg):
        self.scans.append((seconds(msg.header.stamp), float(msg.range_max)))

    def on_odom(self, msg):
        self.odometry.append((seconds(msg.header.stamp), float(msg.pose.pose.position.x),
                              float(msg.pose.pose.position.y), float(msg.twist.twist.linear.x)))

    def on_filtered_odom(self, msg):
        self.filtered_odometry.append((seconds(msg.header.stamp), float(msg.pose.covariance[0]),
                                       float(msg.pose.covariance[7]), float(msg.pose.covariance[35]),
                                       float(msg.pose.pose.position.x), float(msg.pose.pose.position.y)))

    def on_estimator_health(self, msg):
        self.estimator_health.append((self.sim_time, int(msg.data)))

    def on_estimator_diagnostics(self, msg):
        if len(msg.data) >= 8:
            self.estimator_diagnostics.append((self.sim_time, *map(float, msg.data[:8])))

    def on_tf(self, msg):
        self.dynamic_tf.update((tf.header.frame_id, tf.child_frame_id) for tf in msg.transforms)

    def on_static_tf(self, msg):
        self.static_tf.update((tf.header.frame_id, tf.child_frame_id) for tf in msg.transforms)

    def on_command(self, msg):
        now = self.sim_time
        if self.last_command_time is not None:
            dt = max(0.0, now - self.last_command_time)
            target = max(0.0, float(msg.linear.x))
            delta = target - self.expected_speed
            limit = 1.0 if delta >= 0.0 else 1.5
            self.expected_speed += max(-limit * dt, min(limit * dt, delta))
        self.last_command_time = now
        self.expected_speeds.append((now, self.expected_speed))
        self.commands.append((now, float(msg.linear.x), float(msg.angular.z)))


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def terminate_process_tree(process):
    if process.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(process.pid)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    else:
        process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--scenario", default="scenarios/autonomy_ros2_fault_validation.yaml")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--fault-seed", type=int, default=7019)
    parser.add_argument("--factor", type=float, default=1.0)
    parser.add_argument("--max-sim-seconds", type=float, default=34.0)
    parser.add_argument("--clean-mission", action="store_true",
                        help="verify a clean external-controller mission through completion")
    parser.add_argument("--kill-controller-at", type=float,
                        help="stop the separate controller at this simulation time to verify the bridge watchdog")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    scenario = (root / args.scenario).resolve()
    require(scenario.is_file(), f"scenario not found: {scenario}")
    rclpy.init()
    probe = Probe()
    controller_log = tempfile.TemporaryFile(mode="w+")
    bridge_log = tempfile.TemporaryFile(mode="w+")
    controller = subprocess.Popen([
        "ros2", "run", "ramplab_ros2_controller", "ramplab_ros2_controller",
        "--ros-args", "-p", "use_sim_time:=true",
    ], stdout=controller_log, stderr=subprocess.STDOUT, env=os.environ.copy())
    bridge = None
    controller_killed = False
    try:
        time.sleep(1.0)
        bridge = subprocess.Popen([
            "ros2", "run", "ramplab_ros2_bridge", "ramplab_ros2_bridge",
            "--scenario", str(scenario), "--seed", str(args.seed), "--fault-seed", str(args.fault_seed),
            "--realtime-factor", str(args.factor), "--max-sim-seconds", str(args.max_sim_seconds),
        ], stdout=bridge_log, stderr=subprocess.STDOUT, env=os.environ.copy())
        deadline = time.monotonic() + max(45.0, args.max_sim_seconds / args.factor * 3.0)
        while bridge.poll() is None and time.monotonic() < deadline:
            rclpy.spin_once(probe, timeout_sec=0.05)
            probe.dynamic_tf_publishers.update(
                endpoint.node_name for endpoint in probe.get_publishers_info_by_topic("/tf")
                if endpoint.node_name != "_NODE_NAME_UNKNOWN_")
            if args.kill_controller_at is not None and not controller_killed and probe.sim_time >= args.kill_controller_at:
                terminate_process_tree(controller)
                controller_killed = True
            if controller.poll() is not None and not controller_killed:
                raise RuntimeError("external controller process exited before the bridge")
        if bridge.poll() is None:
            bridge.terminate()
            raise RuntimeError("bridge exceeded the validation deadline")
        for _ in range(20):
            rclpy.spin_once(probe, timeout_sec=0.05)
        bridge_log.seek(0)
        bridge_output = bridge_log.read()
        controller_log.seek(0)
        controller_output = controller_log.read()
        require(bridge.returncode == 0, f"bridge returned {bridge.returncode}:\n{bridge_output}")
        if controller_killed:
            timeout_times = []
            for line in bridge_output.splitlines():
                marker = "cmd_vel timeout at sim_time="
                if marker in line:
                    timeout_times.append(float(line.split(marker, 1)[1].split(" ", 1)[0]))
            post_kill_timeouts = [stamp for stamp in timeout_times if stamp >= args.kill_controller_at]
            require(post_kill_timeouts,
                    f"bridge watchdog did not activate after the controller was terminated:\n{bridge_output}")
            require("Final speed: 0.000 m/s" in bridge_output and "Collisions: 0" in bridge_output,
                    f"watchdog did not safely stop the tug:\n{bridge_output}")
            print(f"ROS command-watchdog regression PASS: post-termination timeout={post_kill_timeouts[-1]:.3f} sim s, final speed=0.000 m/s, collisions=0")
            print("Bridge summary:\n" + bridge_output.strip())
            return
        require(controller.poll() is None, "external ROS controller unexpectedly exited")
        require(len(probe.clock) > 100, "too few /clock messages received")
        require(all(a < b for a, b in zip(probe.clock, probe.clock[1:])), "/clock did not increase strictly")
        if not args.clean_mission:
            require(max(probe.clock) >= args.max_sim_seconds - 0.05, "simulation clock did not reach requested limit")
        require(len(probe.filtered_odometry) > 100, "filtered odometry was not published")
        require(all(a[0] < b[0] for a, b in zip(probe.filtered_odometry, probe.filtered_odometry[1:])),
                "filtered odometry stamps did not increase monotonically")
        require(any(x >= 0.0 and y >= 0.0 and yaw >= 0.0
                    for _, x, y, yaw, _, _ in probe.filtered_odometry),
                "filtered odometry covariance is missing or invalid")
        if args.clean_mission:
            require("Result: SUCCESS" in bridge_output and "Collisions: 0" in bridge_output,
                    f"clean external mission did not complete safely:\n{bridge_output}")
            require(not any("ground_truth" in name or "groundtruth" in name for name, _ in probe.get_topic_names_and_types()),
                    "a ground-truth topic was visible in the ROS graph")
            require(("odom", "base_link") in probe.dynamic_tf,
                    "filtered odometry did not own the dynamic odom -> base_link transform")
            require(("map", "odom") in probe.static_tf and ("base_link", "imu") in probe.static_tf and
                    ("base_link", "lidar") in probe.static_tf,
                    "static TF ownership is missing map/odom or sensor transforms")
            require(len(probe.dynamic_tf_publishers) == 1,
                    f"expected one dynamic TF publisher, got {sorted(probe.dynamic_tf_publishers)}")
            print(f"ROS clean mission PASS: completion, collisions=0, filtered odometry={len(probe.filtered_odometry)}, "
                  f"/clock={max(probe.clock):.3f} sim s, TF owner={sorted(probe.dynamic_tf_publishers)}")
            print("Bridge summary:\n" + bridge_output.strip())
            return
        require(probe.estimator_health and any(code == 2 for _, code in probe.estimator_health),
                "estimator did not report degraded health during injected faults")
        before_loss = [v[1] for v in probe.estimator_diagnostics if 4.0 <= v[0] < 4.8]
        during_loss = [v[1] for v in probe.estimator_diagnostics if 7.0 <= v[0] < 7.8]
        require(before_loss and during_loss and statistics.median(during_loss) > statistics.median(before_loss),
                "reported position covariance did not grow during GNSS loss")
        require(any(v[7] > 0 for v in probe.estimator_diagnostics if 14.0 <= v[0] < 18.0),
                "biased GNSS measurements did not trigger innovation rejection")
        require(not any(5.0 <= stamp < 8.0 for stamp in probe.gnss), "GNSS dropout interval published a fix")
        degraded_scans = [r for stamp, r in probe.scans if 8.0 <= stamp < 11.0]
        require(degraded_scans and all(abs(r - 12.0) < 0.02 for r in degraded_scans),
                "LiDAR range_limit was not reflected in LaserScan.range_max")
        normal_scans = [r for stamp, r in probe.scans if 2.0 <= stamp < 4.0]
        require(normal_scans and all(abs(r - 30.0) < 0.02 for r in normal_scans),
                "baseline LiDAR range was not observed")
        slip = [v for stamp, _, _, v in probe.odometry if 11.0 <= stamp < 14.0]
        require(slip and all(math.isfinite(v) for v in slip), "faulted odometry was not published")
        def speed_ratios(start, end):
            values = []
            for stamp, _, _, speed in probe.odometry:
                if not start <= stamp < end or speed <= 0.5:
                    continue
                nearest = min(probe.expected_speeds, key=lambda item: abs(item[0] - stamp), default=None)
                if nearest and abs(nearest[0] - stamp) <= 0.15 and nearest[1] > 1.0:
                    values.append(speed / nearest[1])
            return values
        nominal_ratio = speed_ratios(3.0, 4.5)
        slip_ratio = speed_ratios(12.0, 13.5)
        require(slip_ratio and statistics.median(slip_ratio) < 0.85,
                f"odometry speed did not show the configured slip loss: n={len(slip_ratio)}, "
                f"median odometry/cmd_vel={statistics.median(slip_ratio) if slip_ratio else 'none'}")
        severe_commands = [v for stamp, v, _ in probe.commands if 21.0 <= stamp < 27.5]
        require(severe_commands and any(v <= 1e-6 for v in severe_commands),
                "severe sensor loss did not produce zero-speed controller commands")
        topics = {name for name, _ in probe.get_topic_names_and_types()}
        require(not any("ground_truth" in name or "groundtruth" in name for name in topics),
                "a ground-truth topic was visible in the ROS graph")
        require(("odom", "base_link") in probe.dynamic_tf,
                "filtered odometry did not own the dynamic odom -> base_link transform")
        require(("map", "odom") in probe.static_tf and ("base_link", "imu") in probe.static_tf and
                ("base_link", "lidar") in probe.static_tf,
                "static TF ownership is missing map/odom or sensor transforms")
        require(len(probe.dynamic_tf_publishers) == 1,
                f"expected one dynamic TF publisher, got {sorted(probe.dynamic_tf_publishers)}")
        print(f"ROS fault validation PASS: /clock {min(probe.clock):.3f}..{max(probe.clock):.3f} sim s; "
              f"filtered odometry={len(probe.filtered_odometry)} samples, health states="
              f"{sorted(set(code for _, code in probe.estimator_health))}, "
              f"covariance growth={statistics.median(before_loss):.3f}->{statistics.median(during_loss):.3f}, "
              f"GNSS gates={max((int(v[7]) for v in probe.estimator_diagnostics), default=0)}; "
              "TF map->odom static, odom->base_link dynamic from bridge; "
              f"GNSS dropout fixes={sum(5.0 <= t < 8.0 for t in probe.gnss)}; "
              f"LiDAR range={min(degraded_scans):.1f} m; odometry measured/command-model speed="
              f"{statistics.median(slip_ratio):.3f} vs nominal {statistics.median(nominal_ratio):.3f}; "
              f"severe zero-speed commands={sum(v <= 1e-6 for v in severe_commands)}")
        print("Bridge summary:\n" + bridge_output.strip())
    finally:
        if bridge is not None and bridge.poll() is None:
            terminate_process_tree(bridge)
        if controller.poll() is None:
            terminate_process_tree(controller)
        probe.destroy_node()
        rclpy.shutdown()
        controller_log.close()
        bridge_log.close()


if __name__ == "__main__":
    main()
