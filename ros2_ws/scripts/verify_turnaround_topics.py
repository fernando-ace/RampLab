#!/usr/bin/env python3
"""Check the live RampLab turnaround observation topics and final task state."""

import argparse
import json
from pathlib import Path
import subprocess
import time

import rclpy
from rclpy.node import Node
from rclpy.qos import DurabilityPolicy, QoSProfile, ReliabilityPolicy
from std_msgs.msg import String


class Probe(Node):
    def __init__(self):
        super().__init__("ramplab_turnaround_probe")
        self.states = []
        self.events = []
        self.errors = []
        qos = QoSProfile(depth=1000, reliability=ReliabilityPolicy.RELIABLE,
                         durability=DurabilityPolicy.TRANSIENT_LOCAL)
        self.create_subscription(String, "/ramplab/turnaround/state", self.on_state, qos)
        self.create_subscription(String, "/ramplab/turnaround/events", self.on_event, qos)

    def on_state(self, message):
        try:
            self.states.append(json.loads(message.data))
        except (json.JSONDecodeError, TypeError) as error:
            self.errors.append(f"state JSON: {error}; payload={message.data[:240]}")

    def on_event(self, message):
        try:
            self.events.append(json.loads(message.data))
        except (json.JSONDecodeError, TypeError) as error:
            self.errors.append(f"event JSON: {error}; payload={message.data[:240]}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--duration", type=float, default=20.0)
    parser.add_argument("--min-aircraft", type=int, default=1)
    parser.add_argument("--bridge-executable")
    parser.add_argument("--scenario", default="scenarios/turnaround_normal.yaml")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--require-outage-reassignment", action="store_true",
                        help="require the event stream to show an outage and replacement completing the affected task")
    parser.add_argument("--require-surface", action="store_true",
                        help="require pushback, taxi, traffic waiting, and runway events in the live feed")
    parser.add_argument("--require-reroute", action="store_true",
                        help="also require a surface closure reroute")
    parser.add_argument("--require-mixed-traffic", action="store_true",
                        help="require the canonical arrivals, departures, and shared-runway event sequence")
    parser.add_argument("--require-integrated-lifecycle", action="store_true",
                        help="require an arrival_turnaround aircraft to complete taxi-in, gate service, taxi-out, and departure under one ID")
    args = parser.parse_args()
    rclpy.init()
    node = Probe()
    bridge = None
    deadline = time.monotonic() + args.duration
    all_departed_since = None
    try:
        if args.bridge_executable:
            rclpy.spin_once(node, timeout_sec=0.2)
            bridge = subprocess.Popen([
                args.bridge_executable, "--scenario", args.scenario, "--seed", str(args.seed)
            ], cwd=Path(__file__).resolve().parents[2])
            deadline = time.monotonic() + args.duration
        while time.monotonic() < deadline:
            rclpy.spin_once(node, timeout_sec=0.1)
            if node.states and node.states[-1]["turnarounds"]:
                state = node.states[-1]
                turnarounds = state["turnarounds"]
                mixed_complete = not args.require_mixed_traffic or all(
                    item["surface_state"] in {"Arrived", "Departed"}
                    for item in state.get("aircraft_operations", [])
                )
                if len(turnarounds) >= args.min_aircraft and mixed_complete and all(
                    item["state"] == "Departed" for item in turnarounds
                ):
                    if all_departed_since is None:
                        all_departed_since = time.monotonic()
                    elif time.monotonic() - all_departed_since >= 1.0:
                        break
        if not node.states:
            detail = "; ".join(node.errors[:3])
            raise RuntimeError("no turnaround state was received" + (f": {detail}" if detail else ""))
        final_state = node.states[-1]
        if not final_state["turnarounds"]:
            raise RuntimeError("state did not contain a turnaround")
        turnarounds = final_state["turnarounds"]
        if len(turnarounds) < args.min_aircraft:
            raise RuntimeError(f"expected at least {args.min_aircraft} aircraft, received {len(turnarounds)}")
        for turnaround in turnarounds:
            if turnaround["state"] != "Departed":
                raise RuntimeError(f"{turnaround['turnaround_id']} stopped in {turnaround['state']}")
            if turnaround["completed_task_count"] != len(turnaround["tasks"]):
                raise RuntimeError(f"{turnaround['turnaround_id']} has unfinished required tasks")
        aircraft_ids = {item["aircraft_id"] for item in turnarounds}
        gate_ids = {item["gate_id"] for item in turnarounds}
        if len(aircraft_ids) != len(turnarounds) or len(gate_ids) != len(turnarounds):
            raise RuntimeError("aircraft or gate identity was duplicated in the final operation snapshot")
        surface_summary = None
        if args.require_surface:
            required_types = {
                "SurfacePushbackStarted", "SurfacePushbackCompleted", "SurfaceTaxiRouteAssigned",
                "SurfaceWaitingForTraffic", "SurfaceRunwayQueueEntered", "SurfaceRunwayClearance",
                "AircraftDeparted",
            }
            observed_types = {event["type"] for event in node.events}
            missing = sorted(required_types - observed_types)
            if missing:
                raise RuntimeError(f"surface observation missed required events: {missing}")
            if not all(item.get("surface_state") == "Departed" and item.get("surface_route_node_ids")
                       for item in turnarounds):
                raise RuntimeError("final ROS state did not expose completed aircraft surface routes")
            reroutes = [event for event in node.events if event["type"] == "SurfaceRerouted"]
            if args.require_reroute and not reroutes:
                raise RuntimeError("surface closure reroute event was not observed")
            surface_summary = {
                "pushbacks": sum(event["type"] == "SurfacePushbackStarted" for event in node.events),
                "traffic_waits": sum(event["type"] == "SurfaceWaitingForTraffic" for event in node.events),
                "runway_queue_entries": sum(event["type"] == "SurfaceRunwayQueueEntered" for event in node.events),
                "reroutes": len(reroutes),
            }
        mixed_summary = None
        if args.require_mixed_traffic:
            operations = final_state.get("aircraft_operations", [])
            arrivals = [item for item in operations if item.get("operation_type") == "arrival"]
            departures = [item for item in operations if item.get("operation_type") == "departure"]
            if len(arrivals) < 2 or len(departures) < 3:
                raise RuntimeError(f"expected at least 2 arrivals and 3 departures; received {len(arrivals)} and {len(departures)}")
            if any(item.get("surface_state") != "Arrived" or item.get("arrival_gate_time_seconds") is None for item in arrivals):
                raise RuntimeError("not all inbound aircraft reached their assigned gates")
            if any(item.get("surface_state") != "Departed" or item.get("actual_departure_seconds") is None for item in departures):
                raise RuntimeError("not all outbound aircraft departed")
            required = {"RunwayRequest", "RunwayGrant", "RunwayOccupied", "RunwayReleased",
                        "ArrivalRunwayExit", "ArrivalTaxiInStarted", "ArrivalAtGate"}
            observed = {event.get("type") for event in node.events}
            missing = sorted(required - observed)
            if missing:
                raise RuntimeError(f"mixed runway event stream missed {missing}")
            grants = [event for event in node.events if event.get("type") == "RunwayGrant"]
            arrival_ids = {item["aircraft_id"] for item in arrivals}
            departure_ids = {item["aircraft_id"] for item in departures}
            if not any(event.get("aircraft_id") in arrival_ids for event in grants) or not any(
                event.get("aircraft_id") in departure_ids for event in grants
            ):
                raise RuntimeError("both operation types were not granted shared runway access")
            mixed_summary = {
                "arrivals_at_gates": len(arrivals), "departures_completed": len(departures),
                "runway_grants": len(grants), "runway_owner_id": final_state.get("runway_owner_id"),
                "runway_queue_ids": final_state.get("runway_queue_aircraft_ids", []),
            }
        lifecycle_summary = None
        if args.require_integrated_lifecycle:
            integrated = [item for item in final_state.get("aircraft_operations", [])
                          if item.get("operation_type") == "arrival_turnaround"]
            complete = [item for item in integrated if item.get("surface_state") == "Departed" and
                        item.get("actual_arrival_seconds") is not None and
                        item.get("arrival_gate_time_seconds") is not None and
                        item.get("actual_departure_seconds") is not None]
            if not complete:
                raise RuntimeError("no integrated arrival-turnaround aircraft completed arrival, gate, and departure")
            aircraft_id = complete[0]["aircraft_id"]
            lifecycle_types = [event.get("type") for event in node.events
                               if event.get("aircraft_id") == aircraft_id]
            required_lifecycle = ["ArrivalTaxiInStarted", "ArrivalAtGate", "TurnaroundStarted",
                                  "TurnaroundReadyForDeparture", "PushbackTaxiOutStarted",
                                  "SurfaceRunwayQueueEntered", "AircraftDeparted"]
            cursor = 0
            for event_type in lifecycle_types:
                if cursor < len(required_lifecycle) and event_type == required_lifecycle[cursor]:
                    cursor += 1
            if cursor != len(required_lifecycle):
                raise RuntimeError(f"aircraft {aircraft_id} lifecycle events were incomplete or out of order: {lifecycle_types}")
            lifecycle_summary = {
                "aircraft_id": aircraft_id,
                "gate_id": complete[0].get("gate_id"),
                "actual_arrival_seconds": complete[0]["actual_arrival_seconds"],
                "arrival_gate_time_seconds": complete[0]["arrival_gate_time_seconds"],
                "actual_departure_seconds": complete[0]["actual_departure_seconds"],
            }
        sequences = [event["sequence"] for event in node.events]
        if sequences != sorted(set(sequences)):
            raise RuntimeError("turnaround event sequence was not strictly increasing")
        if not any(event["type"] == "TurnaroundTaskStarted" and event["vehicle_id"] is not None for event in node.events):
            sample = [{"type": event.get("type"), "vehicle_id": event.get("vehicle_id")}
                      for event in node.events[:8]]
            raise RuntimeError(f"no mobile resource task assignment was observed; events={len(node.events)} sample={sample}")
        completed = [event["task_id"] for event in node.events if event["type"] == "TurnaroundTaskCompleted"]
        task_ids = [task["task_id"] for item in turnarounds for task in item["tasks"]]
        if len(completed) != len(set(completed)) or set(completed) != set(task_ids):
            raise RuntimeError(f"event stream has duplicate or missing task completions; completed={completed} expected={task_ids}")
        outage_summary = None
        if args.require_outage_reassignment:
            outage = next((event for event in node.events
                           if event["type"] == "TurnaroundVehicleUnavailable" and event["task_id"] is not None), None)
            if outage is None:
                raise RuntimeError("no vehicle outage tied to active service work was observed")
            reassignment = next((event for event in node.events
                                 if event["type"] == "TurnaroundTaskReassigned" and
                                 event["task_id"] == outage["task_id"] and
                                 event["sequence"] > outage["sequence"]), None)
            if reassignment is None:
                raise RuntimeError("the outaged task was not reassigned after the outage event")
            original = next((event for event in node.events
                             if event["type"] == "TurnaroundTaskDispatched" and
                             event["task_id"] == outage["task_id"] and
                             event["sequence"] < outage["sequence"] and
                             event["vehicle_id"] == outage["vehicle_id"]), None)
            started = next((event for event in node.events
                            if event["type"] == "TurnaroundTaskStarted" and
                            event["task_id"] == outage["task_id"] and
                            event["sequence"] > reassignment["sequence"] and
                            event["vehicle_id"] == reassignment["vehicle_id"]), None)
            completed_event = next((event for event in node.events
                                    if event["type"] == "TurnaroundTaskCompleted" and
                                    event["task_id"] == outage["task_id"] and started is not None and
                                    event["sequence"] > started["sequence"]), None)
            if original is None or started is None or completed_event is None:
                raise RuntimeError("original assignment, replacement start, or replacement completion was missing")
            affected_task = next((task for item in turnarounds for task in item["tasks"]
                                  if task["task_id"] == outage["task_id"]), None)
            if affected_task is None or affected_task["reassignments"] < 1 or affected_task["state"] != "Completed":
                raise RuntimeError("final state does not show the reassigned task completed")
            outage_summary = {
                "vehicle_id": outage["vehicle_id"],
                "time_seconds": outage["time_seconds"],
                "task_id": outage["task_id"],
                "original_vehicle_id": original["vehicle_id"],
                "replacement_vehicle_id": reassignment["vehicle_id"],
                "reassignment_time_seconds": reassignment["time_seconds"],
                "replacement_started_seconds": started["time_seconds"],
                "completed_seconds": completed_event["time_seconds"],
            }
        print(json.dumps({
            "state_samples": len(node.states),
            "event_count": len(node.events),
            "aircraft_count": len(turnarounds),
            "gate_count": len(gate_ids),
            "task_completion_events": len(completed),
            "outage_reassignment": outage_summary,
            "surface_operations": surface_summary,
            "mixed_traffic": mixed_summary,
            "integrated_lifecycle": lifecycle_summary,
            "aircraft": [{
                "turnaround_id": item["turnaround_id"],
                "state": item["state"],
                "tasks_completed": item["completed_task_count"],
                "tasks_total": len(item["tasks"]),
            } for item in turnarounds],
        }, sort_keys=True))
    finally:
        node.destroy_node()
        rclpy.shutdown()
        if bridge is not None:
            bridge.terminate()
            try:
                bridge.wait(timeout=5)
            except subprocess.TimeoutExpired:
                bridge.kill()
                bridge.wait()


if __name__ == "__main__":
    main()
