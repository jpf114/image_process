// 单元测试：分块统计（exactStatsRGB / windowStatsRGB）
#include "ip/block_stats.h"
#include "ip/gdal_raii.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {
struct GdalEnv {
    GdalEnv() { GDALAllRegister(); }
};
const GdalEnv gdalEnv;
} // namespace
namespace {

// 合成 MEM 影像（RGB + alpha），fill 回调逐像素填值
ip::DataSetPtr makeMem(int w, int h, bool alpha,
                       const std::function<void(int x, int y, uint8_t rgb[3],
                                                uint8_t& a)>& fill) {
    GDALDriver* mem = GetGDALDriverManager()->GetDriverByName("MEM");
    const int nb = alpha ? 4 : 3;
    GDALDataset* ds = mem->Create("", w, h, nb, GDT_Byte, nullptr);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t rgb[3] = {0, 0, 0};
            uint8_t a = 255;
            fill(x, y, rgb, a);
            for (int b = 0; b < 3; ++b) {
                ds->GetRasterBand(b + 1)->RasterIO(GF_Write, x, y, 1, 1,
                                                   &rgb[b], 1, 1, GDT_Byte, 0,
                                                   0, nullptr);
            }
            if (alpha) {
                ds->GetRasterBand(4)->RasterIO(GF_Write, x, y, 1, 1, &a, 1, 1,
                                               GDT_Byte, 0, 0, nullptr);
            }
        }
    }
    return ip::DataSetPtr(ds);
}

} // namespace

TEST(BlockStats, ConstantImageExact) {
    auto ds = makeMem(64, 64, false, [](int, int, uint8_t rgb[3], uint8_t&) {
        rgb[0] = 100; rgb[1] = 150; rgb[2] = 200;
    });
    const ip::BandStats3 st = ip::exactStatsRGB(ds.get());
    for (int b = 0; b < 3; ++b) {
        EXPECT_NEAR(st.mean[b], b == 0 ? 100 : b == 1 ? 150 : 200, 1e-6);
        EXPECT_NEAR(st.std[b], 0.0, 1e-6);
        EXPECT_EQ(st.validCount[b], 64u * 64);
    }
}

TEST(BlockStats, NodataExcluded) {
    // 左半有效(50) 右半 alpha=0（无效），均值应只统计左半
    auto ds = makeMem(64, 32, true, [](int x, int, uint8_t rgb[3], uint8_t& a) {
        rgb[0] = 50; rgb[1] = 50; rgb[2] = 50;
        a = x < 32 ? 255 : 0;
    });
    const ip::BandStats3 st = ip::exactStatsRGB(ds.get());
    EXPECT_NEAR(st.mean[0], 50.0, 1e-6);
    EXPECT_EQ(st.validCount[0], 32u * 32);
}

TEST(BlockStats, WindowSmallExact) {
    auto ds = makeMem(64, 64, false, [](int, int, uint8_t rgb[3], uint8_t&) {
        rgb[0] = 10; rgb[1] = 20; rgb[2] = 30;
    });
    const ip::BandStats3 st = ip::windowStatsRGB(ds.get(), 8, 8, 16, 16, 2048);
    EXPECT_NEAR(st.mean[0], 10.0, 1e-6);
    EXPECT_NEAR(st.mean[2], 30.0, 1e-6);
}

TEST(BlockStats, TileSamplingMatchesFullRead) {
    // 渐变影像：tile 采样（maxSide=128）与全读统计的 μ/σ 应接近（采样均匀无偏）
    auto ds = makeMem(512, 512, false, [](int x, int y, uint8_t rgb[3], uint8_t&) {
        rgb[0] = static_cast<uint8_t>((x + y) % 256);
        rgb[1] = static_cast<uint8_t>(x % 256);
        rgb[2] = static_cast<uint8_t>(y % 256);
    });
    const ip::BandStats3 full = ip::windowStatsRGB(ds.get(), 0, 0, 512, 512, 4096);
    const ip::BandStats3 samp = ip::windowStatsRGB(ds.get(), 0, 0, 512, 512, 128);
    for (int b = 0; b < 3; ++b) {
        EXPECT_NEAR(samp.mean[b], full.mean[b], 2.0);
        EXPECT_NEAR(samp.std[b], full.std[b], 3.0);
    }
}

TEST(BlockStats, BilinearConsistencyOfGdalPath) {
    // 窗口统计内部（GDAL 读 + 掩码）与逐像素直读一致性抽查
    auto ds = makeMem(32, 32, true, [](int x, int, uint8_t rgb[3], uint8_t& a) {
        rgb[0] = static_cast<uint8_t>(x * 7 % 256);
        a = 255;
    });
    const ip::BandStats3 st = ip::windowStatsRGB(ds.get(), 0, 0, 32, 32, 2048);
    EXPECT_NEAR(st.mean[0], (0 + 31 * 7 % 256) / 2.0, 4.0);
}
