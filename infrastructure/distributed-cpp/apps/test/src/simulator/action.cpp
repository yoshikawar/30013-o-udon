#include "hexa_udon/simulator/action.hpp"

#include <limits>
#include <utility>

namespace hexa_udon::simulator {

ActionPlanParseResult::ActionPlanParseResult(
    std::variant<DayActionPlan, SimulationError> storage)
    : storage_(std::move(storage))
{
}

ActionPlanParseResult ActionPlanParseResult::success(DayActionPlan plan)
{
    return ActionPlanParseResult(std::move(plan));
}

ActionPlanParseResult ActionPlanParseResult::failure(SimulationError error)
{
    return ActionPlanParseResult(std::move(error));
}

bool ActionPlanParseResult::has_value() const noexcept
{
    return std::holds_alternative<DayActionPlan>(storage_);
}

ActionPlanParseResult::operator bool() const noexcept
{
    return has_value();
}

const DayActionPlan& ActionPlanParseResult::value() const&
{
    return std::get<DayActionPlan>(storage_);
}

DayActionPlan&& ActionPlanParseResult::value() &&
{
    return std::get<DayActionPlan>(std::move(storage_));
}

const SimulationError& ActionPlanParseResult::error() const&
{
    return std::get<SimulationError>(storage_);
}

ActionPlanParseResult parse_action_plan(const RawDayActionPlan& raw_plan)
{
    DayActionPlan result;
    result.reserve(raw_plan.size());
    for (std::size_t agent_index = 0; agent_index < raw_plan.size(); ++agent_index) {
        AgentActionPlan actions;
        actions.reserve(raw_plan[agent_index].size());
        for (std::size_t command_index = 0;
             command_index < raw_plan[agent_index].size();
             ++command_index) {
            const auto raw = raw_plan[agent_index][command_index];
            if (raw >= 0 && raw <= 5) {
                const auto direction = core::direction_from_int(raw);
                if (!direction.has_value()) {
                    return ActionPlanParseResult::failure({
                        SimulationErrorCode::InvalidActionValue,
                        "movement direction is invalid",
                        agent_index,
                        command_index,
                        std::nullopt,
                    });
                }
                actions.emplace_back(MoveAction{*direction});
                continue;
            }
            if (raw == std::numeric_limits<std::int32_t>::min()) {
                return ActionPlanParseResult::failure({
                    SimulationErrorCode::WaitDurationOverflow,
                    "wait duration cannot be represented as int32",
                    agent_index,
                    command_index,
                    std::nullopt,
                });
            }
            if (raw < 0) {
                actions.emplace_back(WaitAction{-raw});
                continue;
            }
            return ActionPlanParseResult::failure({
                SimulationErrorCode::InvalidActionValue,
                "action must be a direction from 0 to 5 or a negative wait duration",
                agent_index,
                command_index,
                std::nullopt,
            });
        }
        result.push_back(std::move(actions));
    }
    return ActionPlanParseResult::success(std::move(result));
}

}  // namespace hexa_udon::simulator
