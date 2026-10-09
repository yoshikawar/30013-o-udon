// Day: 1 日の解の組み立てと評価（procon2026 の solvers/meet.cpp / common.hpp から移した。処理は元のまま）
#include "hexa_udon/solver.hpp"

namespace hexa_udon::solver {

Day::Day(const vector<Agent>& st_, int steps_, int dayIndex_, Router& r, const vector<int>& patrolIds_,
    const vector<int>& supplyIds_, const vector<vector<int>>& brandSpots_)
    : st(st_), steps(steps_), dayIndex(dayIndex_), lastDay(dayIndex_ == D - 1), router(r), patrolIds(patrolIds_),
      supplyIds(supplyIds_), brandSpots(brandSpots_), P(patrolIds_.size()), startReserved(S, 0), startBrand(B, 0),
      fastMemo(NC, nullptr), cheapMemo(NC, nullptr), insertMemo(P), actsMemo(P) {
    for (int i : patrolIds) {
        startFuel.push_back(st[i].fuel - fuelToKeep(st[i].fuel));
        int s = spotAt[st[i].pos];
        if (s >= 0 && startReserved[s] < spots[s].stock) { startReserved[s]++; startBrand[spots[s].brand] = 1; }
        startSpot.push_back(s);
    }
}

// 補給車なしの場合のための燃料キープ処理
int Day::fuelToKeep(int fuel) const {
    if (!supplyIds.empty() || lastDay) return 0;
    long long rest = 0;
    for (int d = dayIndex + 1; d < D; d++) rest += daySteps[d];
    return fuel * rest / (rest + steps);
}

const PathTable& Day::fast(int c) {
    if (!fastMemo[c]) fastMemo[c] = &router.get(c, 0);
    return *fastMemo[c];
}

const PathTable& Day::cheap(int c) {
    if (!cheapMemo[c]) cheapMemo[c] = &router.get(c, 1);
    return *cheapMemo[c];
}

const vector<int>& Day::pathCells(int from, int to, int mode) {
    auto [it, added] = pathMemo.try_emplace(((long long)from * NC + to) * 2 + mode);
    if (added) it->second = router.path(from, to, mode);
    return it->second;
}

const Station& Day::stationOf(int v) const {
    int x = -1 - v;
    return stations[x / MAX_STATIONS][x % MAX_STATIONS];
}

// 補給車の予定から、待機場所ごとの時間帯を決める。挿入の費用と行動の表は予定ごとに覚えて、元の予定に戻したときに使い回す
void Day::setSchedule(const vector<vector<pair<int, int>>>& sch) {
    if (!stations.empty() && sch == schedule) return;
    if (memoBySchedule.size() >= 4) memoBySchedule.clear();
    memoBySchedule[schedule] = {move(insertMemo), move(actsMemo)};
    if (auto it = memoBySchedule.find(sch); it != memoBySchedule.end()) {
        insertMemo = move(it->second.first);
        actsMemo = move(it->second.second);
        memoBySchedule.erase(it);
    } else {
        insertMemo.assign(P, {});
        actsMemo.assign(P, {});
    }
    schedule = sch;
    endReserveMemo.assign(NC, -1);
    stations.assign(supplyIds.size(), {});
    for (size_t j = 0; j < supplyIds.size(); j++) {
        int cell = st[supplyIds[j]].pos, t = 0;
        for (size_t k = 0; k < schedule[j].size(); k++) {
            auto [c, leave] = schedule[j][k];
            int d = fast(cell).t[c];
            bool last = k + 1 == schedule[j].size();
            if (d == INT_MAX || t + d > steps) { stations[j].push_back({c, INT_MAX, -1}); continue; }
            int from = t + d, to = last ? steps : max(from, min(leave, steps));
            stations[j].push_back({c, from, to});
            t = to;
            cell = c;
        }
    }
}

// 日の終わりに、補給車の日の終わりの位置まで燃料の少ない経路で戻れる燃料
int Day::endReserve(int cell) {
    if (lastDay || supplyIds.empty()) return 0;
    int& r = endReserveMemo[cell];
    if (r >= 0) return r;
    r = INT_MAX;
    for (size_t j = 0; j < supplyIds.size(); j++) {
        int last = st[supplyIds[j]].pos;
        for (const Station& s : stations[j]) if (s.from != INT_MAX) last = s.cell;
        r = min(r, cheap(cell).f[last]);
    }
    return r == INT_MAX ? 0 : r;
}

// route を from 番目から状態 x で回る。at >= 0 なら at 番目に node を差し込んだ並びとして回る
// 区間（補給と補給の間）の中では、残りを燃料の少ない経路で回れる燃料が残る限り速い経路を使う
// 回り終える時刻を返す。着けない・steps を超える・limit 以上になるなら INT_MAX
int Day::runFrom(Stop x, const vector<int>& route, int from, int at, int node, int limit,
            vector<Stop>* stops, vector<Leg>* legs) {
    int len = route.size() + (at >= 0);
    auto item = [&](int i) { return at < 0 || i < at ? route[i] : i == at ? node : route[i - 1]; };
    if ((int)need.size() < len + 2) need.resize(len + 2);
    for (int i = from; i < len;) {
        int e = i;
        while (e < len - 1 && !isRefuel(item(e))) e++;
        need[e + 1] = isRefuel(item(e)) ? 0 : endReserve(cellOf(item(e)));
        for (int l = e; l >= i; l--) {
            int prev = l == i ? x.cell : cellOf(item(l - 1));
            int f = cheap(prev).f[cellOf(item(l))];
            if (f == INT_MAX) return INT_MAX;
            need[l] = need[l + 1] + f;
        }
        for (int l = i; l <= e; l++) {
            int v = item(l), c = cellOf(v);
            const PathTable& f0 = fast(x.cell);
            const PathTable& c0 = cheap(x.cell);
            int mode;
            if (x.fuel - f0.f[c] >= need[l + 1]) mode = 0;
            else if (x.fuel - c0.f[c] >= need[l + 1]) mode = 1;
            else return INT_MAX;
            const PathTable& pt = mode == 0 ? f0 : c0;
            Leg leg{mode, x.t + pt.t[c], 0};
            x = {c, leg.arrive, x.fuel - pt.f[c]};
            if (isRefuel(v)) {
                const Station& s = stationOf(v);
                if (x.t > s.to) return INT_MAX;
                x.t = max(x.t, s.from);
                x.fuel = FUEL_LIMIT;
            }
            leg.done = x.t;
            if (x.t > steps || x.t >= limit) return INT_MAX;
            if (stops) stops->push_back(x);
            if (legs) legs->push_back(leg);
        }
        i = e + 1;
    }
    return x.t;
}

vector<Stop> Day::stopsAlong(int pi, const vector<int>& route) {
    vector<Stop> stops{startStop(pi)};
    runFrom(startStop(pi), route, 0, -1, 0, INT_MAX, &stops);
    return stops;
}

int Day::finishWith(const vector<Stop>& stops, const vector<int>& route, int k, int node, int limit) {
    int segStart = k;
    while (segStart > 0 && !isRefuel(route[segStart - 1])) segStart--;
    return runFrom(stops[segStart], route, segStart, k, node, limit);
}

// 行けなくなった訪問を前から外す
void Day::prune(int pi, vector<int>& route) {
    while (!feasible(pi, route)) {
        size_t ok = stopsAlong(pi, route).size() - 1;
        route.erase(route.begin() + ok);
    }
}

Insertion Day::cheapestSpot(int pi, const vector<int>& route, const vector<int>& cands, const vector<char>& inRoute,
                       const vector<int>& reserved) {
    Insertion best;
    vector<Stop> stops = stopsAlong(pi, route);
    if (stops.size() <= route.size()) return best;
    int base = stops.back().t;
    for (int s : cands) {
        if (inRoute[s] || startSpot[pi] == s || reserved[s] >= spots[s].stock) continue;
        for (int k = 0; k <= (int)route.size(); k++) {
            int f = finishWith(stops, route, k, s, best.cost == INT_MAX ? INT_MAX : base + best.cost);
            if (f <= steps && f - base < best.cost) best = {f - base, k, s};
        }
    }
    return best;
}

vector<int> Day::reservedOf(const vector<vector<int>>& routes) const {
    vector<int> r = startReserved;
    for (auto& route : routes) for (int v : route) if (v >= 0) r[v]++;
    return r;
}

vector<char> Day::inRouteOf(const vector<int>& route) const {
    vector<char> in(S, 0);
    for (int v : route) if (v >= 0) in[v] = 1;
    return in;
}

// 系列 b を入れる。どの巡回車にも入らなければ、補給を1回足してから入れる
bool Day::insertBrand(int b, vector<vector<int>>& routes, bool withRefuel) {
    vector<int> reserved = reservedOf(routes);
    Insertion best;
    int bp = -1;
    for (int pi = 0; pi < P; pi++) {
        Insertion in = cheapestSpot(pi, routes[pi], brandSpots[b], inRouteOf(routes[pi]), reserved);
        if (in.cost < best.cost) { best = in; bp = pi; }
    }
    if (bp >= 0) {
        routes[bp].insert(routes[bp].begin() + best.pos, best.node);
        return true;
    }
    if (!withRefuel) return false;
    int bestCost = INT_MAX;
    vector<int> bestRoute;
    for (int pi = 0; pi < P; pi++) {
        int base = INT_MAX;
        {
            vector<Stop> s0 = stopsAlong(pi, routes[pi]);
            if (s0.size() <= routes[pi].size()) continue;
            base = s0.back().t;
        }
        for (size_t j = 0; j < stations.size(); j++)
            for (size_t k = 0; k < stations[j].size(); k++) {
                if (!hasRefuel(j, k)) continue;
                for (int a = 0; a <= (int)routes[pi].size(); a++) {
                    vector<int> r = routes[pi];
                    r.insert(r.begin() + a, refuelNode(j, k));
                    Insertion in = cheapestSpot(pi, r, brandSpots[b], inRouteOf(r), reserved);
                    if (in.cost == INT_MAX) continue;
                    int f = stopsAlong(pi, r).back().t + in.cost;
                    if (f - base < bestCost) {
                        bestCost = f - base;
                        bestRoute = r;
                        bestRoute.insert(bestRoute.begin() + in.pos, in.node);
                        bp = pi;
                    }
                }
            }
    }
    if (bp < 0) return false;
    routes[bp] = bestRoute;
    return true;
}

// 一番安い巡回車と2番目の差が大きい系列から入れる。入らない系列は補給を足して入れる
vector<vector<int>> Day::assignBrands(int& missing) {
    vector<vector<int>> routes(P);
    vector<int> rest;
    for (int b = 0; b < B; b++) if (!startBrand[b]) rest.push_back(b);
    vector<int> failed;
    while (!rest.empty()) {
        vector<int> reserved = reservedOf(routes);
        double bestRegret = -1;
        int bi = -1, bq = -1;
        Insertion bestIn;
        for (size_t x = 0; x < rest.size(); x++) {
            Insertion first, second;
            int q1 = -1;
            for (int pi = 0; pi < P; pi++) {
                Insertion in = cheapestSpot(pi, routes[pi], brandSpots[rest[x]], inRouteOf(routes[pi]), reserved);
                if (in.cost < first.cost) { second = first; first = in; q1 = pi; }
                else if (in.cost < second.cost) second = in;
            }
            double regret = first.cost == INT_MAX ? 1e18 : second.cost == INT_MAX ? 1e9 : second.cost - first.cost;
            if (regret > bestRegret) { bestRegret = regret; bi = x; bq = q1; bestIn = first; }
        }
        if (bq < 0) failed.push_back(rest[bi]);
        else routes[bq].insert(routes[bq].begin() + bestIn.pos, bestIn.node);
        rest.erase(rest.begin() + bi);
    }
    missing = 0;
    for (int b : failed) if (!insertBrand(b, routes)) missing++;
    return routes;
}

// 所要時間の増分が一番小さい（巡回車, スポット, 位置）を入らなくなるまで挿し込む。在庫は経路に入れた台数で予約する
// 巡回車ごとの最安の挿入は在庫の予約によらないので、同じ経路なら覚えた表を使い回す
vector<vector<int>> Day::fill(vector<vector<int>> routes, double noise) {
    vector<int> reserved = reservedOf(routes);
    vector<vector<char>> inRoute(P);
    vector<vector<Stop>> stops(P);
    vector<const vector<Insertion>*> best(P);
    auto refresh = [&](int pi) {
        inRoute[pi] = inRouteOf(routes[pi]);
        stops[pi] = stopsAlong(pi, routes[pi]);
        auto& memo = insertMemo[pi];
        if (auto it = memo.find(routes[pi]); it != memo.end()) { best[pi] = &it->second; return; }
        // 覚える経路の数を絞るのは、LNS が長く回ると覚えた表でメモリを使い切るため
        if (memo.size() > 4096) memo.clear();
        vector<Insertion>& row = memo[routes[pi]];
        row.assign(S, {});
        best[pi] = &row;
        if (stops[pi].size() <= routes[pi].size()) return;
        int base = stops[pi].back().t;
        for (int s = 0; s < S; s++) {
            Insertion& in = row[s];
            if (inRoute[pi][s] || startSpot[pi] == s) continue;
            for (int k = 0; k <= (int)routes[pi].size(); k++) {
                int f = finishWith(stops[pi], routes[pi], k, s, in.cost == INT_MAX ? INT_MAX : base + in.cost);
                if (f <= steps && f - base < in.cost) in = {f - base, k, s};
            }
        }
    };
    for (int pi = 0; pi < P; pi++) refresh(pi);
    while (true) {
        Insertion choice;
        int bp = -1;
        double bestKey = 1e18;
        for (int pi = 0; pi < P; pi++)
            for (int s = 0; s < S; s++) {
                const Insertion& in = (*best[pi])[s];
                if (in.cost == INT_MAX || reserved[s] >= spots[s].stock) continue;
                double key = noise > 0 ? (in.cost + 1) * (1 + noise * rng.nextDouble()) : in.cost;
                if (key < bestKey) { bestKey = key; choice = in; bp = pi; }
            }
        if (bp < 0) return routes;
        routes[bp].insert(routes[bp].begin() + choice.pos, choice.node);
        reserved[choice.node]++;
        refresh(bp);
    }
}

vector<char> Day::brandsOf(const vector<vector<int>>& routes) const {
    vector<char> has = startBrand;
    for (auto& r : routes) for (int v : r) if (v >= 0) has[spots[v].brand] = 1;
    return has;
}

// need の系列を入れ直し、取れていない系列を tryMissing なら入れてみてから、空いた時間に玉を入れる。need が入らなければ false。
// 補給を足して入れる方法を 5 回に 1 回しか試さないのは、毎回だと重くて回数が減り、まったく試さないと
// 補給車の予定を動かしたときに系列を救えず、どちらも玉が減ったため
// prev と同じ経路の巡回車は行けると分かっているので、行けない訪問を探さない（prev は同じ補給車の予定での経路）
bool Day::repair(vector<vector<int>>& routes, const vector<char>& needBrands, bool tryMissing, double noise,
            const vector<vector<int>>* prev) {
    for (int pi = 0; pi < P; pi++) if (!prev || (*prev)[pi] != routes[pi]) prune(pi, routes[pi]);
    vector<char> has = brandsOf(routes);
    for (int b = 0; b < B; b++)
        if (needBrands[b] && !has[b] && !insertBrand(b, routes, rng.nextInt(5) == 0)) return false;
    if (tryMissing) {
        has = brandsOf(routes);
        for (int b = 0; b < B; b++) if (!has[b]) insertBrand(b, routes, false);
    }
    routes = fill(routes, noise);
    return true;
}

// 実際の行動と評価
vector<vector<int>> Day::toPlan(const vector<vector<int>>& routes) {
    vector<vector<int>> plan(NA);
    for (int pi = 0; pi < P; pi++) {
        vector<int>& acts = plan[patrolIds[pi]];
        auto& memo = actsMemo[pi];
        if (auto it = memo.find(routes[pi]); it != memo.end()) { acts = it->second; continue; }
        if (memo.size() > 4096) memo.clear();
        vector<Leg> legs;
        Stop x = startStop(pi);
        runFrom(x, routes[pi], 0, -1, 0, INT_MAX, nullptr, &legs);
        int cell = x.cell, t = 0;
        for (size_t k = 0; k < legs.size(); k++) {
            int c = cellOf(routes[pi][k]);
            appendMoves(pathCells(cell, c, legs[k].mode), acts);
            if (legs[k].done > legs[k].arrive) acts.push_back(-(legs[k].done - legs[k].arrive));
            cell = c;
            t = legs[k].done;
        }
        if (t < steps) acts.push_back(-(steps - t));
        memo[routes[pi]] = acts;
    }
    for (size_t j = 0; j < supplyIds.size(); j++) {
        vector<int>& acts = plan[supplyIds[j]];
        int cell = st[supplyIds[j]].pos, t = 0;
        for (const Station& s : stations[j]) {
            if (s.from == INT_MAX) continue;
            int used;
            vector<int> mv = movesWithin(pathCells(cell, s.cell, 0), router.status, steps - t, used);
            acts.insert(acts.end(), mv.begin(), mv.end());
            t += used;
            cell = s.cell;
            if (s.to > t) { acts.push_back(-(s.to - t)); t = s.to; }
        }
        if (t < steps) acts.push_back(-(steps - t));
    }
    return plan;
}

Day::Eval Day::evaluate(const vector<vector<int>>& routes) {
    Eval e;
    e.plan = toPlan(routes);
    DayResult r = simulateDay(st, e.plan, router.status, steps);
    if (!r.valid) { cerr << "[solver] 不正な計画: " << r.error << "\n"; return e; }
    e.end = r.end;
    double score = E_BRAND * r.brands + r.balls;
    long long busy = 0, fuelLeft = 0;
    if (!lastDay) {
        const int need = min(FUEL_LIMIT, daySteps[min(dayIndex + 1, D - 1)]);
        for (int i : patrolIds) {
            fuelLeft += min(r.end[i].fuel, need);
            if (spotAt[r.end[i].pos] >= 0) score += E_END_SPOT;
        }
        score += 0.5 * fuelLeft / ((long long)P * FUEL_LIMIT + 1);
    }
    e.lasting = score;
    for (int i : patrolIds) busy += steps - (!e.plan[i].empty() && e.plan[i].back() < 0 ? -e.plan[i].back() : 0);
    score -= 0.5 * busy / ((long long)P * steps + 1);
    e.score = score;
    return e;
}

}  // namespace hexa_udon::solver
