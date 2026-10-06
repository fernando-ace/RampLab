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
  airside::AircraftSnapshot aircraft;
  aircraft.id = airside::AircraftId{7};
  aircraft.surface_state = "Taxiing";
  aircraft.surface_position_m = airside::Vec2{12.5, -3.0};
  aircraft.surface_heading_rad = 1.25;
  aircraft.surface_speed_mps = 4.0;
  aircraft.surface_route = {airside::NodeId{4}, airside::NodeId{8}};
  aircraft.surface_wait_reason = "edge_or_intersection_reserved";
  snapshot.aircraft.push_back(aircraft);

  const auto json = ramplab_ros2_bridge::turnaround_state_json(snapshot);
  EXPECT_NE(json.find("\"turnaround_id\":\"TO-AX101\""), std::string::npos);
  EXPECT_NE(json.find("\"aircraft_id\":7"), std::string::npos);
  EXPECT_NE(json.find("\"gate_id\":2"), std::string::npos);
  EXPECT_NE(json.find("\"scheduled_arrival_seconds\":0"), std::string::npos);
  EXPECT_NE(json.find("\"pending_task_count\":0"), std::string::npos);
  EXPECT_NE(json.find("\"schedule_slack_seconds\":-30"), std::string::npos);
  EXPECT_NE(json.find("\"critical_path_task_ids\":[3]"), std::string::npos);
  EXPECT_NE(json.find("\"assigned_vehicle_id\":9"), std::string::npos);
  EXPECT_NE(json.find("\"active_task_count\":1"), std::string::npos);
  EXPECT_NE(json.find("\"surface_state\":\"Taxiing\""), std::string::npos);
  EXPECT_NE(json.find("\"surface_position_m\":{\"x_m\":12.5,\"y_m\":-3}"), std::string::npos);
  EXPECT_NE(json.find("\"surface_route_node_ids\":[4,8]"), std::string::npos);
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
  event.gate = airside::GateId{4};
  event.route = airside::Route{{airside::NodeId{1}, airside::NodeId{2}}, {airside::EdgeId{9}}, 10.0, airside::SimTime{3}};

  const auto json = ramplab_ros2_bridge::turnaround_event_json(event);
  EXPECT_NE(json.find("\"type\":\"TurnaroundTaskStarted\""), std::string::npos);
  EXPECT_NE(json.find("TO-\\\"7"), std::string::npos);
  EXPECT_NE(json.find("\"task_id\":2"), std::string::npos);
  EXPECT_NE(json.find("\"gate_id\":4"), std::string::npos);
  EXPECT_NE(json.find("\"route_node_ids\":[1,2]"), std::string::npos);
}

TEST(TurnaroundJsonTest, PublishesMixedRunwayOwnerQueueAndArrivalOperationState) {
  airside::SimulationSnapshot snapshot;
  snapshot.simulation_time = airside::SimTime{25};
  snapshot.runway_owner = airside::AircraftId{8};
  snapshot.runway_queue = {airside::AircraftId{9}};
  airside::AircraftSnapshot arrival;
  arrival.id = airside::AircraftId{8};
  arrival.flight_number = "AR-8";
  arrival.operation_type = "arrival";
  arrival.surface_state = "RunwayOccupied";
  arrival.runway_queue_entered_at = airside::SimTime{10};
  arrival.runway_clearance_at = airside::SimTime{20};
  arrival.runway_release_at = airside::SimTime{25};
  snapshot.aircraft.push_back(arrival);
  const auto json = ramplab_ros2_bridge::turnaround_state_json(snapshot);
  EXPECT_NE(json.find("\"runway_owner_id\":8"), std::string::npos);
  EXPECT_NE(json.find("\"runway_queue_aircraft_ids\":[9]"), std::string::npos);
  EXPECT_NE(json.find("\"operation_type\":\"arrival\""), std::string::npos);
  EXPECT_NE(json.find("\"surface_state\":\"RunwayOccupied\""), std::string::npos);
  EXPECT_NE(json.find("\"runway_clearance_time_seconds\":20"), std::string::npos);
}
