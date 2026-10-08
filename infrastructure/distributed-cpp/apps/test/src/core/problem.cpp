// solver の問題（大域変数）と、o-udon の型との変換
#include "hexa_udon/solver.hpp"

namespace hexa_udon::solver {

Rng rng;
int H, W, NC;
vector<int> cellType;
vector<Spot> spots;
vector<int> rawBrand;
int S, B;
vector<int> spotAt;
int NA;
vector<int> agentStart;
int FUEL_LIMIT, D, PLAYERS, BUSY, JAM;
vector<int> daySteps, daySeconds;
int today;
bool choosingKinds;
double interimSec;
function<void(const vector<vector<int>>&)> interimSink;

void emitInterim(const vector<vector<int>>& plan) {
    if (interimSec <= 0 || choosingKinds || !interimSink) return;
    interimSink(plan);
}

void loadMap(const core::MapDefinition& map, span<const core::Spot> mapSpots, int fuelLimit) {
    H = map.height();
    W = map.width();
    NC = H * W;
    cellType.assign(NC, 0);
    for (int c = 0; c < NC; c++) cellType[c] = core::to_int(map.cells()[c]);
    S = mapSpots.size();
    spots.resize(S);
    rawBrand.clear();
    for (const auto& s : mapSpots) rawBrand.push_back(s.brand);
    sort(rawBrand.begin(), rawBrand.end());
    rawBrand.erase(unique(rawBrand.begin(), rawBrand.end()), rawBrand.end());
    B = rawBrand.size();
    for (int i = 0; i < S; i++) {
        const auto& s = mapSpots[i];
        spots[i] = {(int)(lower_bound(rawBrand.begin(), rawBrand.end(), s.brand) - rawBrand.begin()), s.position.value, s.max_stock};
    }
    spotAt.assign(NC, -1);
    for (int i = 0; i < S; i++) spotAt[spots[i].pos] = i;
    FUEL_LIMIT = fuelLimit;
}

void loadProblem(const core::MatchConfig& match) {
    loadMap(match.map, match.spots, match.fuel_limit);
    NA = match.initial_agent_positions.size();
    agentStart.clear();
    for (const auto& p : match.initial_agent_positions) agentStart.push_back(p.value);
    D = match.day_steps.size();
    daySteps.assign(match.day_steps.begin(), match.day_steps.end());
    daySeconds.assign(match.day_seconds.begin(), match.day_seconds.end());
    PLAYERS = match.players;
    BUSY = match.busy_threshold;
    JAM = match.jammed_threshold;
}

vector<Agent> agentsOf(const vector<core::AgentState>& agents) {
    vector<Agent> st;
    for (const auto& a : agents) st.push_back({core::to_int(a.kind), a.position.value, a.fuel});
    return st;
}

vector<int> statusOf(const vector<core::TrafficState>& traffic) {
    vector<int> st(NC, 0);
    for (const auto& t : traffic) st[t.position.value] = core::to_int(t.status);
    return st;
}

simulator::DayActionPlan toActionPlan(const vector<vector<int>>& plan) {
    simulator::DayActionPlan out(plan.size());
    for (size_t i = 0; i < plan.size(); i++)
        for (int a : plan[i]) {
            if (a < 0) out[i].push_back(simulator::WaitAction{-a});
            else out[i].push_back(simulator::MoveAction{static_cast<core::Direction>(a)});
        }
    return out;
}

vector<vector<int>> fromActionPlan(const simulator::DayActionPlan& plan) {
    vector<vector<int>> out(plan.size());
    for (size_t i = 0; i < plan.size(); i++)
        for (const auto& a : plan[i]) {
            if (const auto* m = get_if<simulator::MoveAction>(&a)) out[i].push_back(core::to_int(m->direction));
            else out[i].push_back(-get<simulator::WaitAction>(a).steps);
        }
    return out;
}

}  // namespace hexa_udon::solver
