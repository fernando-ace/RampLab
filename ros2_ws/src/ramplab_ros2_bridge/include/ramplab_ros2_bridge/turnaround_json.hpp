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
  std::string output = std::format("{{\"simulation_time_seconds\":{},\"turnarounds\":[",
                                   snapshot.simulation_time.count());
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
    output += std::format("],\"pending_task_count\":{},\"tasks\":[", pending);
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
  output += "]}";
  return output;
}

inline std::string turnaround_event_json(const airside::SimulationEventRecord& event) {
  return std::format(
      "{{\"sequence\":{},\"time_seconds\":{},\"type\":\"{}\",\"turnaround_id\":\"{}\","
      "\"aircraft_id\":{},\"gate_id\":{},\"task_id\":{},\"service_type\":\"{}\",\"vehicle_id\":{},\"detail\":\"{}\"}}",
      event.sequence, event.timestamp.count(), airside::to_string(event.type),
      json_escape(event.turnaround_id), event.aircraft ? std::to_string(event.aircraft->value()) : "null",
      event.gate ? std::to_string(event.gate->value()) : "null", event.task ? std::to_string(event.task->value()) : "null",
      event.service ? airside::to_string(*event.service) : std::string_view{},
      event.vehicle ? std::to_string(event.vehicle->value()) : "null", json_escape(event.detail));
}

}  // namespace ramplab_ros2_bridge
