#include "hexa_udon/protocol/json_codec.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace hexa_udon::protocol {
namespace {

using Json = nlohmann::json;

class CoreValidationFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

template <typename T>
T integer(const Json& object, const char* key) {
    const auto& value = object.at(key);
    if (!value.is_number_integer()) {
        throw std::runtime_error(std::string{key} + " must be integer");
    }
    return value.get<T>();
}

const Json& array(const Json& object, const char* key) {
    const auto& value = object.at(key);
    if (!value.is_array()) {
        throw std::runtime_error(std::string{key} + " must be array");
    }
    return value;
}

std::vector<core::Quantity> integer_array(const Json& object, const char* key) {
    std::vector<core::Quantity> result;
    for (const auto& value : array(object, key)) {
        if (!value.is_number_integer()) {
            throw std::runtime_error(std::string{key} + " elements must be integers");
        }
        result.push_back(value.get<core::Quantity>());
    }
    return result;
}

core::AgentState decode_agent(const Json& json) {
    const auto kind = core::agent_kind_from_int(integer<std::int32_t>(json, "kind"));
    if (!kind) {
        throw std::runtime_error("invalid agent kind");
    }
    return {*kind,
            {integer<core::Quantity>(json, "pos")},
            integer<core::Quantity>(json, "fuel")};
}

template <typename T, typename Function>
Result<T> parse(const std::string& text, Function function) {
    try {
        return Result<T>::success(function(Json::parse(text)));
    } catch (const Json::parse_error& error) {
        return Result<T>::failure({ErrorCode::InvalidJson, error.what()});
    } catch (const CoreValidationFailure& error) {
        return Result<T>::failure({ErrorCode::CoreValidation, error.what()});
    } catch (const std::exception& error) {
        return Result<T>::failure({ErrorCode::InvalidSchema, error.what()});
    }
}

void require_valid(const core::ValidationErrors& errors) {
    if (!errors.empty()) {
        throw CoreValidationFailure(errors.front().field + ": " + errors.front().message);
    }
}

}  // namespace

Result<core::MatchConfig> decode_match_setting(const std::string& text) {
    return parse<core::MatchConfig>(text, [](const Json& json) {
        const auto& map_json = json.at("map");
        const auto height = integer<core::GridSize>(map_json, "height");
        const auto width = integer<core::GridSize>(map_json, "width");
        const auto& rows = array(map_json, "cells");
        if (height <= 0 || rows.size() != static_cast<std::size_t>(height)) {
            throw std::runtime_error("cells height mismatch");
        }
        std::vector<core::Terrain> cells;
        for (const auto& row : rows) {
            if (!row.is_array() || width <= 0 || row.size() != static_cast<std::size_t>(width)) {
                throw std::runtime_error("cells width mismatch");
            }
            for (const auto& value : row) {
                if (!value.is_number_integer()) {
                    throw std::runtime_error("cell enum must be integer");
                }
                const auto terrain = core::terrain_from_int(value.get<std::int32_t>());
                if (!terrain) {
                    throw std::runtime_error("invalid terrain");
                }
                cells.push_back(*terrain);
            }
        }
        auto map = core::MapDefinition::create(height, width, std::move(cells));
        if (!map) {
            throw CoreValidationFailure(map.errors().front().message);
        }

        core::MatchConfig config{integer<core::UnixTimestamp>(json, "startsAt"),
                                 integer_array(json, "daySeconds"),
                                 integer_array(json, "daySteps"),
                                 std::move(map).value(),
                                 {},
                                 {},
                                 integer<core::Quantity>(json, "fuelLimits"),
                                 integer<core::Quantity>(json, "players"),
                                 integer<core::Quantity>(json, "busyThreshold"),
                                 integer<core::Quantity>(json, "jammedThreshold")};
        for (const auto& spot : array(json, "spots")) {
            config.spots.push_back({integer<core::Quantity>(spot, "brand"),
                                    {integer<core::Quantity>(spot, "pos")},
                                    integer<core::Quantity>(spot, "stocks")});
        }
        for (const auto& position : array(json, "agents")) {
            if (!position.is_number_integer()) {
                throw std::runtime_error("agent position must be integer");
            }
            config.initial_agent_positions.push_back({position.get<core::Quantity>()});
        }
        require_valid(core::validate(config));
        return config;
    });
}

Result<core::DailyState> decode_match_state(
    const std::string& text, const core::MatchConfig& config) {
    return parse<core::DailyState>(text, [&config](const Json& json) {
        core::DailyState state{integer<core::UnixTimestamp>(json, "endsAt"),
                               integer<core::Quantity>(json, "day"),
                               {},
                               {},
                               {}};
        for (const auto& agent : array(json, "agents")) {
            state.own_agents.push_back(decode_agent(agent));
        }
        for (const auto& other : array(json, "others")) {
            core::OtherTeamState team{integer<std::int32_t>(other, "id"), {}};
            for (const auto& agent : array(other, "agents")) {
                team.agents.push_back(decode_agent(agent));
            }
            state.other_teams.push_back(std::move(team));
        }
        for (const auto& traffic : array(json, "traffics")) {
            const auto status =
                core::road_status_from_int(integer<std::int32_t>(traffic, "status"));
            if (!status) {
                throw std::runtime_error("invalid traffic status");
            }
            state.traffic.push_back(
                {{integer<core::Quantity>(traffic, "pos")}, *status});
        }
        require_valid(core::validate(state, config));
        return state;
    });
}

Result<std::int32_t> decode_revision(const std::string& text) {
    try {
        const auto json = Json::parse(text);
        if (!json.contains("revision")) {
            return Result<std::int32_t>::failure(
                {ErrorCode::MissingRevision, "revision missing"});
        }
        const auto revision = integer<std::int32_t>(json, "revision");
        if (revision < 0) {
            return Result<std::int32_t>::failure(
                {ErrorCode::RejectedRevision, "negative revision"});
        }
        return Result<std::int32_t>::success(revision);
    } catch (const Json::parse_error& error) {
        return Result<std::int32_t>::failure({ErrorCode::InvalidJson, error.what()});
    } catch (const std::exception& error) {
        return Result<std::int32_t>::failure({ErrorCode::InvalidSchema, error.what()});
    }
}

Result<std::string> encode_agent_kinds(const std::vector<core::AgentKind>& kinds) {
    Json json = Json::array();
    for (const auto kind : kinds) {
        if (!core::agent_kind_from_int(core::to_int(kind))) {
            return Result<std::string>::failure(
                {ErrorCode::InvalidSchema, "invalid agent kind"});
        }
        json.push_back(core::to_int(kind));
    }
    return Result<std::string>::success(json.dump());
}

Result<std::string> encode_actions(const simulator::DayActionPlan& plan) {
    Json json = Json::array();
    for (const auto& agent_plan : plan) {
        Json row = Json::array();
        for (const auto& action : agent_plan) {
            if (const auto* move = std::get_if<simulator::MoveAction>(&action)) {
                if (!core::direction_from_int(core::to_int(move->direction))) {
                    return Result<std::string>::failure(
                        {ErrorCode::InvalidSchema, "invalid direction"});
                }
                row.push_back(core::to_int(move->direction));
            } else {
                const auto steps = std::get<simulator::WaitAction>(action).steps;
                if (steps <= 0) {
                    return Result<std::string>::failure(
                        {ErrorCode::InvalidSchema, "invalid wait"});
                }
                row.push_back(-steps);
            }
        }
        json.push_back(std::move(row));
    }
    return Result<std::string>::success(json.dump());
}

}  // namespace hexa_udon::protocol
