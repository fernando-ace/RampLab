#pragma once

#include "airside/core/types.hpp"
#include "airside/world/airport_graph.hpp"

#include <optional>
#include <string>
#include <utility>

namespace airside {

struct Gate {
    GateId id;
    std::string name;
    NodeId node;
    // Optional physical stand location. node remains the taxi-route attachment.
    // Departure pushback moves from this stand to node under its normal reservation.
    std::optional<Vec2> stand_position_m;
    bool enabled{true};
    std::optional<AircraftId> occupying_aircraft;

    Gate(GateId gate_id, std::string gate_name, NodeId gate_node, bool is_enabled = true)
        : id(gate_id), name(std::move(gate_name)), node(gate_node), enabled(is_enabled) {}
};

}  // namespace airside
