#pragma once

#include "hexa_udon/core/map_definition.hpp"
#include "hexa_udon/core/result.hpp"
#include "hexa_udon/core/types.hpp"

#include <vector>

namespace hexa_udon::core {

struct Spot {
    Quantity brand;
    CellIndex position;
    Quantity max_stock;
};

struct AgentState {
    AgentKind kind;
    CellIndex position;
    Quantity fuel;
};

struct OtherTeamState {
    std::int32_t id;
    std::vector<AgentState> agents;
};

struct TrafficState {
    CellIndex position;
    RoadStatus status;
};

struct MatchConfig {
    UnixTimestamp starts_at;
    std::vector<Quantity> day_seconds;
    std::vector<Quantity> day_steps;
    MapDefinition map;
    std::vector<Spot> spots;
    std::vector<CellIndex> initial_agent_positions;
    Quantity fuel_limit;
    Quantity players;
    Quantity busy_threshold;
    Quantity jammed_threshold;
};

struct DailyState {
    UnixTimestamp ends_at;
    Quantity day;
    std::vector<AgentState> own_agents;
    std::vector<OtherTeamState> other_teams;
    std::vector<TrafficState> traffic;
};

[[nodiscard]] ValidationErrors validate(const MatchConfig& config);
[[nodiscard]] ValidationErrors validate(const DailyState& state, const MatchConfig& config);

}  // namespace hexa_udon::core
