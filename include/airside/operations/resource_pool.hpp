#pragma once

#include "airside/core/types.hpp"

#include <cstddef>
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

    [[nodiscard]] ResourceRequestResult request(AircraftId aircraft);
    [[nodiscard]] std::optional<ResourceAssignment> release(VehicleId vehicle);
    [[nodiscard]] std::optional<AircraftId> assigned_aircraft(VehicleId vehicle) const;
    [[nodiscard]] std::size_t waiting_count() const noexcept;
    [[nodiscard]] std::size_t available_count() const noexcept;

private:
    [[nodiscard]] bool is_waiting(AircraftId aircraft) const noexcept;

    std::vector<VehicleId> available_;
    std::deque<AircraftId> waiting_;
    std::unordered_map<VehicleId, AircraftId> assignments_;
};

}  // namespace airside
