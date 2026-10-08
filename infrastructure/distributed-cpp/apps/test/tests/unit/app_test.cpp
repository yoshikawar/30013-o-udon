#include "hexa_udon/solver.hpp"
#include "hexa_udon/simulator/simulator.hpp"

#include <utility>
#include <vector>

// solver を o-udon の型で動かしたときの最低限の確認
int main() {
    using namespace hexa_udon;
    auto map = std::move(core::MapDefinition::create(8, 8,
        std::vector<core::Terrain>(64, core::Terrain::Plain)).value());
    // 系列番号は 0 始まりでなくても、シミュレーションの結果は元の番号で返す
    const core::MatchConfig match{0, {60, 60}, {40, 40}, map,
        {{7, {9}, 2}, {11, {30}, 1}, {7, {45}, 1}},
        {{0}, {2}, {1}}, 20, 1, 1, 2};
    solver::loadProblem(match);
    if (solver::B != 2 || solver::rawBrand != std::vector<int>{7, 11}) return 1;

    // solver の方向は o-udon の盤の隣接と同じ
    for (std::int32_t cell = 0; cell < 64; ++cell)
        for (int d = 0; d < 6; ++d) {
            const auto expected = map.neighbor({cell}, static_cast<core::Direction>(d));
            if (solver::neighbor(cell, d) != (expected ? expected->value : -1)) return 2;
        }

    // 巡回車 0 が右下に動いてスポット 9 に着く。補給車 2 も左下に動いて 9 に来るので、燃料は満タンに戻る
    const std::vector<core::AgentState> agents{
        {core::AgentKind::Patrol, {0}, 20}, {core::AgentKind::Patrol, {2}, 20}, {core::AgentKind::Supply, {1}, 20}};
    const simulator::DaySimulationInput input{map, match.spots, match.fuel_limit, 40, agents, {}};
    const simulator::DayActionPlan plan{
        {simulator::MoveAction{core::Direction::LowerRight}, simulator::WaitAction{38}},
        {simulator::WaitAction{40}},
        {simulator::MoveAction{core::Direction::LowerLeft}, simulator::WaitAction{38}}};
    const auto result = simulator::simulate_day(input, plan);
    if (!result) return 3;
    if (result.value().total_balls != 1 || result.value().distinct_brands != std::set<core::Quantity>{7}) return 4;
    if (result.value().end_agents[0].position.value != 9 || result.value().end_agents[0].fuel != 20) return 5;
    const auto score = simulator::official_score({}, result.value());
    if (score.total_unique_brands != 1 || score.cumulative_daily_unique_brands != 1 || score.total_bowls != 1) return 6;
    const auto progress = simulator::accumulate_progress({}, result.value());
    if (progress.total_balls != 1 || progress.daily_distinct_brand_counts != std::vector<core::Quantity>{1}) return 7;

    // 燃料が足りない移動は弾く
    const std::vector<core::AgentState> empty{
        {core::AgentKind::Patrol, {0}, 0}, {core::AgentKind::Patrol, {2}, 20}, {core::AgentKind::Supply, {1}, 20}};
    const simulator::DaySimulationInput empty_input{map, match.spots, match.fuel_limit, 40, empty, {}};
    if (simulator::simulate_day(empty_input, plan)) return 8;

    // solver の計画は厳密なシミュレーションを通り、系列を全部取る
    solver::today = 0;
    const auto planned = solver::planDay(solver::agentsOf(agents), solver::statusOf({}), std::vector<char>(solver::B, 0),
                                       40, false, 200.0);
    const auto checked = simulator::simulate_day(input, solver::toActionPlan(planned));
    if (!checked || checked.value().distinct_brands.size() != 2) return 9;
    if (solver::fromActionPlan(solver::toActionPlan(planned)) != planned) return 10;

    // 種別決めは車の数だけ種別を返す
    const auto kinds = solver::solveKind(300.0);
    solver::choosingKinds = false;
    if (kinds.size() != 3) return 11;
    return 0;
}
