// solver: 種別決めと 1 日の計画（補給の時期と場所も計画に入れる LNS ＋ 焼きなまし）。procon2026 の solvers/meet.cpp, solvers/common.hpp を移したもの。
// 処理は元のまま。定義は次のファイルに分けて置く:
//   src/core/problem.cpp                    問題（大域変数）と o-udon の型との変換
//   src/simulator/simulator.cpp             1 日のシミュレーション（ルール完全再現）
//   src/pathfinding/router.cpp              最短経路（最速 / 燃料最小）
//   src/planner/day_planner.cpp             Day: 1 日の解（訪問の並びと補給）の組み立てと評価
//   src/optimizer/optimizer.cpp             Planner: LNS + 焼きなましと planDay
//   src/planner/prematch_type_selector.cpp  種別決め
#pragma once
#include "hexa_udon/core/models.hpp"
#include "hexa_udon/simulator/action.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <numeric>
#include <queue>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hexa_udon::solver {
using namespace std;

const int INF_FUEL = 1 << 29;

// ------------------------------ ユーティリティ ------------------------------
struct Timer {
    chrono::steady_clock::time_point st = chrono::steady_clock::now();
    double ms() const { return chrono::duration<double, milli>(chrono::steady_clock::now() - st).count(); }
};
struct Rng {
    uint64_t x = 88172645463325252ULL;
    uint64_t next() { x ^= x << 7; x ^= x >> 9; return x; }
    int nextInt(int n) { return (int)(next() % (uint64_t)n); }
    double nextDouble() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
};
extern Rng rng;

// ------------------------------ 問題（src/core/problem.cpp） ------------------------------
enum Terrain { PLAIN = 0, ROAD = 1, MOUNTAIN = 2, POND = 3 };

extern int H, W, NC;                 // 縦, 横, セル数
extern vector<int> cellType;         // [NC]
struct Spot { int brand, pos, stock; };
extern vector<Spot> spots;           // brand は 0..B-1 に振り直し済み
extern vector<int> rawBrand;         // 振り直した系列番号 -> 元の系列番号
extern int S, B;                     // スポット数, 系列数
extern vector<int> spotAt;           // セル -> スポット番号 (なければ -1)
extern int NA;                       // 1 チームのエージェント数
extern vector<int> agentStart;
extern int FUEL_LIMIT, D, PLAYERS, BUSY, JAM;
extern vector<int> daySteps, daySeconds;
extern int today;                    // planDay を呼ぶ日（0 始まり）
extern bool choosingKinds;           // 種別決めのシミュレーションの中で planDay を呼んでいる
extern double interimSec;            // 途中の計画を出す間隔（秒）。0 なら出さない

// 方向: 0 左上, 1 右上, 2 右, 3 右下, 4 左下, 5 左（偶数行が右にずれる）
const int DX_EVEN[6] = {0, 1, 1, 1, 0, -1}, DX_ODD[6] = {-1, 0, 1, 0, -1, -1};
const int DY[6] = {-1, -1, 0, 1, 1, 0};
inline int neighbor(int p, int d) {
    int x = p % W, y = p / W;
    int nx = x + ((y % 2 == 0) ? DX_EVEN[d] : DX_ODD[d]), ny = y + DY[d];
    if (nx < 0 || nx >= W || ny < 0 || ny >= H) return -1;
    return ny * W + nx;
}
inline bool passable(int p) { return p >= 0 && cellType[p] != POND; }
// そのセル「から」出るのにかかるステップ数 / 燃料
inline int stepCost(int p, const vector<int>& status) {
    switch (cellType[p]) {
        case PLAIN: return 2;
        case MOUNTAIN: return 3;
        case ROAD: return status[p] == 0 ? 1 : status[p] == 1 ? 2 : 4;
    }
    return 1 << 20;
}
inline int fuelCost(int p) { return cellType[p] == PLAIN ? 1 : 2; }

struct Agent { int kind, pos, fuel; };  // kind 0 巡回車 / 1 補給車

// o-udon の型との変換。loadMap はシミュレーションに要る分（盤面・スポット・燃料の上限）だけを入れる
void loadMap(const core::MapDefinition& map, span<const core::Spot> mapSpots, int fuelLimit);
void loadProblem(const core::MatchConfig& match);
vector<Agent> agentsOf(const vector<core::AgentState>& agents);
vector<int> statusOf(const vector<core::TrafficState>& traffic);
simulator::DayActionPlan toActionPlan(const vector<vector<int>>& plan);
vector<vector<int>> fromActionPlan(const simulator::DayActionPlan& plan);

// 途中の計画を受け取る先（client が提出する）
extern function<void(const vector<vector<int>>&)> interimSink;
void emitInterim(const vector<vector<int>>& plan);

// ------------------------------ シミュレータ（src/simulator/simulator.cpp） ------------------------------
struct DayResult {
    bool valid = true;
    string error;
    vector<Agent> end;
    vector<long long> stay;   // 自チームの道路セル滞在ステップ数
    vector<char> brandHit;    // [B]
    int brands = 0, balls = 0;
};

// 道路状態: sumStay = 全チーム分の前日+前々日の滞在ステップ数
vector<int> statusFromStay(const vector<long long>& sumStay, int players);
bool checkStructure(const vector<Agent>& st, const vector<vector<int>>& plan, const vector<int>& status, int steps, string& err);
DayResult simulateDay(const vector<Agent>& st, const vector<vector<int>>& plan, const vector<int>& status, int steps);
vector<vector<int>> allWait(int steps);

// ------------------------------ 経路（src/pathfinding/router.cpp） ------------------------------
struct PathTable { vector<int> t, f, par; };
struct Router {
    vector<int> status;
    unordered_map<int, PathTable> tbl[2];  // [0] 最速, [1] 燃料最小（要素の参照は再ハッシュでも無効にならない）
    explicit Router(const vector<int>& st) : status(st) {}
    const PathTable& get(int src, int mode);
    vector<int> path(int src, int dst, int mode);  // src..dst のセル列
};
int dirTo(int a, int b);
void appendMoves(const vector<int>& cells, vector<int>& acts);
int pathSteps(const vector<int>& cells, const vector<int>& status);
// cells に沿って steps 以内で行けるところまでの行動
vector<int> movesWithin(const vector<int>& cells, const vector<int>& status, int steps, int& used);

// ------------------------------ 1 日の解（src/planner/day_planner.cpp） ------------------------------
const double E_BRAND = 1000.0;
const double E_END_SPOT = 0.8;
const double E_TOMORROW = 500.0;
const double FUEL_TIGHT = 1.5;
const double LNS_T0 = 2.0, LNS_T1 = 0.02;
const double LNS_NOISE = 0.3;
const int MAX_STATIONS = 8;

struct Insertion { int cost = INT_MAX, pos = 0, node = -1; };
struct Stop { int cell, t, fuel; };
struct Leg { int mode, arrive, done; };       // 区間の経路（0 速い / 1 燃料の少ない）、着いた時刻、補給を終えた時刻
struct Station { int cell, from, to; };       // 補給車がこのマスにいる時間帯

// 経路の訪問: 0 以上はスポット、負は補給（補給車 j の k 番目の待機場所）
inline int refuelNode(int j, int k) { return -1 - (j * MAX_STATIONS + k); }
inline bool isRefuel(int v) { return v < 0; }

struct Day {
    const vector<Agent>& st;
    int steps, dayIndex;
    bool lastDay;
    Router& router;
    const vector<int>& patrolIds;
    const vector<int>& supplyIds;
    const vector<vector<int>>& brandSpots;
    int P;
    vector<int> startFuel, startSpot;         // 巡回車ごとの今日使える燃料と、朝に立っているスポット（なければ -1）
    vector<int> startReserved;                // スポットごとの朝に立っている巡回車が取る玉の数
    vector<char> startBrand;
    vector<vector<pair<int, int>>> schedule;  // 補給車ごとの（待機するマス, 離れる時刻）
    vector<vector<Station>> stations;
    vector<const PathTable*> fastMemo, cheapMemo;
    unordered_map<long long, vector<int>> pathMemo;
    vector<map<vector<int>, vector<Insertion>>> insertMemo;  // 巡回車ごと: 経路 → スポットごとの最安の挿入
    vector<map<vector<int>, vector<int>>> actsMemo;            // 巡回車ごと: 経路 → 実際の行動
    map<vector<vector<pair<int, int>>>, pair<vector<map<vector<int>, vector<Insertion>>>, vector<map<vector<int>, vector<int>>>>> memoBySchedule;
    vector<int> need;                         // runFrom の作業領域
    vector<int> endReserveMemo;

    Day(const vector<Agent>& st_, int steps_, int dayIndex_, Router& r, const vector<int>& patrolIds_,
        const vector<int>& supplyIds_, const vector<vector<int>>& brandSpots_);

    int fuelToKeep(int fuel) const;

    const PathTable& fast(int c);
    const PathTable& cheap(int c);
    const PathTable& table(int c, int mode) { return mode == 0 ? fast(c) : cheap(c); }
    const vector<int>& pathCells(int from, int to, int mode);

    const Station& stationOf(int v) const;
    int cellOf(int v) const { return v >= 0 ? spots[v].pos : stationOf(v).cell; }

    void setSchedule(const vector<vector<pair<int, int>>>& sch);

    int endReserve(int cell);

    int runFrom(Stop x, const vector<int>& route, int from, int at, int node, int limit,
                vector<Stop>* stops = nullptr, vector<Leg>* legs = nullptr);

    Stop startStop(int pi) const { return {st[patrolIds[pi]].pos, 0, startFuel[pi]}; }

    vector<Stop> stopsAlong(int pi, const vector<int>& route);

    bool feasible(int pi, const vector<int>& route) { return stopsAlong(pi, route).size() == route.size() + 1; }

    int finishWith(const vector<Stop>& stops, const vector<int>& route, int k, int node, int limit);

    void prune(int pi, vector<int>& route);

    bool hasRefuel(int j, int k) const { return j < (int)stations.size() && k < (int)stations[j].size() && stations[j][k].from != INT_MAX; }

    Insertion cheapestSpot(int pi, const vector<int>& route, const vector<int>& cands, const vector<char>& inRoute,
                           const vector<int>& reserved);

    vector<int> reservedOf(const vector<vector<int>>& routes) const;
    vector<char> inRouteOf(const vector<int>& route) const;

    bool insertBrand(int b, vector<vector<int>>& routes, bool withRefuel = true);

    vector<vector<int>> assignBrands(int& missing);

    vector<vector<int>> fill(vector<vector<int>> routes, double noise);

    vector<char> brandsOf(const vector<vector<int>>& routes) const;

    bool repair(vector<vector<int>>& routes, const vector<char>& needBrands, bool tryMissing, double noise,
                const vector<vector<int>>* prev = nullptr);

    vector<vector<int>> toPlan(const vector<vector<int>>& routes);

    struct Eval { double score = -1e18, lasting = -1e18; vector<vector<int>> plan; vector<Agent> end; };  // lasting: 動き終えた時刻の項を除いた評価

    Eval evaluate(const vector<vector<int>>& routes);
};

// ------------------------------ 1 日の計画（src/optimizer/optimizer.cpp） ------------------------------
vector<vector<int>> planDay(const vector<Agent>& st, const vector<int>& status, const vector<char>& before,
                            int steps, bool lastDay, double timeMs);

// ------------------------------ 種別決め（src/planner/prematch_type_selector.cpp） ------------------------------
vector<vector<int>> kindCandidates();
struct MatchScore {
    int matchBrands = 0, dayBrands = 0, balls = 0;
    bool operator<(const MatchScore& o) const {
        return tie(matchBrands, dayBrands, balls) < tie(o.matchBrands, o.dayBrands, o.balls);
    }
};
// 全チームが同じ動きをすると仮定（交通量 = 自チームの滞在数）して全日程を回す
MatchScore simulateMatch(const vector<int>& kinds, double timeMs);
vector<int> solveKind(double timeMs);

}  // namespace hexa_udon::solver
