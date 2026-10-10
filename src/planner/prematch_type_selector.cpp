// 種別決め: 種別の候補ごとに全日程を回して比べる（procon2026 の solvers/meet.cpp / common.hpp から移した。処理は元のまま）
#include "hexa_udon/solver.hpp"

namespace hexa_udon::solver {

vector<vector<int>> kindsWithSupplies(int k) {
    vector<vector<int>> out;
    for (int mask = 0; mask < (1 << NA); mask++) {
        if (__builtin_popcount(mask) != k) continue;
        vector<int> kinds(NA, 0);
        for (int i = 0; i < NA; i++) if (mask >> i & 1) kinds[i] = 1;
        out.push_back(kinds);
    }
    return out;
}

vector<vector<int>> kindCandidates() {
    vector<int> status0(NC, 0);
    Router router(status0);
    vector<long long> spread(NA, 0);
    for (int i = 0; i < NA; i++)
        for (int s = 0; s < S; s++) spread[i] += router.get(agentStart[i], 0).t[spots[s].pos];
    auto spreadOf = [&](const vector<int>& kinds) {
        long long c = 0;
        for (int i = 0; i < NA; i++) if (kinds[i]) c += spread[i];
        return c;
    };
    auto byCentrality = [&](vector<vector<int>> v) {
        stable_sort(v.begin(), v.end(), [&](auto& a, auto& b) { return spreadOf(a) < spreadOf(b); });
        return v;
    };
    bool tight = *max_element(daySteps.begin(), daySteps.end()) >= FUEL_TIGHT * FUEL_LIMIT;
    vector<vector<int>> singles = byCentrality(kindsWithSupplies(1)), multi;
    for (int m = 2; m <= min(3, NA / 2); m++) {
        vector<vector<int>> combos = byCentrality(kindsWithSupplies(m));
        multi.insert(multi.end(), combos.begin(), combos.begin() + (tight ? combos.size() : 1));
    }
    vector<vector<int>> cands;
    size_t head = tight ? min<size_t>(2, singles.size()) : singles.size();
    cands.insert(cands.end(), singles.begin(), singles.begin() + head);
    cands.insert(cands.end(), multi.begin(), multi.end());
    cands.insert(cands.end(), singles.begin() + head, singles.end());
    cands.push_back(vector<int>(NA, 0));
    return cands;
}

MatchScore simulateMatch(const vector<int>& kinds, double timeMs, bool busy) {
    vector<Agent> st(NA);
    for (int i = 0; i < NA; i++) st[i] = {kinds[i], agentStart[i], FUEL_LIMIT};
    vector<char> collected(B, 0);
    vector<vector<long long>> stays;
    MatchScore ms;
    for (int d = 0; d < D; d++) {
        vector<long long> sum(NC, 0);
        for (int k = max(0, d - 2); k < d; k++) for (int p = 0; p < NC; p++) sum[p] += stays[k][p];
        vector<int> status = statusFromStay(sum, 1);
        if (busy && d > 0)
            for (int c = 0; c < NC; c++) if (cellType[c] == ROAD) status[c] = max(status[c], 1);
        today = d;
        auto plan = planDay(st, status, collected, daySteps[d], d == D - 1, timeMs / D);
        DayResult r = simulateDay(st, plan, status, daySteps[d]);
        if (!r.valid) r = simulateDay(st, allWait(daySteps[d]), status, daySteps[d]);
        for (int b = 0; b < B; b++) collected[b] |= r.brandHit[b];
        ms.dayBrands += r.brands; ms.balls += r.balls;
        stays.push_back(r.stay);
        st = r.end;
    }
    for (int b = 0; b < B; b++) ms.matchBrands += collected[b];
    return ms;
}

vector<int> solveKind(double timeMs) {
    choosingKinds = true;
    auto cands = kindCandidates();
    Timer tm;
    vector<pair<MatchScore, vector<int>>> scored;
    for (size_t c = 0; c < cands.size(); c++) {
        double remain = timeMs - tm.ms();
        if (c > 0 && remain <= 0) { cerr << "[kind] 時間切れ: 残り " << cands.size() - c << " 候補を省略\n"; break; }
        const double each = max(1.0, remain / (cands.size() - c)) / 2;
        MatchScore smooth = simulateMatch(cands[c], each), heavy = simulateMatch(cands[c], each, true);
        MatchScore ms{min(smooth.matchBrands, heavy.matchBrands), min(smooth.dayBrands, heavy.dayBrands),
                      smooth.balls + heavy.balls};
        cerr << "[kind] cand=" << c << " (";
        for (int k : cands[c]) cerr << k;
        cerr << ") brands=" << ms.matchBrands << " dayBrands=" << ms.dayBrands << " balls=" << ms.balls << "\n";
        scored.push_back({ms, cands[c]});
    }
    // 総系列が一番多い候補のうち、日別系列が最大から 1 以内のものは同点とみなし、玉で選ぶ。
    // 日別系列の見込みは種別決めの中の短い計画で出すので ±1 は揺れる。その差で補給車を増やすと、
    // 巡回車が減った分だけ玉を損するため（練習場の 32x32 で、補給車 2 台を選んで玉が 4% 少なかった）
    int topBrands = INT_MIN, topDay = INT_MIN;
    for (auto& [ms, k] : scored) topBrands = max(topBrands, ms.matchBrands);
    for (auto& [ms, k] : scored) if (ms.matchBrands == topBrands) topDay = max(topDay, ms.dayBrands);
    const pair<MatchScore, vector<int>>* pick = nullptr;
    for (auto& s : scored) {
        const MatchScore& ms = s.first;
        if (ms.matchBrands != topBrands || ms.dayBrands < topDay - KIND_DAY_TOLERANCE) continue;
        if (!pick || tie(ms.balls, ms.dayBrands) > tie(pick->first.balls, pick->first.dayBrands)) pick = &s;
    }
    return pick ? pick->second : cands[0];
}

}  // namespace hexa_udon::solver
