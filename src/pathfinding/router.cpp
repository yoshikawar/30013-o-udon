// 最短経路（最速 / 燃料最小）（procon2026 の solvers/meet.cpp / common.hpp から移した。処理は元のまま）
#include "hexa_udon/solver.hpp"

namespace hexa_udon::solver {

const PathTable& Router::get(int src, int mode) {
    auto it = tbl[mode].find(src);
    if (it != tbl[mode].end()) return it->second;
    PathTable pt;
    pt.t.assign(NC, INT_MAX); pt.f.assign(NC, INT_MAX); pt.par.assign(NC, -1);
    // 辞書順 (主キー, 副キー)
    using T = tuple<long long, int>;
    priority_queue<T, vector<T>, greater<T>> pq;
    auto key = [&](int t, int f) { return mode == 0 ? (long long)t * 100000 + f : (long long)f * 100000 + t; };
    pt.t[src] = 0; pt.f[src] = 0;
    pq.push({0, src});
    while (!pq.empty()) {
        auto [k, u] = pq.top(); pq.pop();
        if (k != key(pt.t[u], pt.f[u])) continue;
        int sc = stepCost(u, status), fc = fuelCost(u);
        for (int d = 0; d < 6; d++) {
            int v = neighbor(u, d);
            if (!passable(v)) continue;
            int nt = pt.t[u] + sc, nf = pt.f[u] + fc;
            if (pt.t[v] == INT_MAX || key(nt, nf) < key(pt.t[v], pt.f[v])) {
                pt.t[v] = nt; pt.f[v] = nf; pt.par[v] = u;
                pq.push({key(nt, nf), v});
            }
        }
    }
    return tbl[mode][src] = move(pt);
}

vector<int> Router::path(int src, int dst, int mode) {
    const PathTable& pt = get(src, mode);
    vector<int> cells;
    for (int c = dst; c != -1; c = pt.par[c]) cells.push_back(c);
    reverse(cells.begin(), cells.end());
    return cells;
}

int dirTo(int a, int b) {
    for (int d = 0; d < 6; d++) if (neighbor(a, d) == b) return d;
    return -1;
}
void appendMoves(const vector<int>& cells, vector<int>& acts) {
    for (size_t i = 0; i + 1 < cells.size(); i++) acts.push_back(dirTo(cells[i], cells[i + 1]));
}
int pathSteps(const vector<int>& cells, const vector<int>& status) {
    int t = 0;
    for (size_t i = 0; i + 1 < cells.size(); i++) t += stepCost(cells[i], status);
    return t;
}
// cells に沿って steps 以内で行けるところまでの行動
vector<int> movesWithin(const vector<int>& cells, const vector<int>& status, int steps, int& used) {
    vector<int> acts;
    used = 0;
    for (size_t k = 0; k + 1 < cells.size(); k++) {
        int c = stepCost(cells[k], status);
        if (used + c > steps) break;
        used += c;
        acts.push_back(dirTo(cells[k], cells[k + 1]));
    }
    return acts;
}

}  // namespace hexa_udon::solver
