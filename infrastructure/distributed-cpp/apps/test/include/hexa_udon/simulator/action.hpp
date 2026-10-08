#pragma once

#include "hexa_udon/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace hexa_udon::simulator {

struct MoveAction {
    core::Direction direction;

    [[nodiscard]] bool operator==(const MoveAction&) const = default;
};

struct WaitAction {
    core::Quantity steps;

    [[nodiscard]] bool operator==(const WaitAction&) const = default;
};

using Action = std::variant<MoveAction, WaitAction>;
using AgentActionPlan = std::vector<Action>;
using DayActionPlan = std::vector<AgentActionPlan>;
using RawDayActionPlan = std::vector<std::vector<std::int32_t>>;

enum class SimulationErrorCode {
    AgentCountMismatch,
    InvalidActionValue,
    InvalidWaitDuration,
    WaitDurationOverflow,
    ActionTimeInsufficient,
    ActionTimeExceeded,
    OutOfBoundsMove,
    MoveIntoPond,
    InsufficientFuel,
    IncompleteMove,
    InvalidInputState,
    MissingRoadStatus,
    InvalidTrafficState,
};

struct SimulationError {
    SimulationErrorCode code;
    std::string message;
    std::optional<std::size_t> agent_index;
    std::optional<std::size_t> command_index;
    std::optional<core::Quantity> step;

    [[nodiscard]] bool operator==(const SimulationError&) const = default;
};

class ActionPlanParseResult {
public:
    [[nodiscard]] static ActionPlanParseResult success(DayActionPlan plan);
    [[nodiscard]] static ActionPlanParseResult failure(SimulationError error);

    [[nodiscard]] bool has_value() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] const DayActionPlan& value() const&;
    [[nodiscard]] DayActionPlan&& value() &&;
    [[nodiscard]] const SimulationError& error() const&;

private:
    explicit ActionPlanParseResult(std::variant<DayActionPlan, SimulationError> storage);

    std::variant<DayActionPlan, SimulationError> storage_;
};

[[nodiscard]] ActionPlanParseResult parse_action_plan(const RawDayActionPlan& raw_plan);

}  // namespace hexa_udon::simulator
