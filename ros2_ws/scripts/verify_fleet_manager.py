#!/usr/bin/env python3
"""Probe the single-process ROS fleet adapter and its identity-keyed state."""
import argparse
import json
import subprocess
import time
from pathlib import Path

import rclpy
from rclpy.node import Node
from std_msgs.msg import String


class Probe(Node):
    def __init__(self):
        super().__init__("ramplab_fleet_manager_probe")
        self.states = []
        self.events = []
        self.create_subscription(String, "/ramplab/fleet/state", self.on_state, 10)
        self.create_subscription(String, "/ramplab/fleet/traffic_events", self.on_event, 100)

    def on_state(self, message):
        self.states.append(json.loads(message.data))

    def on_event(self, message):
        self.events.append(json.loads(message.data))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--scenario", default="scenarios/autonomy_fleet.yaml")
    parser.add_argument("--seconds", type=float, default=20.0)
    parser.add_argument("--require-retreat", action="store_true")
    args = parser.parse_args()
    executable = Path(__file__).resolve().parents[1] / "install/lib/ramplab_ros2_bridge/ramplab_ros2_fleet_bridge.exe"
    if not executable.exists():
        raise RuntimeError(f"fleet bridge executable not found: {executable}")
    process = subprocess.Popen([str(executable), "--scenario", args.scenario], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    rclpy.init()
    probe = Probe()
    try:
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            rclpy.spin_once(probe, timeout_sec=0.05)
        if len(probe.states) < 2:
            raise RuntimeError(f"too few fleet state messages: {len(probe.states)}")
        latest = probe.states[-1]
        vehicles = latest["vehicles"]
        identities = [vehicle["vehicle_id"] for vehicle in vehicles]
        expected = {"tug_01", "tug_02", "tug_03"}
        if set(identities) != expected or len(set(identities)) != len(identities):
            raise RuntimeError(f"vehicle identity mismatch: {identities}")
        if not all("x_m" in vehicle and "y_m" in vehicle and "speed_mps" in vehicle for vehicle in vehicles):
            raise RuntimeError("fleet state omitted per-vehicle kinematics")
        stamps = [sample["simulation_time_s"] for sample in probe.states]
        if stamps[-1] <= stamps[0]:
            raise RuntimeError(f"fleet simulation clock did not advance: {stamps[0]}..{stamps[-1]}")
        if not probe.events:
            raise RuntimeError("coordinated traffic event topic published no events")
        if args.require_retreat:
            kinds = [event["kind"] for event in probe.events]
            required = ("retreat_selected", "retreat_started", "retreat_completed",
                        "retreat_resource_released", "mission_resumed")
            missing = [kind for kind in required if kind not in kinds]
            if missing:
                raise RuntimeError(f"retreat lifecycle events missing: {missing}")
            started = next(event for event in probe.events if event["kind"] == "retreat_started")
            completed = next(event for event in probe.events
                             if event["kind"] == "retreat_completed" and event["vehicle_id"] == started["vehicle_id"])
            distance = ((completed["x_m"] - started["x_m"]) ** 2 +
                        (completed["y_m"] - started["y_m"]) ** 2) ** 0.5
            if distance < 1.0:
                raise RuntimeError(f"retreat did not physically clear the resource: {distance:.3f} m")
            if not all("recovery_state" in vehicle and "retreat_progress_m" in vehicle for vehicle in vehicles):
                raise RuntimeError("fleet state omitted retreat state fields")
            print(f"ROS retreat probe PASS: vehicle={started['vehicle_id']}, "
                  f"resource={started['resource']}, physical_retreat={distance:.2f} m, "
                  f"lifecycle={' -> '.join(required)}")
        print(f"ROS fleet probe PASS: {len(probe.states)} state samples, ids={sorted(identities)}, "
              f"simulation={stamps[0]:.2f}..{stamps[-1]:.2f} s, traffic_events={len(probe.events)}")
    finally:
        probe.destroy_node()
        rclpy.shutdown()
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        if process.returncode not in (0, -15, 1):
            output = process.stdout.read().decode(errors="replace") if process.stdout else ""
            print(output)


if __name__ == "__main__":
    main()
