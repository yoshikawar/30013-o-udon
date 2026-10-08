// Planner: 1 日の計画（初期解 → LNS ＋ 焼きなまし）（procon2026 の solvers/meet.cpp / common.hpp から移した。処理は元のまま）
#include "hexa_udon/solver.hpp"

namespace hexa_udon::solver {

struct Planner {
    const vector<Agent>& st;
    int steps;
    bool lastDay;
    Router router;
    vector<int> patrols, supplies;
    vector<vector<int>> brandSpots;
    vector<int> hubJump, hubCells;
    map<vector<int>, int> tomorrowMemo;
    double emittedLasting = -1e18;
    vector<vector<int>> emittedPlan;

    Planner(const vector<Agent>& st_, const vector<int>& status, int steps_, bool last)
        : st(st_), steps(steps_), lastDay(last), router(status), brandSpots(B) {
        for (int i = 0; i < NA; i++) (st[i].kind == 0 ? patrols : supplies).push_back(i);
        for (int s = 0; s < S; s++) brandSpots[spots[s].brand].push_back(s);
        for (int i : patrols) hubJump.push_back(st[i].pos);
        for (int s = 0; s < S; s++) hubJump.push_back(spots[s].pos);
        for (int c = 0; c < NC; c++) if (passable(c) && cellType[c] != ROAD) hubCells.push_back(c);
    }

    // 明日の朝の割り当てで入らない系列の数（道路の状態は今日のまま、補給車は今日の終わりの位置で待つとみなす）
    int tomorrowMisses(const vector<Agent>& end) {
        if (lastDay) return 0;
        vector<int> key;
        for (const Agent& a : end) { key.push_back(a.pos); key.push_back(a.fuel); }
        if (auto it = tomorrowMemo.find(key); it != tomorrowMemo.end()) return it->second;
        Day next(end, daySteps[today + 1], today + 1, router, patrols, supplies, brandSpots);
        vector<vector<pair<int, int>>> sch;
        for (int j : supplies) sch.push_back({{end[j].pos, INT_MAX}});
        next.setSchedule(sch);
        int missing;
        next.assignBrands(missing);
        return tomorrowMemo[key] = missing;
    }

    double total(const Day::Eval& e) { return e.score < -1e17 ? e.score : e.score - E_TOMORROW * tomorrowMisses(e.end); }
    double lastingTotal(const Day::Eval& e) { return e.lasting < -1e17 ? e.lasting : e.lasting - E_TOMORROW * tomorrowMisses(e.end); }

    // 動き終えた時刻の項を除いた評価が良くなったときだけ出し直すのは、今日の玉にも翌日にも効かない改善で出し直すと回答時間で負けるため
    void emit(const vector<vector<int>>& plan, double lasting) {
        if (lasting <= emittedLasting || lasting < -1e17) return;
        emitInterim(plan);
        emittedLasting = lasting;
        emittedPlan = plan;
    }

    // 補給車の予定の候補: 今の位置、密度の高いスポット、巡回車の位置で一日中待つ
    vector<vector<vector<pair<int, int>>>> scheduleCandidates() {
        vector<vector<vector<pair<int, int>>>> out;
        if (supplies.empty()) return {{}};
        vector<double> density(S, 0);
        int np = max<int>(1, patrols.size());
        for (int s = 0; s < S; s++) {
            const PathTable& pt = router.get(spots[s].pos, 0);
            for (int u = 0; u < S; u++)
                if (pt.t[spots[u].pos] != INT_MAX) density[s] += min(spots[u].stock, np) / (1.0 + pt.t[spots[u].pos] / 8.0);
        }
        vector<int> order(S);
        iota(order.begin(), order.end(), 0);
        sort(order.begin(), order.end(), [&](int a, int b) { return density[a] > density[b]; });
        vector<int> cells;
        for (int j : supplies) cells.push_back(st[j].pos);
        for (int s : order) cells.push_back(spots[s].pos);
        for (int i : patrols) cells.push_back(st[i].pos);
        vector<vector<pair<int, int>>> base;
        for (int j : supplies) base.push_back({{st[j].pos, INT_MAX}});
        out.push_back(base);
        for (size_t j = 0; j < supplies.size(); j++)
            for (int c : cells) {
                if (c == st[supplies[j]].pos) continue;
                auto sch = base;
                sch[j] = {{c, INT_MAX}};
                out.push_back(sch);
            }
        return out;
    }

    struct Solution { vector<vector<pair<int, int>>> schedule; vector<vector<int>> routes; double score = -1e18; vector<vector<int>> plan; double lasting = -1e18; };

    Solution build(Day& day, const vector<vector<pair<int, int>>>& sch) {
        Solution s{sch};
        day.setSchedule(sch);
        int missing;
        s.routes = day.fill(day.assignBrands(missing), 0);
        Day::Eval e = day.evaluate(s.routes);
        s.score = total(e);
        s.plan = e.plan;
        s.lasting = lastingTotal(e);
        return s;
    }

    // 補給車の待機場所を動かす: 半分は近くへ 1〜3 歩（道路は避ける）、残りは巡回車の位置かスポット、ランダムな平地・山地
    int movedCell(int c) {
        int r = rng.nextInt(4);
        if (r < 2) {
            for (int k = 1 + rng.nextInt(3); k > 0; k--) {
                int nx = neighbor(c, rng.nextInt(6));
                if (passable(nx) && cellType[nx] != ROAD) c = nx;
            }
            return c;
        }
        if (r == 2) return hubJump[rng.nextInt(hubJump.size())];
        return hubCells[rng.nextInt(hubCells.size())];
    }

    static const int OPS = 9;
    static constexpr const char* OP_NAMES[OPS] = {"remove", "patrol", "near", "relocate", "reverse", "orOpt", "refuel", "station", "split"};

    // 経路か補給車の予定を変える。何も変えられなければ false
    bool perturb(Day& day, vector<vector<int>>& routes, vector<vector<pair<int, int>>>& sch, int op) {
        int P = routes.size();
        vector<pair<int, int>> visits;
        for (int pi = 0; pi < P; pi++)
            for (int k = 0; k < (int)routes[pi].size(); k++) visits.push_back({pi, k});
        if (op <= 5 && visits.empty()) return false;
        if (op == 0) {
            int remove = 1 + rng.nextInt(max<int>(1, visits.size() / 5));
            for (int x = 0; x < remove; x++) swap(visits[x], visits[x + rng.nextInt(visits.size() - x)]);
            sort(visits.begin(), visits.begin() + remove, greater<>());
            for (int x = 0; x < remove; x++) routes[visits[x].first].erase(routes[visits[x].first].begin() + visits[x].second);
            return true;
        }
        auto [pi, k] = op <= 5 ? visits[rng.nextInt(visits.size())] : make_pair(rng.nextInt(max(1, P)), 0);
        if (op == 1) { routes[pi].clear(); return true; }
        if (op == 2) {
            const PathTable& pt = router.get(day.cellOf(routes[pi][k]), 0);
            int radius = steps / 16 + rng.nextInt(steps / 8 + 1);
            for (auto& r : routes) r.erase(remove_if(r.begin(), r.end(), [&](int v) { return v >= 0 && pt.t[spots[v].pos] <= radius; }), r.end());
            return true;
        }
        if (op == 3) {
            int v = routes[pi][k];
            if (P < 2 || v < 0) return false;
            int qi = (pi + 1 + rng.nextInt(P - 1)) % P;
            if (count(routes[qi].begin(), routes[qi].end(), v)) return false;
            routes[pi].erase(routes[pi].begin() + k);
            routes[qi].insert(routes[qi].begin() + rng.nextInt(routes[qi].size() + 1), v);
            return true;
        }
        if (op == 4) {
            if (routes[pi].size() < 2) return false;
            int j = rng.nextInt(routes[pi].size());
            reverse(routes[pi].begin() + min(k, j), routes[pi].begin() + max(k, j) + 1);
            return true;
        }
        if (op == 5) {
            vector<int>& r = routes[pi];
            int len = min<int>(1 + rng.nextInt(3), r.size() - k);
            vector<int> seg(r.begin() + k, r.begin() + k + len);
            r.erase(r.begin() + k, r.begin() + k + len);
            r.insert(r.begin() + rng.nextInt(r.size() + 1), seg.begin(), seg.end());
            return true;
        }
        if (P == 0) return false;
        if (op == 6) {
            vector<int>& r = routes[pi];
            auto it = find_if(r.begin(), r.end(), isRefuel);
            if (it != r.end() && rng.nextInt(2)) { r.erase(it); return true; }
            if (sch.empty()) return false;
            int j = rng.nextInt(sch.size()), kk = rng.nextInt(sch[j].size());
            r.insert(r.begin() + rng.nextInt(r.size() + 1), refuelNode(j, kk));
            return true;
        }
        if (sch.empty()) return false;
        int j = rng.nextInt(sch.size());
        if (op == 7) {
            int kk = rng.nextInt(sch[j].size());
            if (rng.nextInt(2) || sch[j].size() == 1) sch[j][kk].first = movedCell(sch[j][kk].first);
            else sch[j][kk].second = max(0, min(steps, sch[j][kk].second + (rng.nextInt(2) ? 1 : -1) * (1 + rng.nextInt(steps / 6 + 1))));
            return true;
        }
        // op == 8: 待機場所を足す（前半をどこかで待ってから今の場所へ）か、2 つ以上あれば 1 つ消す
        if (sch[j].size() >= 2 && rng.nextInt(2)) {
            int kk = rng.nextInt(sch[j].size());
            sch[j].erase(sch[j].begin() + kk);
            for (auto& r : routes) r.erase(remove_if(r.begin(), r.end(), [&](int v) { return isRefuel(v); }), r.end());
            return true;
        }
        if ((int)sch[j].size() >= MAX_STATIONS) return false;
        int c = hubJump[rng.nextInt(hubJump.size())];
        sch[j].insert(sch[j].begin(), {c, 1 + rng.nextInt(max(1, steps / 2))});
        for (auto& r : routes)
            for (int& v : r)
                if (isRefuel(v)) { int x = -1 - v; if (x / MAX_STATIONS == j) v = refuelNode(j, x % MAX_STATIONS + 1); }
        return true;
    }

    // 焼きなまし
    Solution improve(Solution cur, double timeMs) {
        Timer tm;
        Day day(st, steps, today, router, patrols, supplies, brandSpots);
        day.setSchedule(cur.schedule);
        Solution best = cur;
        double curScore = day.evaluate(cur.routes).score, bestRaw = curScore;
        vector<char> need = day.brandsOf(cur.routes);
        long long iter = 0, accepted = 0, tries[OPS] = {}, gains[OPS] = {};
        double lastEmit = 0, lastBest = 0;
        while (tm.ms() < timeMs) {
            if (interimSec > 0 && tm.ms() - lastEmit >= interimSec * 1000) { lastEmit = tm.ms(); emit(best.plan, best.lasting); }
            iter++;
            vector<vector<int>> next = cur.routes;
            auto sch = cur.schedule;
            int op = rng.nextInt(OPS);
            // しばらく一番良い解が更新されなければ、一番良い解から巡回車 2 台ぶんを壊してやり直す
            if (tm.ms() - lastBest > timeMs * 0.15) {
                cur = best;
                curScore = bestRaw;
                next = cur.routes;
                sch = cur.schedule;
                for (int x = 0; x < 2 && !next.empty(); x++) next[rng.nextInt(next.size())].clear();
                lastBest = tm.ms();
                op = 1;
            } else if (!perturb(day, next, sch, op)) continue;
            bool schChanged = sch != day.schedule;
            if (schChanged) day.setSchedule(sch);
            bool ok = day.repair(next, need, iter % 10 == 0, rng.nextInt(2) ? LNS_NOISE : 0, schChanged ? nullptr : &cur.routes);
            double v = -1e18;
            Day::Eval e;
            if (ok) { e = day.evaluate(next); v = e.score; }
            if (v > -1e17) { tries[op]++; if (v > curScore + 1e-9) gains[op]++; }
            double temp = LNS_T0 * pow(LNS_T1 / LNS_T0, tm.ms() / timeMs);
            if (v > -1e17 && (v >= curScore || rng.nextDouble() < exp((v - curScore) / temp))) {
                cur.routes = next; cur.schedule = sch; curScore = v; accepted++;
                need = day.brandsOf(cur.routes);
                if (v > best.score) {
                    double t = total(e);
                    if (t > best.score) { best = {sch, next, t, e.plan, lastingTotal(e)}; bestRaw = v; lastBest = tm.ms(); }
                }
            } else if (schChanged) day.setSchedule(cur.schedule);
        }
        cerr << "[solver] iter=" << iter << " accepted=" << accepted << " score=" << best.score;
        for (int op = 0; op < OPS; op++) cerr << " " << OP_NAMES[op] << "=" << gains[op] << "/" << tries[op];
        cerr << "\n";
        return best;
    }

    vector<vector<int>> plan(double timeMs) {
        Timer tm;
        Day day(st, steps, today, router, patrols, supplies, brandSpots);
        Solution best;
        auto cands = scheduleCandidates();
        if (choosingKinds && cands.size() > 8) cands.resize(8);
        for (auto& sch : cands) {
            if (best.score > -1e17 && tm.ms() > timeMs * 0.3) break;
            Solution s = build(day, sch);
            if (s.score > best.score) best = s;
        }
        if (best.score < -1e17) return allWait(steps);
        if (!choosingKinds) {
            emit(best.plan, best.lasting);
            best = improve(best, timeMs - tm.ms());
            if (interimSec > 0 && emittedLasting >= best.lasting) return emittedPlan;
        }
        return best.plan;
    }
};

vector<vector<int>> planDay(const vector<Agent>& st, const vector<int>& status, const vector<char>&,
                            int steps, bool lastDay, double timeMs) {
    Planner p(st, status, steps, lastDay);
    return p.plan(timeMs);
}

}  // namespace hexa_udon::solver
