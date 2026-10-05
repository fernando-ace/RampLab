#pragma once

#include "airside/core/types.hpp"

#include <cstdint>
#include <optional>
#include <queue>
#include <vector>

namespace airside {

enum class EventType {
    AircraftArrival,
    VehicleArrivalAtAircraft,
    ServiceCompleted,
    VehicleArrivalAtDepot,
    EdgeAvailabilityChanged,
    AircraftDeparture,
    AbstractServiceCompleted,
    TurnaroundTaskEligible,
    TaskDurationChanged,
    FleetTick,
    VehicleOutage,
    SurfaceTick,
};

enum class EntityKind { None, Aircraft, Vehicle, Edge, Task };

struct EntityRef {
    EntityKind kind{EntityKind::None};
    std::uint32_t value{0};

    constexpr auto operator<=>(const EntityRef&) const = default;
};

struct Event {
    SimTime timestamp{};
    EventType type{EventType::AircraftArrival};
    EntityRef entity{};
    std::uint64_t sequence{0};
    std::int64_t data{0};

    constexpr auto operator<=>(const Event&) const = default;
};

class EventQueue {
public:
    [[nodiscard]] std::uint64_t schedule(
        SimTime timestamp,
        EventType type,
        EntityRef entity = {},
        std::int64_t data = 0);

    [[nodiscard]] std::optional<Event> pop();
    [[nodiscard]] const Event* peek() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    void clear() noexcept;

private:
    struct LaterEvent {
        bool operator()(const Event& left, const Event& right) const noexcept;
    };

    std::priority_queue<Event, std::vector<Event>, LaterEvent> events_;
    std::uint64_t next_sequence_{0};
};

}  // namespace airside
