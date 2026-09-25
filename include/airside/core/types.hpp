#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
#include <functional>
#include <ostream>

namespace airside {

using SimTime = std::chrono::seconds;

template <typename Tag>
class StrongId {
public:
    using value_type = std::uint32_t;

    constexpr StrongId() = default;
    explicit constexpr StrongId(value_type value) noexcept : value_(value) {}

    [[nodiscard]] constexpr value_type value() const noexcept { return value_; }
    constexpr auto operator<=>(const StrongId&) const = default;

private:
    value_type value_{0};
};

struct AircraftTag;
struct VehicleTag;
struct GateTag;
struct NodeTag;
struct TaskTag;
struct EdgeTag;

using AircraftId = StrongId<AircraftTag>;
using VehicleId = StrongId<VehicleTag>;
using GateId = StrongId<GateTag>;
using NodeId = StrongId<NodeTag>;
using TaskId = StrongId<TaskTag>;
using EdgeId = StrongId<EdgeTag>;

template <typename Tag>
std::ostream& operator<<(std::ostream& stream, StrongId<Tag> id) {
    return stream << id.value();
}

}  // namespace airside

template <typename Tag>
struct std::hash<airside::StrongId<Tag>> {
    std::size_t operator()(airside::StrongId<Tag> id) const noexcept {
        return std::hash<std::uint32_t>{}(id.value());
    }
};
