#include "hexa_udon/core/models.hpp"

#include <cstddef>
#include <string>
#include <utility>

namespace hexa_udon::core {

namespace {

void require_positive(
    ValidationErrors& errors, const Quantity value, std::string field)
{
    if (value <= 0) {
        errors.push_back({ValidationErrorCode::NonPositiveValue, std::move(field), "value must be positive"});
    }
}

void require_range(
    ValidationErrors& errors,
    const Quantity value,
    const Quantity minimum,
    const Quantity maximum,
    std::string field)
{
    if (value < minimum || value > maximum) {
        errors.push_back({
            ValidationErrorCode::ValueOutOfRange,
            std::move(field),
            "value is outside the official range",
        });
    }
}

void require_position(
    ValidationErrors& errors,
    const MapDefinition& map,
    const CellIndex position,
    std::string field)
{
    if (!map.contains(position)) {
        errors.push_back({ValidationErrorCode::OutOfRangeCell, std::move(field), "cell index is outside the map"});
    }
}

}  // namespace

ValidationErrors validate(const MatchConfig& config)
{
    ValidationErrors errors;
    require_range(errors, config.map.height(), 8, 32, "map.height");
    require_range(errors, config.map.width(), 8, 32, "map.width");
    if (config.day_seconds.empty() || config.day_steps.empty()) {
        errors.push_back({ValidationErrorCode::EmptySchedule, "day_seconds/day_steps", "day schedule must not be empty"});
    }
    if (config.day_seconds.size() != config.day_steps.size()) {
        errors.push_back({ValidationErrorCode::ScheduleSizeMismatch, "day_seconds/day_steps", "day schedule sizes must match"});
    }

    for (std::size_t i = 0; i < config.day_seconds.size(); ++i) {
        require_positive(errors, config.day_seconds[i], "day_seconds[" + std::to_string(i) + "]");
    }
    for (std::size_t i = 0; i < config.day_steps.size(); ++i) {
        require_positive(errors, config.day_steps[i], "day_steps[" + std::to_string(i) + "]");
    }
    for (std::size_t i = 0; i < config.spots.size(); ++i) {
        require_position(errors, config.map, config.spots[i].position, "spots[" + std::to_string(i) + "].position");
        require_positive(errors, config.spots[i].max_stock, "spots[" + std::to_string(i) + "].max_stock");
    }
    for (std::size_t i = 0; i < config.initial_agent_positions.size(); ++i) {
        require_position(errors, config.map, config.initial_agent_positions[i], "initial_agent_positions[" + std::to_string(i) + "]");
    }

    require_positive(errors, config.fuel_limit, "fuel_limit");
    require_positive(errors, config.players, "players");
    require_range(errors, config.busy_threshold, 1, 5, "busy_threshold");
    require_range(errors, config.jammed_threshold, 2, 10, "jammed_threshold");
    return errors;
}

ValidationErrors validate(const DailyState& state, const MatchConfig& config)
{
    ValidationErrors errors;
    for (std::size_t i = 0; i < state.own_agents.size(); ++i) {
        require_position(errors, config.map, state.own_agents[i].position, "own_agents[" + std::to_string(i) + "].position");
    }
    for (std::size_t team = 0; team < state.other_teams.size(); ++team) {
        for (std::size_t agent = 0; agent < state.other_teams[team].agents.size(); ++agent) {
            require_position(
                errors,
                config.map,
                state.other_teams[team].agents[agent].position,
                "other_teams[" + std::to_string(team) + "].agents[" + std::to_string(agent) + "].position");
        }
    }
    for (std::size_t i = 0; i < state.traffic.size(); ++i) {
        const auto field = "traffic[" + std::to_string(i) + "].position";
        require_position(errors, config.map, state.traffic[i].position, field);
        const auto terrain = config.map.terrain_at(state.traffic[i].position);
        if (terrain.has_value() && *terrain != Terrain::Road) {
            errors.push_back({ValidationErrorCode::InvalidTerrainCombination, field, "traffic state must refer to a road cell"});
        }
    }
    return errors;
}

}  // namespace hexa_udon::core
