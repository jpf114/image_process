// 单元测试：LS 求解器 / CDF LUT / Wallis 参数场
#include "cb_internal.h"

#include <gtest/gtest.h>

#include "ip/gdal_raii.h"

#include <cmath>
#include <vector>

namespace {
struct GdalEnv {
    GdalEnv() { GDALAllRegister(); }
};
const GdalEnv gdalEnv;
} // namespace
namespace {

ip::DataSetPtr makeMem(int w, int h,
                       const std::function<uint8_t(int, int)>& fillGray) {
    GDALDriver* mem = GetGDALDriverManager()->GetDriverByName("MEM");
    GDALDataset* ds = mem->Create("", w, h, 3, GDT_Byte, nullptr);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t v = fillGray(x, y);
            for (int b = 0; b < 3; ++b) {
                ds->GetRasterBand(b + 1)->RasterIO(GF_Write, x, y, 1, 1, &v, 1,
                                                   1, GDT_Byte, 0, 0, nullptr);
            }
        }
    }
    return ip::DataSetPtr(ds);
}

} // namespace

TEST(LeastSquares, ChainExact) {
    // x1-x0=1, x2-x1=1, x0=0（强锚）→ [0,1,2]
    std::vector<ipcb::LsEquation> eqs;
    eqs.push_back({{{1, 1.0}, {0, -1.0}}, 1.0, 1.0});
    eqs.push_back({{{2, 1.0}, {1, -1.0}}, 1.0, 1.0});
    eqs.push_back({{{0, 1.0}}, 0.0, 1000.0});
    std::vector<double> x;
    ASSERT_TRUE(ipcb::solveLeastSquares(eqs, 3, 0, x));
    EXPECT_NEAR(x[0], 0.0, 1e-9);
    EXPECT_NEAR(x[1], 1.0, 1e-9);
    EXPECT_NEAR(x[2], 2.0, 1e-9);
}

TEST(LeastSquares, OverdeterminedContradiction) {
    // 矛盾观测 1 与 2 → LS 解 1.5（等权均值）
    std::vector<ipcb::LsEquation> eqs;
    eqs.push_back({{{0, -1.0}, {1, 1.0}}, 1.0, 1.0});
    eqs.push_back({{{0, -1.0}, {1, 1.0}}, 2.0, 1.0});
    eqs.push_back({{{0, 1.0}}, 0.0, 1000.0});
    std::vector<double> x;
    ASSERT_TRUE(ipcb::solveLeastSquares(eqs, 2, 0, x));
    EXPECT_NEAR(x[1], 1.5, 1e-9);
}

TEST(LeastSquares, UnconnectedVariableZero) {
    // 变量 2 与参考分量无方程 → 保持 0（调用方语义=恒等）
    std::vector<ipcb::LsEquation> eqs;
    eqs.push_back({{{1, 1.0}, {0, -1.0}}, 1.0, 1.0});
    eqs.push_back({{{0, 1.0}}, 0.0, 1000.0});
    std::vector<double> x;
    ASSERT_TRUE(ipcb::solveLeastSquares(eqs, 3, 0, x));
    EXPECT_NEAR(x[2], 0.0, 1e-12);
}

TEST(CdfMatchLut, MonotonicAndAnchor) {
    // src 分布集中在 0..49；ref 集中在 200..249 → LUT 将低值映射到高值且单调
    uint64_t src[256] = {0}, ref[256] = {0};
    for (int i = 0; i < 50; ++i) src[i] = 100;
    for (int i = 200; i < 250; ++i) ref[i] = 100;
    uint8_t lut[256];
    ipcb::cdfMatchLut(src, ref, lut);
    for (int i = 1; i < 256; ++i) EXPECT_GE(lut[i], lut[i - 1]);
    // src 的 50 分位 → ref 的 50 分位（≈225）
    EXPECT_GE(lut[25], 220);
    EXPECT_LE(lut[25], 230);
}

TEST(CdfMatchLut, IdentityForSameHist) {
    uint64_t h[256] = {0};
    for (int i = 0; i < 256; ++i) h[i] = i + 1;
    uint8_t lut[256];
    ipcb::cdfMatchLut(h, h, lut);
    for (int i = 0; i < 256; ++i) EXPECT_NEAR(lut[i], i, 1);  // 允许平段 ±1
}

TEST(WallisGrid, TwoValueImageTarget) {
    // 0/200 各半（σ=100, μ=100）；target μ=200 σ=50 → a=0.5, b=150
    auto ds = makeMem(128, 128, [](int x, int) {
        return x < 64 ? 0 : 200;
    });
    const double tMean[3] = {200, 200, 200};
    const double tStd[3] = {50, 50, 50};
    const double g[3] = {1, 1, 1}, o[3] = {0, 0, 0};
    const ipcb::WallisGrid wg =
        ipcb::computeWallisGrid(ds.get(), tMean, tStd, 512, g, o);
    ASSERT_EQ(wg.nbx, 1);
    ASSERT_EQ(wg.nby, 1);
    for (int b = 0; b < 3; ++b) {
        EXPECT_NEAR(wg.a[b], 0.5, 0.01);
        EXPECT_NEAR(wg.b[b], 150.0, 0.5);
    }
}

TEST(WallisGrid, ClipRangeSemantics) {
    // 限幅 [0.4,2.5]：σ_t=50, σ'=200 → a=0.25 → 限幅 0.4（Phase 0 经验语义）
    auto ds = makeMem(128, 128, [](int x, int) {
        return x < 64 ? 0 : 200;
    });
    const double tMean[3] = {210, 210, 210};
    const double tStd[3] = {50, 50, 50};
    const double g[3] = {2, 2, 2}, o[3] = {10, 10, 10};
    const ipcb::WallisGrid wg =
        ipcb::computeWallisGrid(ds.get(), tMean, tStd, 512, g, o);
    EXPECT_NEAR(wg.a[0], 0.4, 0.01);
    EXPECT_NEAR(wg.b[0], 210.0 - 0.4 * 210.0, 0.5);
}

TEST(WallisGrid, SceneGainFolded) {
    // 组合模式：先行景级变换 μ'=g·μ+o；0/200 各半 + g=2,o=10 → μ'=210,σ'=200
    // target μ=210 σ=100 → a=0.5（限幅区间 [0.4,2.5] 内）；块统计用 raw，公式内含 g/o
    auto ds = makeMem(128, 128, [](int x, int) {
        return x < 64 ? 0 : 200;
    });
    const double tMean[3] = {210, 210, 210};
    const double tStd[3] = {100, 100, 100};
    const double g[3] = {2, 2, 2}, o[3] = {10, 10, 10};
    const ipcb::WallisGrid wg =
        ipcb::computeWallisGrid(ds.get(), tMean, tStd, 512, g, o);
    for (int b = 0; b < 3; ++b) {
        // μ_raw=100 → μ'=210；σ_raw=100 → σ'=200 → a=0.5；b=210-0.5*210=105
        EXPECT_NEAR(wg.a[b], 0.5, 0.01);
        EXPECT_NEAR(wg.b[b], 105.0, 0.5);
    }
}
