#!/usr/bin/env python3
"""Measure live RampLab ROS 2 topics and verify simulation-time ordering."""

import argparse
import math
import sys
import time

import rclpy
from geometry_msgs.msg import Twist
from nav_msgs.msg import Odometry, Path
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy, DurabilityPolicy, HistoryPolicy
from rosgraph_msgs.msg import Clock
from sensor_msgs.msg import Imu, LaserScan, NavSatFix


SENSORS = ("scan", "imu", "odom", "gnss")
EXPECTED_SIM_HZ = {"clock": 50.0, "scan": 10.0, "imu": 50.0, "odom": 20.0, "gnss": 5.0}


def stamp_seconds(stamp):
    return stamp.sec + stamp.nanosec / 1_000_000_000.0


class Probe(Node):
    def __init__(self):
        super().__init__("ramplab_topic_probe", namespace="/ramplab/validation")
        self.counts = {name: 0 for name in ("clock", *SENSORS, "route", "cmd_vel")}
        self.sim_stamps = {name: [] for name in ("clock", *SENSORS)}
        self.wall_stamps = {name: [] for name in self.counts}
        self.frames = {}
        self.qos_sensor = QoSProfile(
            history=HistoryPolicy.KEEP_LAST, depth=5,
            reliability=ReliabilityPolicy.BEST_EFFORT,
            durability=DurabilityPolicy.VOLATILE)
        self.qos_reliable = QoSProfile(
            history=HistoryPolicy.KEEP_LAST, depth=10,
            reliability=ReliabilityPolicy.RELIABLE,
            durability=DurabilityPolicy.VOLATILE)
        self.qos_route = QoSProfile(
            history=HistoryPolicy.KEEP_LAST, depth=1,
            reliability=ReliabilityPolicy.RELIABLE,
            durability=DurabilityPolicy.TRANSIENT_LOCAL)
        self.create_subscription(Clock, "/clock", self.clock_cb, self.qos_reliable)
        self.create_subscription(LaserScan, "/ramplab/tug1/scan", self.scan_cb, self.qos_sensor)
        self.create_subscription(Imu, "/ramplab/tug1/imu", self.imu_cb, self.qos_sensor)
        self.create_subscription(Odometry, "/ramplab/tug1/odom", self.odom_cb, self.qos_sensor)
        self.create_subscription(NavSatFix, "/ramplab/tug1/gnss", self.gnss_cb, self.qos_sensor)
        self.create_subscription(Path, "/ramplab/tug1/route", self.route_cb, self.qos_route)
        self.create_subscription(Twist, "/ramplab/tug1/cmd_vel", self.command_cb, self.qos_sensor)

    def record(self, name, stamp=None, frame=None):
        self.counts[name] += 1
        self.wall_stamps[name].append(time.perf_counter())
        if stamp is not None:
            self.sim_stamps[name].append(stamp)
        if frame is not None:
            self.frames[name] = frame

    def clock_cb(self, msg):
        self.record("clock", stamp_seconds(msg.clock))

    def scan_cb(self, msg):
        self.record("scan", stamp_seconds(msg.header.stamp), msg.header.frame_id)

    def imu_cb(self, msg):
        self.record("imu", stamp_seconds(msg.header.stamp), msg.header.frame_id)

    def odom_cb(self, msg):
        self.record("odom", stamp_seconds(msg.header.stamp), msg.header.frame_id + "/" + msg.child_frame_id)

    def gnss_cb(self, msg):
        self.record("gnss", stamp_seconds(msg.header.stamp), msg.header.frame_id)

    def route_cb(self, msg):
        self.record("route", frame=msg.header.frame_id)

    def command_cb(self, _msg):
        self.record("cmd_vel")


def measured_rate(values):
    return (len(values) - 1) / (values[-1] - values[0]) if len(values) > 1 and values[-1] > values[0] else 0.0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--duration", type=float, default=6.0)
    parser.add_argument("--factor", type=float, default=1.0,
                        help="bridge simulation pacing factor, used to explain wall-rate scaling")
    args = parser.parse_args()
    if args.duration <= 0 or args.factor <= 0:
        parser.error("duration and factor must be positive")

    rclpy.init()
    node = Probe()
    start = time.perf_counter()
    try:
        while time.perf_counter() - start < args.duration:
            rclpy.spin_once(node, timeout_sec=0.05)
    finally:
        node.destroy_node()
        rclpy.shutdown()

    failures = []
    for name, timestamps in node.sim_stamps.items():
        if len(timestamps) < 2:
            failures.append(f"{name}: fewer than two timestamped messages")
        if any(right <= left for left, right in zip(timestamps, timestamps[1:])):
            failures.append(f"{name}: simulation timestamps did not increase strictly")
    for name in ("route", "cmd_vel", *SENSORS, "clock"):
        if node.counts[name] == 0:
            failures.append(f"{name}: no live message observed")

    print(f"Probe duration: {time.perf_counter() - start:.2f} wall seconds; pacing factor: {args.factor:.2f}x")
    print("topic    count  sim_rate_hz  observed_wall_hz  frame")
    for name in ("clock", *SENSORS, "route", "cmd_vel"):
        stamps = node.sim_stamps.get(name, [])
        sim_rate = measured_rate(stamps) if stamps else math.nan
        wall_rate = measured_rate(node.wall_stamps[name])
        frame = node.frames.get(name, "-")
        expected = EXPECTED_SIM_HZ.get(name)
        print(f"{name:8s} {node.counts[name]:5d} {sim_rate:12.2f} {wall_rate:16.2f}  {frame}")
        if expected is not None and math.isfinite(sim_rate):
            if abs(sim_rate - expected) / expected > 0.35:
                failures.append(f"{name}: simulation rate {sim_rate:.2f} Hz differs from {expected:.2f} Hz")
    if failures:
        print("Validation failures:", file=sys.stderr)
        for failure in failures:
            print(f"- {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
