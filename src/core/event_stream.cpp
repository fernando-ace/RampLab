#include "airside/core/event_stream.hpp"

#include <format>

namespace airside {

std::string_view to_string(SimulationEventType type) noexcept {
    switch (type) {
    case SimulationEventType::AircraftArrived: return "AircraftArrived";
    case SimulationEventType::AircraftStateChanged: return "AircraftStateChanged";
    case SimulationEventType::VehicleAssigned: return "VehicleAssigned";
    case SimulationEventType::VehicleDeparted: return "VehicleDeparted";
    case SimulationEventType::VehicleArrived: return "VehicleArrived";
    case SimulationEventType::VehicleStateChanged: return "VehicleStateChanged";
    case SimulationEventType::ServiceStarted: return "ServiceStarted";
    case SimulationEventType::ServiceCompleted: return "ServiceCompleted";
    case SimulationEventType::AircraftReadyForPushback: return "AircraftReadyForPushback";
    case SimulationEventType::AircraftDeparted: return "AircraftDeparted";
    case SimulationEventType::RoadClosed: return "RoadClosed";
    case SimulationEventType::RoadOpened: return "RoadOpened";
    case SimulationEventType::ResourceWaitStarted: return "ResourceWaitStarted";
    case SimulationEventType::ResourceAssigned: return "ResourceAssigned";
    case SimulationEventType::TurnaroundCreated: return "TurnaroundCreated";
    case SimulationEventType::TurnaroundTaskReady: return "TurnaroundTaskReady";
    case SimulationEventType::TurnaroundTaskDispatched: return "TurnaroundTaskDispatched";
    case SimulationEventType::TurnaroundTaskStarted: return "TurnaroundTaskStarted";
    case SimulationEventType::TurnaroundTaskCompleted: return "TurnaroundTaskCompleted";
    case SimulationEventType::TurnaroundTaskReassigned: return "TurnaroundTaskReassigned";
    case SimulationEventType::TurnaroundCriticalPathChanged: return "TurnaroundCriticalPathChanged";
    case SimulationEventType::TurnaroundPredictedLate: return "TurnaroundPredictedLate";
    case SimulationEventType::TurnaroundReadyForDeparture: return "TurnaroundReadyForDeparture";
    case SimulationEventType::TurnaroundDisruptionDetected: return "TurnaroundDisruptionDetected";
    }
    return "Unknown";
}

std::string_view to_string(AircraftState state) noexcept {
    switch (state) {
    case AircraftState::Scheduled: return "Scheduled";
    case AircraftState::Arriving: return "Arriving";
    case AircraftState::AtGate: return "AtGate";
    case AircraftState::WaitingForServices: return "WaitingForServices";
    case AircraftState::ReadyForPushback: return "ReadyForPushback";
    case AircraftState::Departed: return "Departed";
    }
    return "Unknown";
}

std::string_view to_string(VehicleState state) noexcept {
    switch (state) {
    case VehicleState::Idle: return "Idle";
    case VehicleState::Assigned: return "Assigned";
    case VehicleState::TravelingToAircraft: return "TravelingToAircraft";
    case VehicleState::Servicing: return "Servicing";
    case VehicleState::ReturningToDepot: return "ReturningToDepot";
    }
    return "Unknown";
}

std::string_view to_string(ServiceType type) noexcept {
    switch (type) {
    case ServiceType::Fueling: return "Fueling";
    case ServiceType::Baggage: return "BaggageUnload";
    case ServiceType::Deboarding: return "Deboarding";
    case ServiceType::Catering: return "Catering";
    case ServiceType::CabinCleaning: return "CabinCleaning";
    case ServiceType::BaggageLoad: return "BaggageLoad";
    case ServiceType::PushbackPreparation: return "PushbackPreparation";
    }
    return "Unknown";
}

std::string_view to_string(TurnaroundState state) noexcept {
    switch (state) {
    case TurnaroundState::Scheduled: return "Scheduled";
    case TurnaroundState::Arrived: return "Arrived";
    case TurnaroundState::Servicing: return "Servicing";
    case TurnaroundState::ReadyForDeparture: return "ReadyForDeparture";
    case TurnaroundState::Departed: return "Departed";
    case TurnaroundState::Delayed: return "Delayed";
    case TurnaroundState::Failed: return "Failed";
    }
    return "Unknown";
}

std::string_view to_string(TaskStatus state) noexcept {
    switch (state) {
    case TaskStatus::Blocked: return "Blocked";
    case TaskStatus::Pending: return "Ready";
    case TaskStatus::Waiting: return "Waiting";
    case TaskStatus::Assigned: return "Dispatched";
    case TaskStatus::InProgress: return "InProgress";
    case TaskStatus::Completed: return "Completed";
    case TaskStatus::Failed: return "Failed";
    }
    return "Unknown";
}

std::string format_event(const SimulationEventRecord& event) {
    const auto& aircraft = event.aircraft_name.empty() ? std::string{"aircraft"} : event.aircraft_name;
    const auto& vehicle = event.vehicle_name.empty() ? std::string{"vehicle"} : event.vehicle_name;
    const auto service_resource = !event.service ? std::string_view{"service"} :
        *event.service == ServiceType::Fueling ? std::string_view{"fuel"} :
        *event.service == ServiceType::Baggage ? std::string_view{"baggage"} : to_string(*event.service);
    switch (event.type) {
    case SimulationEventType::AircraftArrived:
        return std::format("{} arrived at Gate A{}", aircraft, event.gate->value());
    case SimulationEventType::AircraftStateChanged:
        return std::format("{} state {} -> {}", aircraft,
            to_string(*event.previous_aircraft_state), to_string(*event.aircraft_state));
    case SimulationEventType::VehicleAssigned: {
        std::string route;
        for (std::size_t index = 0; index < event.route->nodes.size(); ++index) {
            if (index != 0) route += "->";
            route += std::to_string(event.route->nodes[index].value());
        }
        return std::format("{} assigned to {} (route {}, {} sec)", vehicle, aircraft,
            route, event.route->travel_time.count());
    }
    case SimulationEventType::VehicleDeparted:
        return std::format("{} departed on route to node {}", vehicle, event.route->nodes.back().value());
    case SimulationEventType::VehicleArrived:
        return std::format("{} arrived at node {}", vehicle,
            event.route ? event.route->nodes.back().value() : 0U);
    case SimulationEventType::VehicleStateChanged:
        return std::format("{} state {} -> {}", vehicle,
            to_string(*event.previous_vehicle_state), to_string(*event.vehicle_state));
    case SimulationEventType::ServiceStarted:
        return std::format("{} service started for {}", to_string(*event.service), aircraft);
    case SimulationEventType::ServiceCompleted:
        return std::format("{} service completed for {}", to_string(*event.service), aircraft);
    case SimulationEventType::AircraftReadyForPushback: return std::format("{} ready for pushback", aircraft);
    case SimulationEventType::AircraftDeparted: return std::format("{} departed", aircraft);
    case SimulationEventType::RoadClosed: return std::format("road edge {} closed", event.edge->value());
    case SimulationEventType::RoadOpened: return std::format("road edge {} opened", event.edge->value());
    case SimulationEventType::ResourceWaitStarted:
        return std::format("{} waiting for {} resource", aircraft, service_resource);
    case SimulationEventType::ResourceAssigned:
        return std::format("{} resource assigned to {} via {}", to_string(*event.service), aircraft, vehicle);
    case SimulationEventType::TurnaroundCreated:
        return std::format("turnaround {} created for {}", event.turnaround_id, aircraft);
    case SimulationEventType::TurnaroundTaskReady:
        return std::format("turnaround {} task {} ready ({})", event.turnaround_id,
            event.task ? event.task->value() : 0U, to_string(*event.service));
    case SimulationEventType::TurnaroundTaskDispatched:
        return std::format("turnaround {} task {} dispatched to {}", event.turnaround_id,
            event.task ? event.task->value() : 0U, vehicle);
    case SimulationEventType::TurnaroundTaskStarted:
        return std::format("turnaround {} task {} started ({})", event.turnaround_id,
            event.task ? event.task->value() : 0U, to_string(*event.service));
    case SimulationEventType::TurnaroundTaskCompleted:
        return std::format("turnaround {} task {} completed ({})", event.turnaround_id,
            event.task ? event.task->value() : 0U, to_string(*event.service));
    case SimulationEventType::TurnaroundTaskReassigned:
        return std::format("turnaround {} task {} reassigned to {}", event.turnaround_id,
            event.task ? event.task->value() : 0U, vehicle);
    case SimulationEventType::TurnaroundCriticalPathChanged:
        return std::format("turnaround {} critical path updated", event.turnaround_id);
    case SimulationEventType::TurnaroundPredictedLate:
        return std::format("turnaround {} predicted late", event.turnaround_id);
    case SimulationEventType::TurnaroundReadyForDeparture:
        return std::format("turnaround {} ready for departure", event.turnaround_id);
    case SimulationEventType::TurnaroundDisruptionDetected:
        return std::format("turnaround {} task {} duration changed to {} sec", event.turnaround_id,
            event.task ? event.task->value() : 0U, event.task_duration ? event.task_duration->count() : 0);
    }
    return "unknown simulation event";
}

}  // namespace airside
