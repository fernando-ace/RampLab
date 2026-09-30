#!/usr/bin/env python3
"""Run two namespaced bridges and verify command/sensor traffic isolation."""

import os
from pathlib import Path
import subprocess
import tempfile
import time

import rclpy
from geometry_msgs.msg import Twist
from nav_msgs.msg import Odometry
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy, DurabilityPolicy
from sensor_msgs.msg import LaserScan
from tf2_msgs.msg import TFMessage


class IsolationProbe(Node):
    def __init__(self):
        super().__init__("ramplab_multi_vehicle_isolation_probe")
        sensor_qos = QoSProfile(depth=10, reliability=ReliabilityPolicy.BEST_EFFORT)
        command_qos = QoSProfile(depth=10, reliability=ReliabilityPolicy.BEST_EFFORT,
                                 durability=DurabilityPolicy.VOLATILE)
        static_qos = QoSProfile(depth=10, reliability=ReliabilityPolicy.RELIABLE,
                                durability=DurabilityPolicy.TRANSIENT_LOCAL)
        self.odom = {name: [] for name in ("alpha", "beta")}
        self.filtered_odom = {name: 0 for name in ("alpha", "beta")}
        self.scans = {name: 0 for name in ("alpha", "beta")}
        self.frames = {name: set() for name in ("alpha", "beta")}
        self.command_subscribers = {name: 0 for name in ("alpha", "beta")}
        self.global_clock_publishers = 0
        self.transforms = set()
        self.alpha_command = self.create_publisher(Twist, "/ramplab/fleet_alpha/cmd_vel", command_qos)
        self.beta_command = self.create_publisher(Twist, "/ramplab/fleet_beta/cmd_vel", command_qos)
        for name, topic_name in (("alpha", "fleet_alpha"), ("beta", "fleet_beta")):
            self.create_subscription(Odometry, f"/ramplab/{topic_name}/odom",
                                     lambda msg, key=name: self.odom[key].append(
                                         (msg.pose.pose.position.x, msg.pose.pose.position.y)), sensor_qos)
            self.create_subscription(Odometry, f"/ramplab/{topic_name}/filtered_odom",
                                     lambda _msg, key=name: self.count_filtered(key), sensor_qos)
            self.create_subscription(LaserScan, f"/ramplab/{topic_name}/scan",
                                     lambda msg, key=name: self.count_scan(key, msg.header.frame_id), sensor_qos)
        self.create_subscription(TFMessage, "/tf", self.on_tf, 100)
        self.create_subscription(TFMessage, "/tf_static", self.on_tf, static_qos)
        self.command_timer = self.create_timer(0.05, self.publish_commands)

    def count_scan(self, key, frame_id):
        self.scans[key] += 1
        self.frames[key].add(frame_id)

    def count_filtered(self, key):
        self.filtered_odom[key] += 1

    def on_tf(self, msg):
        self.transforms.update((tf.header.frame_id, tf.child_frame_id) for tf in msg.transforms)

    def publish_commands(self):
        moving = Twist()
        moving.linear.x = 1.5
        self.alpha_command.publish(moving)
        self.beta_command.publish(Twist())


def terminate_process(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def main():
    # This local Fast DDS 3.6.2 Windows build crashes in fastdds-3.6.dll when
    # a second participant starts with the default shared-memory transport.
    # UDPv4 keeps this test on one ROS domain while avoiding that local SHM fault.
    os.environ["FASTDDS_BUILTIN_TRANSPORTS"] = "UDPv4"
    root = Path(__file__).resolve().parents[2]
    scenario = root / "scenarios" / "autonomy_tug.yaml"
    bridge_executable = root / "ros2_ws" / "install" / "lib" / "ramplab_ros2_bridge" / "ramplab_ros2_bridge.exe"
    if not bridge_executable.is_file():
        raise RuntimeError(f"built ROS bridge executable not found: {bridge_executable}")
    logs = [tempfile.TemporaryFile(mode="w+") for _ in range(2)]
    processes = []
    rclpy.init()
    probe = IsolationProbe()
    try:
        for index, (vehicle, camera_port) in enumerate((("fleet_alpha", 39110), ("fleet_beta", 39112))):
            args = [
                str(bridge_executable),
                "--scenario", str(scenario), "--seed", "42", "--realtime-factor", "1",
                "--max-sim-seconds", "12", "--vehicle-id", vehicle,
                "--camera-port", str(camera_port), "--lidar-port", str(camera_port + 1),
            ]
            if index > 0:
                args.append("--no-global-clock")
            processes.append(subprocess.Popen(args, stdout=logs[index], stderr=subprocess.STDOUT,
                                              env=os.environ.copy()))
            time.sleep(0.5)

        deadline = time.monotonic() + 30.0
        while time.monotonic() < deadline and any(process.poll() is None for process in processes):
            rclpy.spin_once(probe, timeout_sec=0.025)
            for name, topic in (("alpha", "/ramplab/fleet_alpha/cmd_vel"),
                                ("beta", "/ramplab/fleet_beta/cmd_vel")):
                probe.command_subscribers[name] = max(
                    probe.command_subscribers[name], len(probe.get_subscriptions_info_by_topic(topic)))
            probe.global_clock_publishers = max(
                probe.global_clock_publishers, len(probe.get_publishers_info_by_topic("/clock")))
        for _ in range(20):
            rclpy.spin_once(probe, timeout_sec=0.025)

        outputs = []
        failures = []
        for index, log in enumerate(logs):
            log.seek(0)
            outputs.append(log.read())
            if processes[index].poll() is None:
                failures.append(f"{('fleet_alpha', 'fleet_beta')[index]} bridge exceeded the 30 s deadline")
            if processes[index].returncode != 0:
                failures.append(f"{('fleet_alpha', 'fleet_beta')[index]} bridge exited {processes[index].returncode}:\n{outputs[-1]}")
        if failures:
            raise RuntimeError("\n".join(failures))

        def displacement(samples):
            if len(samples) < 2:
                return 0.0
            first, last = samples[0], samples[-1]
            return ((last[0] - first[0]) ** 2 + (last[1] - first[1]) ** 2) ** 0.5

        alpha_distance = displacement(probe.odom["alpha"])
        beta_distance = displacement(probe.odom["beta"])
        if min(probe.scans.values()) < 20:
            raise RuntimeError(f"both namespaces must publish sensors; scans={probe.scans}")
        if alpha_distance < 1.5:
            raise RuntimeError(f"fleet_alpha command did not move its bridge enough: {alpha_distance:.3f} m")
        if beta_distance > 0.5:
            raise RuntimeError(f"fleet_beta moved despite receiving only zero commands: {beta_distance:.3f} m")
        if probe.command_subscribers != {"alpha": 1, "beta": 1}:
            raise RuntimeError(f"expected one isolated bridge command subscriber per namespace, got {probe.command_subscribers}")
        if probe.global_clock_publishers != 1:
            raise RuntimeError(f"expected one owner for global /clock, got {probe.global_clock_publishers}")
        expected_tf = {("fleet_alpha/odom", "fleet_alpha/base_link"),
                       ("fleet_beta/odom", "fleet_beta/base_link"),
                       ("fleet_alpha/base_link", "fleet_alpha/lidar"),
                       ("fleet_beta/base_link", "fleet_beta/lidar")}
        missing_tf = expected_tf - probe.transforms
        if missing_tf:
            raise RuntimeError(f"vehicle-specific TF frame chains were not observed: {sorted(missing_tf)}; "
                               f"filtered odometry={probe.filtered_odom}; observed TF={sorted(probe.transforms)}")
        print("ROS multi-vehicle isolation PASS")
        print(f"fleet_alpha: odom={len(probe.odom['alpha'])}, scans={probe.scans['alpha']}, displacement={alpha_distance:.3f} m")
        print(f"fleet_beta:  odom={len(probe.odom['beta'])}, scans={probe.scans['beta']}, displacement={beta_distance:.3f} m")
        print(f"filtered odometry samples: {probe.filtered_odom}")
        print(f"global /clock publishers: {probe.global_clock_publishers}; command subscribers: {probe.command_subscribers}")
        print(f"namespaced TF edges: {sorted(expected_tf)}")
        for output in outputs:
            for line in output.splitlines():
                if "Published messages:" in line:
                    print(line)
    finally:
        for process in processes:
            terminate_process(process)
        probe.destroy_node()
        rclpy.shutdown()
        for log in logs:
            log.close()


if __name__ == "__main__":
    main()
