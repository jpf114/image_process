// 单元测试：瓦片渲染（nodata 三模式/阈值/skip-empty）——合成 3857 源落盘 GTiff
#include "tile_ctx.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <vector>

namespace {
struct GdalEnv {
    GdalEnv() { GDALAllRegister(); }
};
const GdalEnv gdalEnv;
} // namespace

namespace {

constexpr int kW = 512, kH = 512;
constexpr const char* kDir = "/tmp/ip_test_render";

// 512×512 RGBA 源（3857，覆盖世界中央 [-10018754, +10018754]²，即 z2 瓦片
// (1..2, 1..2)）。alpha：leftHalfValid 时世界 x<0（源左半）有效；allInvalid
// 时全无效。落盘为真实 GeoTIFF：VRT 的 SourceDataset 必须是可重开路径
//（MEM 空描述会在 VRT CreateCopy 的 XML 回读校验时打开失败）——与生产一致
ip::DataSetPtr makeSrc(bool leftHalfValid, bool allInvalid = false) {
    std::filesystem::create_directories(kDir);
    GDALDriver* mem = GetGDALDriverManager()->GetDriverByName("MEM");
    GDALDataset* ds = mem->Create("", kW, kH, 4, GDT_Byte, nullptr);
    double gt[6] = {-10018754.17139462, 20037508.342789248 / kW, 0,
                    10018754.17139462, 0, -20037508.342789248 / kH};
    ds->SetGeoTransform(gt);
    OGRSpatialReference srs = ip::makeSrsFromEpsg(3857);
    char* wkt = nullptr;
    srs.exportToWkt(&wkt);
    if (wkt) {
        ds->SetProjection(wkt);
        CPLFree(wkt);
    }
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            uint8_t v = 100;
            for (int b = 0; b < 3; ++b) {
                ds->GetRasterBand(b + 1)->RasterIO(GF_Write, x, y, 1, 1, &v, 1,
                                                   1, GDT_Byte, 0, 0, nullptr);
            }
            uint8_t a =
                allInvalid ? 0 : ((!leftHalfValid || x < kW / 2) ? 255 : 0);
            ds->GetRasterBand(4)->RasterIO(GF_Write, x, y, 1, 1, &a, 1, 1,
                                           GDT_Byte, 0, 0, nullptr);
        }
    }
    GDALDriver* gtiff = GetGDALDriverManager()->GetDriverByName("GTiff");
    GDALDataset* f =
        gtiff->CreateCopy((std::string(kDir) + "/src.tif").c_str(), ds, FALSE,
                          nullptr, nullptr, nullptr);
    GDALClose(ds);
    if (!f) {
        ADD_FAILURE() << "测试源落盘 GTiff 失败";
        return ip::DataSetPtr();
    }
    GDALClose(f);
    return ip::openReadOnly(std::string(kDir) + "/src.tif");
}

// renderTile 落盘路径：{outputDir}/{z}/{x}/{y}.png
std::string tilePath(int z, long long tx, long long ty) {
    return std::string(kDir) + "/" + std::to_string(z) + "/" +
           std::to_string(tx) + "/" + std::to_string(ty) + ".png";
}

// RenderCtx 含 atomic 计数器不可拷贝，就地填充
void makeCtx(iptl::RenderCtx& ctx, const iptl::TileMatrixSet& tms, int z,
             const ip::GeoExtent& ext, bool skipEmpty, bool fillMode) {
    ctx.nBands = 4;
    ctx.tileSize = 256;
    ctx.threshold = 100;
    ctx.skipEmpty = skipEmpty;
    ctx.fillMode = fillMode;
    ctx.fillValue = 128;
    ctx.format = "png";
    ctx.alg = GRA_Bilinear;
    ctx.outputDir = kDir;
    ctx.dataExtent = ext;
}

} // namespace

TEST(RenderTile, FullValidTile) {
    auto src = makeSrc(true);
    const iptl::TileMatrixSet tms = iptl::makeWebMercatorQuad(24);
    double res = 0;
    ip::GeoExtent ext;
    std::string err;
    ASSERT_TRUE(iptl::buildWarpedVrt(src.get(), tms, "bilinear",
                                     "/tmp/ip_test_render/warp.vrt", res, ext,
                                     err))
        << err;
    GDALDataset* warpDs = iptl::threadLocalWarpDs("/tmp/ip_test_render/warp.vrt");
    ASSERT_NE(warpDs, nullptr);

    iptl::RenderCtx ctx;
    makeCtx(ctx,tms, 2, ext, /*skipEmpty=*/true, false);
    // z2 瓦片 (1,1)：x∈[-10M,0], y∈[0,10M] —— 完全落在 alpha 有效区
    const iptl::TileBbox bbox = iptl::tileBbox(tms, 2, 1, 1, 256, 256);
    std::error_code ec;
    std::filesystem::remove(tilePath(2, 1, 1), ec);
    const int rc =
        iptl::renderTile(warpDs, bbox, ctx, tms.levels[2].res, 2, 1, 1, err);
    ASSERT_EQ(rc, 0) << err;
    EXPECT_TRUE(std::filesystem::exists(tilePath(2, 1, 1)));
}

TEST(RenderTile, TransparentNodataHalf) {
    auto src = makeSrc(true);
    const iptl::TileMatrixSet tms = iptl::makeWebMercatorQuad(24);
    double res = 0;
    ip::GeoExtent ext;
    std::string err;
    ASSERT_TRUE(iptl::buildWarpedVrt(src.get(), tms, "bilinear",
                                     "/tmp/ip_test_render/warp2.vrt", res, ext,
                                     err))
        << err;
    GDALDataset* warpDs =
        iptl::threadLocalWarpDs("/tmp/ip_test_render/warp2.vrt");
    ASSERT_NE(warpDs, nullptr);

    // z1 瓦片 (0,0)：x∈[-20M,0], y∈[0,20M] —— 与数据（世界 x<0）相交 1/4，
    // 部分透明
    iptl::RenderCtx ctx;
    makeCtx(ctx,tms, 1, ext, /*skipEmpty=*/true, false);
    const iptl::TileBbox bbox = iptl::tileBbox(tms, 1, 0, 0, 256, 256);
    std::error_code ec;
    std::filesystem::remove(tilePath(1, 0, 0), ec);
    const int rc =
        iptl::renderTile(warpDs, bbox, ctx, tms.levels[1].res, 1, 0, 0, err);
    ASSERT_EQ(rc, 0) << err;
    auto ds = ip::openReadOnly(tilePath(1, 0, 0));
    ASSERT_NE(ds, nullptr);
    std::vector<uint8_t> buf(256 * 256);
    ds->GetRasterBand(4)->RasterIO(GF_Read, 0, 0, 256, 256, buf.data(), 256,
                                   256, GDT_Byte, 0, 0, nullptr);
    int valid = 0;
    for (int i = 0; i < 256 * 256; ++i) valid += buf[i] > 0;
    EXPECT_GT(valid, 0);
    EXPECT_LT(valid, 256 * 256);  // 部分透明
}

TEST(RenderTile, SkipEmptyTile) {
    // 全 alpha=0 源 → rc=1 且不落盘
    auto src = makeSrc(true, /*allInvalid=*/true);
    const iptl::TileMatrixSet tms = iptl::makeWebMercatorQuad(24);
    double res = 0;
    ip::GeoExtent ext;
    std::string err;
    ASSERT_TRUE(iptl::buildWarpedVrt(src.get(), tms, "bilinear",
                                     "/tmp/ip_test_render/warp3.vrt", res, ext,
                                     err))
        << err;
    GDALDataset* warpDs =
        iptl::threadLocalWarpDs("/tmp/ip_test_render/warp3.vrt");
    ASSERT_NE(warpDs, nullptr);

    iptl::RenderCtx ctx;
    makeCtx(ctx,tms, 2, ext, /*skipEmpty=*/true, false);
    // z2 瓦片 (1,2)：与 dataExtent 几何相交（dataExtent 不含 alpha 语义），
    // 但 alpha 全无效 → skip
    const iptl::TileBbox bbox = iptl::tileBbox(tms, 2, 1, 2, 256, 256);
    const int rc =
        iptl::renderTile(warpDs, bbox, ctx, tms.levels[2].res, 2, 1, 2, err);
    EXPECT_EQ(rc, 1);
    EXPECT_FALSE(std::filesystem::exists(tilePath(2, 1, 2)));
}

TEST(RenderTile, ResumeSkipsExisting) {
    // 断点续跑：已存在瓦片返回 4（与"写出"0 区分——避免统计重复计数）
    auto src = makeSrc(true);
    const iptl::TileMatrixSet tms = iptl::makeWebMercatorQuad(24);
    double res = 0;
    ip::GeoExtent ext;
    std::string err;
    ASSERT_TRUE(iptl::buildWarpedVrt(src.get(), tms, "bilinear",
                                     "/tmp/ip_test_render/warp5.vrt", res, ext,
                                     err))
        << err;
    GDALDataset* warpDs = iptl::threadLocalWarpDs("/tmp/ip_test_render/warp5.vrt");
    ASSERT_NE(warpDs, nullptr);

    iptl::RenderCtx ctx;
    makeCtx(ctx, tms, 2, ext, /*skipEmpty=*/true, false);
    ctx.resume = true;
    const iptl::TileBbox bbox = iptl::tileBbox(tms, 2, 1, 1, 256, 256);
    std::error_code ec;
    std::filesystem::remove(tilePath(2, 1, 1), ec);
    EXPECT_EQ(iptl::renderTile(warpDs, bbox, ctx, tms.levels[2].res, 2, 1, 1, err), 0);
    // 第二次：已存在 → 4，且不重复计为写出
    EXPECT_EQ(iptl::renderTile(warpDs, bbox, ctx, tms.levels[2].res, 2, 1, 1, err), 4);
}

TEST(RenderTile, FillMode) {
    auto src = makeSrc(true);  // 世界 x>0 部分无效 → fill 模式下填 fill_value
    const iptl::TileMatrixSet tms = iptl::makeWebMercatorQuad(24);
    double res = 0;
    ip::GeoExtent ext;
    std::string err;
    ASSERT_TRUE(iptl::buildWarpedVrt(src.get(), tms, "bilinear",
                                     "/tmp/ip_test_render/warp4.vrt", res, ext,
                                     err))
        << err;
    GDALDataset* warpDs =
        iptl::threadLocalWarpDs("/tmp/ip_test_render/warp4.vrt");
    ASSERT_NE(warpDs, nullptr);

    iptl::RenderCtx ctx;
    makeCtx(ctx, tms, 2, ext, /*skipEmpty=*/false, /*fillMode=*/true);
    // z2 瓦片 (2,1)：x∈[0,10M] —— 位于 alpha 无效区，fill 填 128
    const iptl::TileBbox bbox = iptl::tileBbox(tms, 2, 2, 1, 256, 256);
    std::error_code ec;
    std::filesystem::remove(tilePath(2, 2, 1), ec);
    ASSERT_EQ(iptl::renderTile(warpDs, bbox, ctx, tms.levels[2].res, 2, 2, 1, err),
              0)
        << err;
    auto ds = ip::openReadOnly(tilePath(2, 2, 1));
    ASSERT_NE(ds, nullptr);
    std::vector<uint8_t> buf(256 * 256);
    ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 256, 256, buf.data(), 256,
                                   256, GDT_Byte, 0, 0, nullptr);
    // fill 模式：无效像素填 128（有效像素 RGB=100）→ 无 0 值
    int zeros = 0;
    for (int i = 0; i < 256 * 256; ++i) zeros += buf[i] == 0;
    EXPECT_EQ(zeros, 0);
}
