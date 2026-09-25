#include "airside/operations/resource_pool.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace airside {

ResourcePool::ResourcePool(std::vector<VehicleId> vehicles) : available_(std::move(vehicles)) {
    if (available_.empty()) {
        throw std::invalid_argument("resource pool requires at least one vehicle");
    }
    std::ranges::sort(available_);
    const std::unordered_set<VehicleId> unique(available_.begin(), available_.end());
    if (unique.size() != available_.size()) {
        throw std::invalid_argument("resource pool vehicle IDs must be unique");
    }
}

ResourceRequestResult ResourcePool::request(AircraftId aircraft) {
    const auto already_assigned = std::ranges::any_of(assignments_, [&](const auto& entry) {
        return entry.second == aircraft;
    });
    if (already_assigned || is_waiting(aircraft)) {
        return {RequestStatus::AlreadyRequested, std::nullopt};
    }

    if (available_.empty()) {
        waiting_.push_back(aircraft);
        return {RequestStatus::Queued, std::nullopt};
    }

    const auto vehicle = available_.front();
    available_.erase(available_.begin());
    assignments_.emplace(vehicle, aircraft);
    return {RequestStatus::Assigned, vehicle};
}

std::optional<ResourceAssignment> ResourcePool::release(VehicleId vehicle) {
    const auto assigned = assignments_.find(vehicle);
    if (assigned == assignments_.end()) {
        throw std::logic_error("cannot release a vehicle that is not assigned");
    }
    assignments_.erase(assigned);

    if (!waiting_.empty()) {
        const auto aircraft = waiting_.front();
        waiting_.pop_front();
        assignments_.emplace(vehicle, aircraft);
        return ResourceAssignment{vehicle, aircraft};
    }

    available_.push_back(vehicle);
    std::ranges::sort(available_);
    return std::nullopt;
}

std::optional<AircraftId> ResourcePool::assigned_aircraft(VehicleId vehicle) const {
    const auto found = assignments_.find(vehicle);
    return found == assignments_.end() ? std::nullopt : std::optional{found->second};
}

std::size_t ResourcePool::waiting_count() const noexcept { return waiting_.size(); }
std::size_t ResourcePool::available_count() const noexcept { return available_.size(); }

bool ResourcePool::is_waiting(AircraftId aircraft) const noexcept {
    return std::ranges::find(waiting_, aircraft) != waiting_.end();
}

}  // namespace airside
