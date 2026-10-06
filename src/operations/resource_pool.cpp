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

ResourceRequestResult ResourcePool::request(AircraftId aircraft, std::int64_t priority, SimTime requested_at) {
    const auto already_assigned = std::ranges::any_of(assignments_, [&](const auto& entry) {
        return entry.second == aircraft;
    });
    if (already_assigned || is_waiting(aircraft)) {
        return {RequestStatus::AlreadyRequested, std::nullopt};
    }

    if (available_.empty()) {
        waiting_.push_back({aircraft, priority, requested_at, next_waiting_sequence_++});
        return {RequestStatus::Queued, std::nullopt};
    }

    const auto vehicle = available_.front();
    available_.erase(available_.begin());
    assignments_.emplace(vehicle, aircraft);
    return {RequestStatus::Assigned, vehicle};
}

std::optional<ResourceAssignment> ResourcePool::release(VehicleId vehicle, SimTime now) {
    const auto assigned = assignments_.find(vehicle);
    if (assigned == assignments_.end()) {
        throw std::logic_error("cannot release a vehicle that is not assigned");
    }
    assignments_.erase(assigned);

    if (!waiting_.empty()) {
        const auto best = std::ranges::max_element(waiting_, [now](const WaitingRequest& left, const WaitingRequest& right) {
            const auto left_age = std::max<std::int64_t>(0, (now - left.requested_at).count()) / 60;
            const auto right_age = std::max<std::int64_t>(0, (now - right.requested_at).count()) / 60;
            const auto left_priority = left.priority + left_age;
            const auto right_priority = right.priority + right_age;
            if (left_priority != right_priority) return left_priority < right_priority;
            if (left.requested_at != right.requested_at) return left.requested_at > right.requested_at;
            return left.sequence > right.sequence;
        });
        const auto assignment = *best;
        waiting_.erase(best);
        assignments_.emplace(vehicle, assignment.aircraft);
        return ResourceAssignment{vehicle, assignment.aircraft};
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
std::size_t ResourcePool::outstanding_count() const noexcept { return waiting_.size() + assignments_.size(); }

bool ResourcePool::is_waiting(AircraftId aircraft) const noexcept {
    return std::ranges::any_of(waiting_, [aircraft](const WaitingRequest& request) { return request.aircraft == aircraft; });
}

}  // namespace airside
