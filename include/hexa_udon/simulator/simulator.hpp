#pragma once
// session・client が使う 1 日のシミュレーション。中身は solver の simulateDay（src/simulator/simulator.cpp）
#include "hexa_udon/core/map_definition.hpp"
#include "hexa_udon/core/models.hpp"
#include "hexa_udon/simulator/action.hpp"

#include <cstdint>
#include <set>
#include <span>
#include <variant>
#include <vector>

namespace hexa_udon::simulator {

struct DaySimulationInput {
    const core::MapDefinition& map;
    std::span<const core::Spot> spots;
    core::Quantity fuel_limit;
    core::Quantity day_steps;
    std::span<const core::AgentState> agents;
    std::span<const core::TrafficState> traffic;
};

struct DaySimulationResult {
    std::vector<core::AgentState> end_agents;
    std::set<core::Quantity> distinct_brands;
    core::Quantity total_balls;
};

struct MatchProgress {
    std::set<core::Quantity> acquired_brands;
    core::Quantity total_balls = 0;
    std::vector<core::Quantity> daily_distinct_brand_counts;
};

class SimulationOutcome {
public:
    [[nodiscard]] static SimulationOutcome success(DaySimulationResult result);
    [[nodiscard]] static SimulationOutcome failure(SimulationError error);

    [[nodiscard]] bool has_value() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] const DaySimulationResult& value() const&;
    [[nodiscard]] DaySimulationResult&& value() &&;
    [[nodiscard]] const SimulationError& error() const&;

private:
    explicit SimulationOutcome(std::variant<DaySimulationResult, SimulationError> storage);
    std::variant<DaySimulationResult, SimulationError> storage_;
};

[[nodiscard]] SimulationOutcome simulate_day(const DaySimulationInput& input, const DayActionPlan& plan);

[[nodiscard]] MatchProgress accumulate_progress(const MatchProgress& previous, const DaySimulationResult& day_result);

// 公式の順位の付け方（総系列 → 日別系列の合計 → 玉）
struct OfficialScore {
    std::int64_t total_unique_brands = 0;
    std::int64_t cumulative_daily_unique_brands = 0;
    std::int64_t total_bowls = 0;

    [[nodiscard]] bool operator==(const OfficialScore&) const = default;
};

[[nodiscard]] OfficialScore official_score(const MatchProgress& previous, const DaySimulationResult& day);
[[nodiscard]] bool better_official_score(const OfficialScore& left, const OfficialScore& right) noexcept;

}  // namespace hexa_udon::simulator
