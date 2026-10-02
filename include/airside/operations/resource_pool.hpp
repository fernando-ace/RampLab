#pragma once

#include "airside/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

namespace airside {

enum class RequestStatus { Assigned, Queued, AlreadyRequested };

struct ResourceRequestResult {
    RequestStatus status{RequestStatus::Queued};
    std::optional<VehicleId> vehicle;
};

struct ResourceAssignment {
    VehicleId vehicle;
    AircraftId aircraft;
};

class ResourcePool {
public:
    explicit ResourcePool(std::vector<VehicleId> vehicles);

    [[nodiscard]] ResourceRequestResult request(
        AircraftId aircraft, std::int64_t priority = 0, SimTime requested_at = SimTime::zero());
    [[nodiscard]] std::optional<ResourceAssignment> release(VehicleId vehicle, SimTime now = SimTime::zero());
    [[nodiscard]] std::optional<AircraftId> assigned_aircraft(VehicleId vehicle) const;
    [[nodiscard]] std::size_t waiting_count() const noexcept;
    [[nodiscard]] std::size_t available_count() const noexcept;
    [[nodiscard]] std::size_t outstanding_count() const noexcept;

private:
    [[nodiscard]] bool is_waiting(AircraftId aircraft) const noexcept;

    std::vector<VehicleId> available_;
    struct WaitingRequest { AircraftId aircraft; std::int64_t priority; SimTime requested_at; std::uint64_t sequence; };
    std::deque<WaitingRequest> waiting_;
    std::unordered_map<VehicleId, AircraftId> assignments_;
    std::uint64_t next_waiting_sequence_{0};
};

}  // namespace airside
