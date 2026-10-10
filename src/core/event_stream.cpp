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
    case SimulationEventType::TurnaroundTaskFailed: return "TurnaroundTaskFailed";
    case SimulationEventType::TurnaroundFailed: return "TurnaroundFailed";
    case SimulationEventType::TurnaroundVehicleUnavailable: return "TurnaroundVehicleUnavailable";
    case SimulationEventType::SurfacePushbackRequested: return "SurfacePushbackRequested";
    case SimulationEventType::SurfacePushbackStarted: return "SurfacePushbackStarted";
    case SimulationEventType::SurfacePushbackCompleted: return "SurfacePushbackCompleted";
    case SimulationEventType::SurfaceTaxiRouteAssigned: return "SurfaceTaxiRouteAssigned";
    case SimulationEventType::SurfaceWaitingForTraffic: return "SurfaceWaitingForTraffic";
    case SimulationEventType::SurfaceRerouted: return "SurfaceRerouted";
    case SimulationEventType::SurfaceRunwayQueueEntered: return "SurfaceRunwayQueueEntered";
    case SimulationEventType::SurfaceRunwayClearance: return "SurfaceRunwayClearance";
    case SimulationEventType::SurfaceSafeFailure: return "SurfaceSafeFailure";
    case SimulationEventType::SurfaceRouteInvalidated: return "SurfaceRouteInvalidated";
    case SimulationEventType::SurfaceReservationAcquired: return "SurfaceReservationAcquired";
    case SimulationEventType::SurfaceReservationReleased: return "SurfaceReservationReleased";
    case SimulationEventType::SurfaceWaitingForPushback: return "SurfaceWaitingForPushback";
    case SimulationEventType::RunwayRequest: return "RunwayRequest";
    case SimulationEventType::RunwayGrant: return "RunwayGrant";
    case SimulationEventType::RunwayOccupied: return "RunwayOccupied";
    case SimulationEventType::RunwayReleased: return "RunwayReleased";
    case SimulationEventType::ArrivalRunwayExit: return "ArrivalRunwayExit";
    case SimulationEventType::ArrivalTaxiInStarted: return "ArrivalTaxiInStarted";
    case SimulationEventType::ArrivalAtGate: return "ArrivalAtGate";
    case SimulationEventType::GateWaitStarted: return "GateWaitStarted";
    case SimulationEventType::GateAssigned: return "GateAssigned";
    case SimulationEventType::AircraftLanded: return "AircraftLanded";
    case SimulationEventType::TurnaroundStarted: return "TurnaroundStarted";
    case SimulationEventType::PushbackTaxiOutStarted: return "PushbackTaxiOutStarted";
    case SimulationEventType::OperatorIntervention: return "OperatorIntervention";
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
    case SimulationEventType::RunwayRequest: return std::format("{} requested runway access ({})", aircraft, event.detail);
    case SimulationEventType::RunwayGrant: return std::format("{} received runway clearance", aircraft);
    case SimulationEventType::RunwayOccupied: return std::format("{} entered the runway", aircraft);
    case SimulationEventType::RunwayReleased: return std::format("{} released the runway", aircraft);
    case SimulationEventType::ArrivalRunwayExit: return std::format("{} exited the runway", aircraft);
    case SimulationEventType::AircraftLanded: return std::format("{} landed", aircraft);
    case SimulationEventType::ArrivalTaxiInStarted: return std::format("{} began taxi-in", aircraft);
    case SimulationEventType::ArrivalAtGate: return std::format("{} arrived at its assigned gate", aircraft);
    case SimulationEventType::GateWaitStarted: return std::format("{} waiting for its assigned gate", aircraft);
    case SimulationEventType::GateAssigned:
        return event.gate ? std::format("{} assigned Gate A{}", aircraft, event.gate->value()) :
            std::format("{} assigned an arrival gate", aircraft);
    case SimulationEventType::TurnaroundStarted: return std::format("{} turnaround started", aircraft);
    case SimulationEventType::PushbackTaxiOutStarted: return std::format("{} started pushback and taxi-out", aircraft);
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
    case SimulationEventType::TurnaroundTaskFailed:
        return std::format("turnaround {} task {} failed: {}", event.turnaround_id,
            event.task ? event.task->value() : 0U, event.detail);
    case SimulationEventType::TurnaroundFailed:
        return std::format("turnaround {} failed: {}", event.turnaround_id, event.detail);
    case SimulationEventType::TurnaroundVehicleUnavailable:
        return std::format("turnaround {} vehicle {} unavailable", event.turnaround_id, vehicle);
    case SimulationEventType::SurfacePushbackRequested: return std::format("{} requested pushback", aircraft);
    case SimulationEventType::SurfacePushbackStarted: return std::format("{} started pushback", aircraft);
    case SimulationEventType::SurfacePushbackCompleted: return std::format("{} completed pushback", aircraft);
    case SimulationEventType::SurfaceTaxiRouteAssigned: return std::format("{} assigned a taxi route", aircraft);
    case SimulationEventType::SurfaceWaitingForTraffic: return std::format("{} waiting for surface traffic", aircraft);
    case SimulationEventType::SurfaceRerouted: return std::format("{} rerouted around a surface closure", aircraft);
    case SimulationEventType::SurfaceRunwayQueueEntered: return std::format("{} entered the runway queue", aircraft);
    case SimulationEventType::SurfaceRunwayClearance: return std::format("{} received runway clearance", aircraft);
    case SimulationEventType::SurfaceSafeFailure:
        return std::format("{} safely stopped: {}", aircraft,
            event.detail.empty() ? "no route to departure handoff" : event.detail);
    case SimulationEventType::SurfaceRouteInvalidated: return std::format("{} route invalidated by closure on edge {}", aircraft, event.edge ? event.edge->value() : 0U);
    case SimulationEventType::SurfaceReservationAcquired: return std::format("{} acquired surface edge {}", aircraft, event.edge ? event.edge->value() : 0U);
    case SimulationEventType::SurfaceReservationReleased: return std::format("{} released surface edge {}", aircraft, event.edge ? event.edge->value() : 0U);
    case SimulationEventType::SurfaceWaitingForPushback: return std::format("{} waiting for a safe pushback window", aircraft);
    }
    return "unknown simulation event";
}

}  // namespace airside
