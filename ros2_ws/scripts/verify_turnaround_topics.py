#!/usr/bin/env python3
"""Check the live RampLab turnaround observation topics and final task state."""

import argparse
import json
import time

import rclpy
from rclpy.node import Node
from std_msgs.msg import String


class Probe(Node):
    def __init__(self):
        super().__init__("ramplab_turnaround_probe")
        self.states = []
        self.events = []
        self.create_subscription(String, "/ramplab/turnaround/state", self.on_state, 10)
        self.create_subscription(String, "/ramplab/turnaround/events", self.on_event, 100)

    def on_state(self, message):
        self.states.append(json.loads(message.data))

    def on_event(self, message):
        self.events.append(json.loads(message.data))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--duration", type=float, default=20.0)
    args = parser.parse_args()
    rclpy.init()
    node = Probe()
    deadline = time.monotonic() + args.duration
    try:
        while time.monotonic() < deadline:
            rclpy.spin_once(node, timeout_sec=0.1)
            if node.states and node.states[-1]["turnarounds"]:
                turnaround = node.states[-1]["turnarounds"][0]
                if turnaround["state"] == "Departed":
                    break
        if not node.states:
            raise RuntimeError("no turnaround state was received")
        final_state = node.states[-1]
        if not final_state["turnarounds"]:
            raise RuntimeError("state did not contain a turnaround")
        turnaround = final_state["turnarounds"][0]
        if turnaround["state"] not in ("ReadyForDeparture", "Departed"):
            raise RuntimeError(f"turnaround stopped in {turnaround['state']}")
        if turnaround["completed_task_count"] != len(turnaround["tasks"]):
            raise RuntimeError("not all turnaround tasks completed")
        sequences = [event["sequence"] for event in node.events]
        if sequences != sorted(set(sequences)):
            raise RuntimeError("turnaround event sequence was not strictly increasing")
        if not any(event["type"] == "TurnaroundTaskStarted" and event["vehicle_id"] is not None for event in node.events):
            raise RuntimeError("no mobile resource task assignment was observed")
        print(json.dumps({
            "state_samples": len(node.states),
            "event_count": len(node.events),
            "turnaround_id": turnaround["turnaround_id"],
            "state": turnaround["state"],
            "tasks_completed": turnaround["completed_task_count"],
            "tasks_total": len(turnaround["tasks"]),
        }, sort_keys=True))
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    main()
