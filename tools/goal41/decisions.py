"""Small operator-facing summaries over the complete simulator decision trace."""
from __future__ import annotations

from typing import Any


def summarize_decisions(trace: dict[str, Any], events: list[dict[str, Any]] | None = None,
                        scenario: dict[str, Any] | None = None) -> list[dict[str, Any]]:
    summaries = []
    for item in trace.get("decisions", []):
        if not isinstance(item, dict):
            continue
        action = str(item.get("action", ""))
        coordination = item.get("coordination", {})
        objective = item.get("objective", {})
        candidates = item.get("candidates", [])
        selected_index = item.get("selected_candidate")
        selected = candidates[selected_index] if isinstance(selected_index, int) and 0 <= selected_index < len(candidates) else None
        trigger = str(coordination.get("replan_trigger", ""))
        aircraft = str(item.get("agent_id", "aircraft"))
        objective_type = str(objective.get("type", "movement")).replace("_", " ").lower()
        reason = str(item.get("explanation") or item.get("reason") or "The simulator applied its configured movement policy.")
        if "closure" in trigger.casefold():
            message = f"{aircraft} selected an available taxi route after a route closure invalidated its plan."
        elif action == "Wait":
            message = f"{aircraft} waited for the simulator's movement constraints to clear before continuing."
        elif action == "PlanRoute":
            message = f"{aircraft} selected a taxi route for its {objective_type} movement."
        elif action == "CompleteObjective":
            message = f"{aircraft} completed its {objective_type} movement objective."
        elif action == "FailObjective":
            message = f"{aircraft} could not complete its {objective_type} movement objective."
        else:
            continue
        summaries.append({
            "event_id": item.get("event_id"),
            "time_seconds": item.get("time_seconds"),
            "aircraft_id": aircraft,
            "action": action,
            "what_changed": message,
            "why": reason,
            "replan_trigger": trigger,
            "reason_code": item.get("reason"),
            "options_considered": len(candidates),
            "selected_option": selected.get("kind") if isinstance(selected, dict) else None,
            "selected_route_distance_m": selected.get("predicted_taxi_distance_m") if isinstance(selected, dict) else None,
            "selected_route_time_seconds": selected.get("predicted_taxi_seconds") if isinstance(selected, dict) else None,
            "selected_estimated_system_cost_seconds": selected.get("estimated_system_cost_seconds") if isinstance(selected, dict) else None,
            "safety_constraints": coordination.get("safety_constraints"),
            "result": item.get("result"),
        })
    if events:
        disruption = next((item for item in (scenario or {}).get("turnaround_disruptions", [])
                           if isinstance(item, dict)), None)
        base_duration = None
        if disruption:
            aircraft = next((item for item in (scenario or {}).get("aircraft", [])
                             if item.get("id") == disruption.get("aircraft")), {})
            task = next((item for item in aircraft.get("service_tasks", [])
                         if item.get("id") == disruption.get("task")), {})
            base_duration = task.get("duration_seconds")
        for event in events:
            if event.get("type") != "TurnaroundDisruptionDetected":
                continue
            duration = event.get("task_duration_seconds")
            added = duration - base_duration if isinstance(duration, (int, float)) and isinstance(base_duration, (int, float)) else None
            service = str(event.get("service", "service"))
            aircraft_name = str(event.get("aircraft", event.get("aircraft_id", "Aircraft")))
            service_label = f"{service.lower()} service"
            summaries.append({
                "event_id": event.get("sequence"),
                "time_seconds": event.get("time_seconds"),
                "aircraft_id": aircraft_name,
                "action": "TurnaroundScheduleAdjusted",
                "what_changed": (f"{aircraft_name} {service_label} was extended by {added:g} seconds."
                                 if isinstance(added, (int, float)) else
                                 f"{aircraft_name} {service_label} duration changed to {duration} seconds."),
                "why": (f"A controlled service-delay event changed the planned {service.lower()} task from "
                        f"{base_duration} to {duration} seconds. The simulator recomputed turnaround readiness."),
                "replan_trigger": "SERVICE_DURATION_DISRUPTION",
                "reason_code": "TURNAROUND_DISRUPTION_DETECTED",
                "options_considered": 1,
                "selected_option": f"{duration} second task duration",
                "selected_route_distance_m": None,
                "selected_route_time_seconds": None,
                "selected_estimated_system_cost_seconds": None,
                "safety_constraints": "Aircraft movement and collision checks remain authoritative in the simulator.",
                "result": "TURNAROUND_ESTIMATE_RECOMPUTED",
            })
    return summaries
