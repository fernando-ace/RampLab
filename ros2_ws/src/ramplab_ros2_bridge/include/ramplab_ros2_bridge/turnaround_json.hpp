#pragma once

#include "airside/core/event_stream.hpp"
#include "airside/integration/snapshot.hpp"

#include <format>
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace ramplab_ros2_bridge {

inline std::string json_escape(std::string_view value) {
  std::string output;
  for (const char character : value) {
    switch (character) {
      case '\\': output += "\\\\"; break;
      case '"': output += "\\\""; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default: output += character; break;
    }
  }
  return output;
}

inline std::string turnaround_state_json(const airside::SimulationSnapshot& snapshot) {
  std::string output = std::format("{{\"simulation_time_seconds\":{},\"runway_owner_id\":{},\"runway_queue_aircraft_ids\":[",
      snapshot.simulation_time.count(), snapshot.runway_owner ? std::to_string(snapshot.runway_owner->value()) : "null");
  for (std::size_t index = 0; index < snapshot.runway_queue.size(); ++index) {
    if (index) output += ',';
    output += std::to_string(snapshot.runway_queue[index].value());
  }
  output += "],\"turnarounds\":[";
  for (std::size_t index = 0; index < snapshot.turnarounds.size(); ++index) {
    const auto& turnaround = snapshot.turnarounds[index];
    if (index != 0) output += ',';
    std::size_t active = 0;
    std::size_t completed = 0;
    std::size_t pending = 0;
    for (const auto& task : turnaround.tasks) {
      active += task.status == airside::TaskStatus::Assigned || task.status == airside::TaskStatus::InProgress ? 1U : 0U;
      completed += task.status == airside::TaskStatus::Completed ? 1U : 0U;
      pending += task.status == airside::TaskStatus::Pending || task.status == airside::TaskStatus::Blocked ||
          task.status == airside::TaskStatus::Waiting ? 1U : 0U;
    }
    const auto aircraft = std::ranges::find(snapshot.aircraft, turnaround.aircraft,
                                             &airside::AircraftSnapshot::id);
    const auto delay = std::max<std::int64_t>(0,
        turnaround.estimated_ready_time.count() - turnaround.target_off_block.count());
    output += std::format(
        "{{\"turnaround_id\":\"{}\",\"aircraft_id\":{},\"gate_id\":{},\"state\":\"{}\","
        "\"scheduled_arrival_seconds\":{},\"actual_arrival_seconds\":{},\"scheduled_departure_seconds\":{},"
        "\"actual_departure_seconds\":{},\"departure_delay_seconds\":{},\"estimated_ready_time_seconds\":{},\"delay_seconds\":{},"
        "\"schedule_slack_seconds\":{},\"failure_reason\":\"{}\",\"active_task_count\":{},\"completed_task_count\":{},\"critical_path_task_ids\":[",
        json_escape(turnaround.turnaround_id), turnaround.aircraft.value(), turnaround.gate.value(),
        airside::to_string(turnaround.state), turnaround.scheduled_arrival.count(),
        turnaround.actual_arrival ? std::to_string(turnaround.actual_arrival->count()) : "null",
        turnaround.scheduled_departure.count(),
        aircraft != snapshot.aircraft.end() && aircraft->actual_departure
            ? std::to_string(aircraft->actual_departure->count()) : "null",
        turnaround.departure_delay ? std::to_string(turnaround.departure_delay->count()) : "null",
        turnaround.estimated_ready_time.count(), delay, turnaround.schedule_slack.count(),
        json_escape(turnaround.failure_reason), active, completed);
    for (std::size_t task_index = 0; task_index < turnaround.critical_path_tasks.size(); ++task_index) {
      if (task_index != 0) output += ',';
      output += std::to_string(turnaround.critical_path_tasks[task_index].value());
    }
    output += std::format(
        "],\"pending_task_count\":{},\"surface_state\":\"{}\",\"surface_wait_reason\":\"{}\","
        "\"surface_speed_mps\":{},\"surface_heading_rad\":{},\"surface_reroutes\":{},\"taxi_distance_m\":{},\"surface_wait_seconds\":{},"
        "\"surface_position_m\":{},\"surface_route_node_ids\":[",
        pending, json_escape(aircraft != snapshot.aircraft.end() ? aircraft->surface_state : std::string{}),
        json_escape(aircraft != snapshot.aircraft.end() ? aircraft->surface_wait_reason : std::string{}),
        aircraft != snapshot.aircraft.end() ? aircraft->surface_speed_mps : 0.0,
        aircraft != snapshot.aircraft.end() ? aircraft->surface_heading_rad : 0.0,
        aircraft != snapshot.aircraft.end() ? aircraft->surface_reroutes : 0U,
        aircraft != snapshot.aircraft.end() ? aircraft->taxi_distance_m : 0.0,
        aircraft != snapshot.aircraft.end() ? aircraft->surface_wait_duration.count() : 0,
        aircraft != snapshot.aircraft.end() && aircraft->surface_position_m
            ? std::format("{{\"x_m\":{},\"y_m\":{}}}", aircraft->surface_position_m->x_m, aircraft->surface_position_m->y_m) : "null");
    if (aircraft != snapshot.aircraft.end()) {
      for (std::size_t route_index = 0; route_index < aircraft->surface_route.size(); ++route_index) {
        if (route_index != 0) output += ',';
        output += std::to_string(aircraft->surface_route[route_index].value());
      }
    }
    output += "],\"tasks\":[";
    for (std::size_t task_index = 0; task_index < turnaround.tasks.size(); ++task_index) {
      const auto& task = turnaround.tasks[task_index];
      if (task_index != 0) output += ',';
      output += std::format(
          "{{\"task_id\":{},\"service_type\":\"{}\",\"state\":\"{}\",\"required_resource\":\"{}\",\"assigned_resource\":\"{}\",\"assigned_vehicle_id\":{},\"requested_at_seconds\":{},\"started_at_seconds\":{},\"completed_at_seconds\":{},\"latest_desirable_completion_seconds\":{},\"reassignments\":{}}}",
          task.id.value(), airside::to_string(task.type), airside::to_string(task.status),
          json_escape(task.required_resource), json_escape(task.assigned_resource),
          task.assigned_vehicle ? std::to_string(task.assigned_vehicle->value()) : "null",
          task.requested_at ? std::to_string(task.requested_at->count()) : "null",
          task.started_at ? std::to_string(task.started_at->count()) : "null",
          task.completed_at ? std::to_string(task.completed_at->count()) : "null",
          task.latest_desirable_completion ? std::to_string(task.latest_desirable_completion->count()) : "null",
          task.reassignments);
    }
    output += "]}";
  }
  output += "],\"aircraft_operations\":[";
  for (std::size_t index = 0; index < snapshot.aircraft.size(); ++index) {
    const auto& aircraft = snapshot.aircraft[index];
    if (index) output += ',';
    output += std::format(
        "{{\"aircraft_id\":{},\"flight_number\":\"{}\",\"operation_type\":\"{}\",\"surface_state\":\"{}\","
        "\"runway_request_time_seconds\":{},\"runway_clearance_time_seconds\":{},\"runway_wait_seconds\":{},"
        "\"runway_release_time_seconds\":{},\"gate_id\":{},\"gate_assigned_at_seconds\":{},"
        "\"actual_arrival_seconds\":{},\"arrival_gate_time_seconds\":{},\"gate_wait_seconds\":{},"
        "\"gate_occupancy_seconds\":{},\"pushback_started_at_seconds\":{},\"pushback_completed_at_seconds\":{},"
        "\"actual_departure_seconds\":{},\"arrival_taxi_started_at_seconds\":{},\"arrival_taxi_completed_at_seconds\":{},"
        "\"arrival_taxi_distance_m\":{},\"departure_taxi_started_at_seconds\":{},\"departure_taxi_completed_at_seconds\":{},"
        "\"departure_taxi_distance_m\":{},\"arrival_runway_request_time_seconds\":{},"
        "\"arrival_runway_clearance_time_seconds\":{},\"arrival_runway_release_time_seconds\":{},"
        "\"departure_runway_request_time_seconds\":{},\"departure_runway_clearance_time_seconds\":{},"
        "\"departure_runway_release_time_seconds\":{},\"taxi_distance_m\":{},\"taxi_started_at_seconds\":{},"
        "\"taxi_completed_at_seconds\":{}}}",
        aircraft.id.value(), json_escape(aircraft.flight_number), json_escape(aircraft.operation_type),
        json_escape(aircraft.surface_state),
        aircraft.runway_queue_entered_at ? std::to_string(aircraft.runway_queue_entered_at->count()) : "null",
        aircraft.runway_clearance_at ? std::to_string(aircraft.runway_clearance_at->count()) : "null",
        aircraft.runway_wait_duration.count(),
        aircraft.runway_release_at ? std::to_string(aircraft.runway_release_at->count()) : "null",
        aircraft.assigned_gate.value(),
        aircraft.gate_assigned_at ? std::to_string(aircraft.gate_assigned_at->count()) : "null",
        aircraft.actual_arrival ? std::to_string(aircraft.actual_arrival->count()) : "null",
        aircraft.arrival_gate_at ? std::to_string(aircraft.arrival_gate_at->count()) : "null",
        aircraft.gate_wait_duration.count(), aircraft.gate_occupancy_duration.count(),
        aircraft.pushback_started_at ? std::to_string(aircraft.pushback_started_at->count()) : "null",
        aircraft.pushback_completed_at ? std::to_string(aircraft.pushback_completed_at->count()) : "null",
        aircraft.actual_surface_departure ? std::to_string(aircraft.actual_surface_departure->count()) : "null",
        aircraft.arrival_taxi_started_at ? std::to_string(aircraft.arrival_taxi_started_at->count()) : "null",
        aircraft.arrival_taxi_completed_at ? std::to_string(aircraft.arrival_taxi_completed_at->count()) : "null",
        aircraft.arrival_taxi_distance_m,
        aircraft.departure_taxi_started_at ? std::to_string(aircraft.departure_taxi_started_at->count()) : "null",
        aircraft.departure_taxi_completed_at ? std::to_string(aircraft.departure_taxi_completed_at->count()) : "null",
        aircraft.departure_taxi_distance_m,
        aircraft.arrival_runway_request_time ? std::to_string(aircraft.arrival_runway_request_time->count()) : "null",
        aircraft.arrival_runway_clearance_time ? std::to_string(aircraft.arrival_runway_clearance_time->count()) : "null",
        aircraft.arrival_runway_release_time ? std::to_string(aircraft.arrival_runway_release_time->count()) : "null",
        aircraft.departure_runway_request_time ? std::to_string(aircraft.departure_runway_request_time->count()) : "null",
        aircraft.departure_runway_clearance_time ? std::to_string(aircraft.departure_runway_clearance_time->count()) : "null",
        aircraft.departure_runway_release_time ? std::to_string(aircraft.departure_runway_release_time->count()) : "null",
        aircraft.taxi_distance_m,
        aircraft.taxi_started_at ? std::to_string(aircraft.taxi_started_at->count()) : "null",
        aircraft.taxi_completed_at ? std::to_string(aircraft.taxi_completed_at->count()) : "null");
  }
  output += "]}";
  return output;
}

inline std::string turnaround_event_json(const airside::SimulationEventRecord& event) {
  auto output = std::format(
      "{{\"sequence\":{},\"time_seconds\":{},\"type\":\"{}\",\"turnaround_id\":\"{}\","
      "\"aircraft_id\":{},\"gate_id\":{},\"task_id\":{},\"service_type\":\"{}\",\"vehicle_id\":{},\"detail\":\"{}\"",
      event.sequence, event.timestamp.count(), airside::to_string(event.type),
      json_escape(event.turnaround_id), event.aircraft ? std::to_string(event.aircraft->value()) : "null",
      event.gate ? std::to_string(event.gate->value()) : "null", event.task ? std::to_string(event.task->value()) : "null",
      event.service ? airside::to_string(*event.service) : std::string_view{},
      event.vehicle ? std::to_string(event.vehicle->value()) : "null", json_escape(event.detail));
  if (event.edge) output += std::format(",\"edge_id\":{}", event.edge->value());
  if (event.route) {
    output += ",\"route_node_ids\":[";
    for (std::size_t index = 0; index < event.route->nodes.size(); ++index) {
      if (index != 0) output += ',';
      output += std::to_string(event.route->nodes[index].value());
    }
    output += ']';
  }
  output += '}';
  return output;
}

}  // namespace ramplab_ros2_bridge
