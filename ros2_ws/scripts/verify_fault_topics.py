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


def seconds(stamp: Time) -> float:
    return float(stamp.sec) + float(stamp.nanosec) / 1.0e9


class Probe(Node):
    def __init__(self):
        super().__init__("ramplab_fault_topic_probe")
        sensor_qos = QoSProfile(depth=10, reliability=ReliabilityPolicy.BEST_EFFORT)
        reliable_qos = QoSProfile(depth=10, reliability=ReliabilityPolicy.RELIABLE)
        command_qos = QoSProfile(depth=10, reliability=ReliabilityPolicy.BEST_EFFORT,
                                 durability=DurabilityPolicy.VOLATILE)
        self.sim_time = 0.0
        self.clock = []
        self.gnss = []
        self.scans = []
        self.odometry = []
        self.commands = []
        self.expected_speeds = []
        self.expected_speed = 0.0
        self.last_command_time = None
        self.create_subscription(Clock, "/clock", self.on_clock, reliable_qos)
        self.create_subscription(NavSatFix, "/ramplab/tug1/gnss", self.on_gnss, sensor_qos)
        self.create_subscription(LaserScan, "/ramplab/tug1/scan", self.on_scan, sensor_qos)
        self.create_subscription(Odometry, "/ramplab/tug1/odom", self.on_odom, sensor_qos)
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
        require(max(probe.clock) >= args.max_sim_seconds - 0.05, "simulation clock did not reach requested limit")
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
        print(f"ROS fault validation PASS: /clock {min(probe.clock):.3f}..{max(probe.clock):.3f} sim s; "
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
