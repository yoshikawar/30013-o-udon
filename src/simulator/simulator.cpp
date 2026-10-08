// 1 日のシミュレーション（ルール完全再現）（procon2026 の solvers/meet.cpp / common.hpp から移した。処理は元のまま）
#include "hexa_udon/solver.hpp"
#include "hexa_udon/simulator/simulator.hpp"

namespace hexa_udon::solver {

vector<int> statusFromStay(const vector<long long>& sumStay, int players) {
    vector<int> st(NC, 0);
    for (int p = 0; p < NC; p++) {
        if (cellType[p] != ROAD) continue;
        long long v = sumStay[p];
        st[p] = v < (long long)BUSY * players ? 0 : v < (long long)JAM * players ? 1 : 2;
    }
    return st;
}

bool checkStructure(const vector<Agent>& st, const vector<vector<int>>& plan, const vector<int>& status, int steps, string& err) {
    if ((int)plan.size() != (int)st.size()) { err = "エージェント数が違う"; return false; }
    for (size_t i = 0; i < st.size(); i++) {
        int cur = st[i].pos; long long tot = 0;
        if (plan[i].empty()) { err = "agent " + to_string(i) + ": 行動が空"; return false; }
        for (int a : plan[i]) {
            if (a < 0) tot += -(long long)a;
            else if (a <= 5) {
                int nx = neighbor(cur, a);
                if (!passable(nx)) { err = "agent " + to_string(i) + ": 移動不可能なセルへ移動"; return false; }
                tot += stepCost(cur, status);
                cur = nx;
            } else { err = "agent " + to_string(i) + ": 不正な値 " + to_string(a); return false; }
        }
        if (tot != steps) { err = "agent " + to_string(i) + ": ステップ数 " + to_string(tot) + " != " + to_string(steps); return false; }
    }
    return true;
}

DayResult simulateDay(const vector<Agent>& st, const vector<vector<int>>& plan, const vector<int>& status, int steps) {
    DayResult r;
    r.stay.assign(NC, 0);
    r.brandHit.assign(B, 0);
    r.end = st;
    if (!checkStructure(st, plan, status, steps, r.error)) { r.valid = false; return r; }
    int n = st.size();
    vector<Agent> a = st;
    vector<int> ptr(n, 0), doneAt(n, INT_MAX), target(n, -1), fcost(n, 0), stock(S);
    for (int s = 0; s < S; s++) stock[s] = spots[s].stock;
    vector<vector<char>> got(n, vector<char>(S, 0));
    auto startAction = [&](int i, int t) {
        if (ptr[i] >= (int)plan[i].size()) { doneAt[i] = INT_MAX; target[i] = -1; return; }
        int x = plan[i][ptr[i]++];
        if (x < 0) { doneAt[i] = t - x; target[i] = -1; }
        else { target[i] = neighbor(a[i].pos, x); fcost[i] = fuelCost(a[i].pos); doneAt[i] = t + stepCost(a[i].pos, status); }
    };
    for (int i = 0; i < n; i++) startAction(i, 0);
    for (int t = 1; t <= steps; t++) {
        // 1. 燃料の消費
        for (int i = 0; i < n; i++)
            if (doneAt[i] == t && target[i] >= 0 && a[i].kind == 0) {
                if (a[i].fuel < fcost[i]) {
                    r.valid = false;
                    r.error = "agent " + to_string(i) + ": step " + to_string(t) + " で燃料不足";
                    r.end = st; r.stay.assign(NC, 0); r.brandHit.assign(B, 0); r.balls = 0;
                    return r;
                }
                a[i].fuel -= fcost[i];
            }
        // 2. 移動の反映
        for (int i = 0; i < n; i++) if (doneAt[i] == t && target[i] >= 0) a[i].pos = target[i];
        // 3. うどんの獲得（番号順）
        for (int i = 0; i < n; i++) {
            if (a[i].kind != 0) continue;
            int s = spotAt[a[i].pos];
            if (s >= 0 && !got[i][s] && stock[s] > 0) { got[i][s] = 1; stock[s]--; r.balls++; r.brandHit[spots[s].brand] = 1; }
        }
        // 4. 燃料の補給
        for (int i = 0; i < n; i++) {
            if (a[i].kind != 0) continue;
            for (int j = 0; j < n; j++) if (a[j].kind == 1 && a[j].pos == a[i].pos) { a[i].fuel = FUEL_LIMIT; break; }
        }
        // 5. 交通量の更新
        for (int i = 0; i < n; i++) if (cellType[a[i].pos] == ROAD) r.stay[a[i].pos]++;
        // アクションフェーズ
        if (t < steps) for (int i = 0; i < n; i++) if (doneAt[i] == t) startAction(i, t);
    }
    r.end = a;
    for (int b = 0; b < B; b++) r.brands += r.brandHit[b];
    return r;
}

vector<vector<int>> allWait(int steps) { return vector<vector<int>>(NA, vector<int>{-steps}); }

}  // namespace hexa_udon::solver

namespace hexa_udon::simulator {

SimulationOutcome SimulationOutcome::success(DaySimulationResult result) { return SimulationOutcome{std::move(result)}; }
SimulationOutcome SimulationOutcome::failure(SimulationError error) { return SimulationOutcome{std::move(error)}; }
bool SimulationOutcome::has_value() const noexcept { return std::holds_alternative<DaySimulationResult>(storage_); }
SimulationOutcome::operator bool() const noexcept { return has_value(); }
const DaySimulationResult& SimulationOutcome::value() const& { return std::get<DaySimulationResult>(storage_); }
DaySimulationResult&& SimulationOutcome::value() && { return std::get<DaySimulationResult>(std::move(storage_)); }
const SimulationError& SimulationOutcome::error() const& { return std::get<SimulationError>(storage_); }
SimulationOutcome::SimulationOutcome(std::variant<DaySimulationResult, SimulationError> storage) : storage_(std::move(storage)) {}

SimulationOutcome simulate_day(const DaySimulationInput& input, const DayActionPlan& plan) {
    namespace m = solver;
    // 計画中（途中の計画の提出）にも呼ばれるので、同じ盤面なら入れ直さない
    if (m::W != input.map.width() || m::H != input.map.height() || m::S != (int)input.spots.size() || m::FUEL_LIMIT != input.fuel_limit)
        m::loadMap(input.map, input.spots, input.fuel_limit);
    std::vector<core::TrafficState> traffic(input.traffic.begin(), input.traffic.end());
    std::vector<core::AgentState> agents(input.agents.begin(), input.agents.end());
    const auto r = m::simulateDay(m::agentsOf(agents), m::fromActionPlan(plan), m::statusOf(traffic), input.day_steps);
    if (!r.valid) {
        const bool fuel = r.error.find("燃料不足") != std::string::npos;
        return SimulationOutcome::failure({fuel ? SimulationErrorCode::InsufficientFuel : SimulationErrorCode::InvalidActionValue,
                                           r.error, std::nullopt, std::nullopt, std::nullopt});
    }
    DaySimulationResult result;
    for (const auto& a : r.end) {
        result.end_agents.push_back({static_cast<core::AgentKind>(a.kind), core::CellIndex{a.pos}, a.fuel});
    }
    for (int b = 0; b < m::B; b++) if (r.brandHit[b]) result.distinct_brands.insert(m::rawBrand[b]);
    result.total_balls = r.balls;
    return SimulationOutcome::success(std::move(result));
}

MatchProgress accumulate_progress(const MatchProgress& previous, const DaySimulationResult& day_result) {
    MatchProgress next = previous;
    next.acquired_brands.insert(day_result.distinct_brands.begin(), day_result.distinct_brands.end());
    next.total_balls += day_result.total_balls;
    next.daily_distinct_brand_counts.push_back(static_cast<core::Quantity>(day_result.distinct_brands.size()));
    return next;
}

OfficialScore official_score(const MatchProgress& previous, const DaySimulationResult& day) {
    auto brands = previous.acquired_brands;
    brands.insert(day.distinct_brands.begin(), day.distinct_brands.end());
    const auto daily_sum = std::accumulate(previous.daily_distinct_brand_counts.begin(),
                                           previous.daily_distinct_brand_counts.end(), std::int64_t{0});
    return {static_cast<std::int64_t>(brands.size()), daily_sum + static_cast<std::int64_t>(day.distinct_brands.size()),
            static_cast<std::int64_t>(previous.total_balls) + day.total_balls};
}

bool better_official_score(const OfficialScore& left, const OfficialScore& right) noexcept {
    return std::tie(left.total_unique_brands, left.cumulative_daily_unique_brands, left.total_bowls)
         > std::tie(right.total_unique_brands, right.cumulative_daily_unique_brands, right.total_bowls);
}

}  // namespace hexa_udon::simulator
