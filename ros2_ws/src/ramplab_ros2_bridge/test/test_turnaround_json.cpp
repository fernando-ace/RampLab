#include "ramplab_ros2_bridge/turnaround_json.hpp"

#include <gtest/gtest.h>

TEST(TurnaroundJsonTest, PublishesTaskStatePriorityScheduleAndAssignments) {
  airside::SimulationSnapshot snapshot;
  snapshot.simulation_time = airside::SimTime{40};
  airside::TurnaroundSnapshot turnaround;
  turnaround.turnaround_id = "TO-AX101";
  turnaround.aircraft = airside::AircraftId{7};
  turnaround.gate = airside::GateId{2};
  turnaround.state = airside::TurnaroundState::Servicing;
  turnaround.scheduled_departure = airside::SimTime{600};
  turnaround.target_off_block = airside::SimTime{500};
  turnaround.estimated_ready_time = airside::SimTime{530};
  turnaround.schedule_slack = airside::SimTime{-30};
  turnaround.critical_path_tasks = {airside::TaskId{3}};
  airside::ServiceTaskSnapshot task;
  task.id = airside::TaskId{3};
  task.type = airside::ServiceType::Fueling;
  task.status = airside::TaskStatus::InProgress;
  task.assigned_vehicle = airside::VehicleId{9};
  task.assigned_resource = "FuelTruck-9";
  turnaround.tasks.push_back(task);
  snapshot.turnarounds.push_back(turnaround);

  const auto json = ramplab_ros2_bridge::turnaround_state_json(snapshot);
  EXPECT_NE(json.find("\"turnaround_id\":\"TO-AX101\""), std::string::npos);
  EXPECT_NE(json.find("\"schedule_slack_seconds\":-30"), std::string::npos);
  EXPECT_NE(json.find("\"critical_path_task_ids\":[3]"), std::string::npos);
  EXPECT_NE(json.find("\"assigned_vehicle_id\":9"), std::string::npos);
  EXPECT_NE(json.find("\"active_task_count\":1"), std::string::npos);
}

TEST(TurnaroundJsonTest, SerializesStructuredTurnaroundEventsAndEscapesIdentifiers) {
  airside::SimulationEventRecord event{airside::SimulationEventType::TurnaroundTaskStarted};
  event.sequence = 4;
  event.timestamp = airside::SimTime{12};
  event.turnaround_id = "TO-\"7";
  event.aircraft = airside::AircraftId{1};
  event.task = airside::TaskId{2};
  event.service = airside::ServiceType::Catering;
  event.vehicle = airside::VehicleId{3};

  const auto json = ramplab_ros2_bridge::turnaround_event_json(event);
  EXPECT_NE(json.find("\"type\":\"TurnaroundTaskStarted\""), std::string::npos);
  EXPECT_NE(json.find("TO-\\\"7"), std::string::npos);
  EXPECT_NE(json.find("\"task_id\":2"), std::string::npos);
}
