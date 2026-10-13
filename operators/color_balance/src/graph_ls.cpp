// 最小二乘图平差求解器：法方程 + 部分主元高斯消元（景数规模 N≤数百，稠密即可）
#include "cb_internal.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <vector>

namespace ipcb {

bool solveLeastSquares(const std::vector<LsEquation>& eqs, int nVars,
                       int refVar, std::vector<double>& out) {
    out.assign(nVars, 0.0);
    if (nVars <= 0) return true;

    // 1. 找与 refVar 连通的变量集合（在方程图上 BFS；孤立变量无约束，不进方程）
    std::vector<std::vector<int>> incident(nVars);
    for (size_t i = 0; i < eqs.size(); ++i) {
        for (const auto& [v, c] : eqs[i].coeffs) {
            (void)c;
            if (v >= 0 && v < nVars) incident[v].push_back(static_cast<int>(i));
        }
    }
    std::vector<char> inSet(nVars, 0);
    std::queue<int> q;
    if (refVar >= 0 && refVar < nVars) {
        inSet[refVar] = 1;
        q.push(refVar);
    }
    while (!q.empty()) {
        const int v = q.front();
        q.pop();
        for (int ei : incident[v]) {
            for (const auto& [u, c] : eqs[ei].coeffs) {
                (void)c;
                if (!inSet[u]) {
                    inSet[u] = 1;
                    q.push(u);
                }
            }
        }
    }
    // 变量重编号（仅连通集）
    std::vector<int> remap(nVars, -1);
    int n = 0;
    for (int v = 0; v < nVars; ++v) {
        if (inSet[v]) remap[v] = n++;
    }
    if (n == 0) return true;

    // 2. 法方程 A^T A x = A^T b（加权）
    std::vector<double> ata(static_cast<size_t>(n) * n, 0.0);
    std::vector<double> atb(n, 0.0);
    for (const auto& e : eqs) {
        // 只取连通集内系数
        std::vector<std::pair<int, double>> terms;
        for (const auto& [v, c] : e.coeffs) {
            if (remap[v] >= 0) terms.emplace_back(remap[v], c);
        }
        if (terms.empty()) continue;
        const double w = std::sqrt(std::max(e.weight, 0.0));
        for (size_t i = 0; i < terms.size(); ++i) {
            atb[terms[i].first] += w * terms[i].second * e.rhs;
            for (size_t j = 0; j < terms.size(); ++j) {
                ata[static_cast<size_t>(terms[i].first) * n + terms[j].first] +=
                    w * terms[i].second * terms[j].second;
            }
        }
    }

    // 3. 高斯消元（部分主元）；奇异则正则化微退（对角加 eps）
    const double eps = 1e-9;
    for (int col = 0; col < n; ++col) {
        int piv = col;
        double best = std::abs(ata[static_cast<size_t>(col) * n + col]);
        for (int r = col + 1; r < n; ++r) {
            const double v = std::abs(ata[static_cast<size_t>(r) * n + col]);
            if (v > best) {
                best = v;
                piv = r;
            }
        }
        if (best < 1e-12) {
            ata[static_cast<size_t>(col) * n + col] += 1e-6;  // 正则化
            continue;
        }
        if (piv != col) {
            for (int c = 0; c < n; ++c) {
                std::swap(ata[static_cast<size_t>(col) * n + c],
                          ata[static_cast<size_t>(piv) * n + c]);
            }
            std::swap(atb[col], atb[piv]);
        }
        const double d = ata[static_cast<size_t>(col) * n + col];
        for (int r = col + 1; r < n; ++r) {
            const double f = ata[static_cast<size_t>(r) * n + col] / d;
            if (f == 0.0) continue;
            for (int c = col; c < n; ++c) {
                ata[static_cast<size_t>(r) * n + c] -=
                    f * ata[static_cast<size_t>(col) * n + c];
            }
            atb[r] -= f * atb[col];
        }
    }
    for (int r = n - 1; r >= 0; --r) {
        double s = atb[r];
        for (int c = r + 1; c < n; ++c) s -= ata[static_cast<size_t>(r) * n + c] * out[c];
        const double d = ata[static_cast<size_t>(r) * n + r];
        out[r] = std::abs(d) > 1e-12 ? s / d : 0.0;
    }

    // 4. 回映射
    std::vector<double> full = out;
    out.assign(nVars, 0.0);
    for (int v = 0; v < nVars; ++v) {
        if (remap[v] >= 0) out[v] = full[remap[v]];
    }
    (void)eps;
    return true;
}

} // namespace ipcb
