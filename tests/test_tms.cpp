// 单元测试：TMS 几何（天地图公式/层级偏移/JSON 解析/瓦片范围）
#include "tms.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>

TEST(Tms, WebMercatorQuadLevel0) {
    const iptl::TileMatrixSet t = iptl::makeWebMercatorQuad(24);
    ASSERT_EQ(t.levels.size(), 24u);
    EXPECT_NEAR(t.levels[0].res, 156543.03392804062, 1e-6);
    EXPECT_NEAR(t.levels[0].topLeftX, -20037508.342789248, 1e-3);
    EXPECT_NEAR(t.levels[0].topLeftY, 20037508.342789248, 1e-3);
    EXPECT_EQ(t.levels[0].matrixW, 1);
    EXPECT_EQ(t.levels[3].matrixW, 8);
    EXPECT_NEAR(t.levels[1].res, t.levels[0].res / 2, 1e-9);
}

TEST(Tms, Cgcs2000TiandituOffset) {
    const iptl::TileMatrixSet t = iptl::makeCGCS2000(24);
    EXPECT_EQ(t.epsg, 4490);
    EXPECT_TRUE(t.geographic);
    EXPECT_EQ(t.idOffset, 1);            // 天地图层级从 1 起
    EXPECT_EQ(t.dirLevel(0), 1);
    EXPECT_EQ(t.levels[0].matrixW, 2);   // z1 全球 2x1
    EXPECT_EQ(t.levels[0].matrixH, 1);
    EXPECT_NEAR(t.levels[0].res, 0.703125, 1e-12);
    EXPECT_NEAR(t.levels[4].res, 0.703125 / 16, 1e-12);
}

TEST(Tms, TileBboxWebMercator) {
    const iptl::TileMatrixSet t = iptl::makeWebMercatorQuad(24);
    // z1 瓦片 (1,0)：右上半球
    const iptl::TileBbox b = iptl::tileBbox(t, 1, 1, 0, 256, 256);
    EXPECT_NEAR(b.minX, 0.0, 1e-3);
    EXPECT_NEAR(b.maxY, 20037508.342789248, 1e-3);
    EXPECT_NEAR(b.minY, 0.0, 1e-3);
    // z1 瓦片 (0,1)：左下半球
    const iptl::TileBbox b2 = iptl::tileBbox(t, 1, 0, 1, 256, 256);
    EXPECT_NEAR(b2.maxX, 0.0, 1e-3);
    EXPECT_NEAR(b2.minY, -20037508.342789248, 1e-3);
}

TEST(Tms, LevelTileRangeKnown) {
    const iptl::TileMatrixSet t = iptl::makeCGCS2000(24);
    // 数据范围（2024 mosaic）：105.2498..105.7502, 28.833..29.167
    // 天地图 z10（索引 9）：span=0.703125/2^9*256=0.3515625°
    long long tx0, ty0, tx1, ty1;
    ASSERT_TRUE(iptl::levelTileRange(t, 9, 256, 256, 105.2498, 28.8331,
                                     105.7502, 29.1668, tx0, ty0, tx1, ty1));
    // x: floor((105.2498+180)/0.3515625)=floor(811.0)=811..floor(285.7502/0.3515625+180)=812
    EXPECT_EQ(tx0, 811);
    EXPECT_EQ(tx1, 812);
    // y: floor((90-29.1668)/0.3515625)=173
    EXPECT_EQ(ty0, 173);
    EXPECT_EQ(ty1, 173);
}

TEST(Tms, ParseTmsJsonAxisOrder) {
    // OGC TMS JSON：4490 lat-first，topLeftCorner 必须写 [90,-180]（Phase 0 教训）
    const char* path = "/tmp/ip_test_tms_CGCS2000.json";
    {
        std::ofstream f(path);
        f << R"({
  "identifier": "CGCS2000",
  "supportedCRS": "http://www.opengis.net/def/crs/EPSG/0/4490",
  "tileMatrix": [
    {"identifier":"1","scaleDenominator":279541141.17044446,
     "topLeftCorner":[90.0,-180.0],"tileWidth":256,"tileHeight":256,
     "matrixWidth":2,"matrixHeight":1}
  ]
})";
    }
    iptl::TileMatrixSet t;
    std::string err;
    ASSERT_TRUE(iptl::parseTmsJson(path, t, err)) << err;
    EXPECT_EQ(t.epsg, 4490);
    EXPECT_TRUE(t.geographic);
    ASSERT_EQ(t.levels.size(), 1u);
    // res = scale*0.28e-3/111319.49
    EXPECT_NEAR(t.levels[0].res, 0.703125, 1e-6);
    EXPECT_NEAR(t.levels[0].topLeftY, 90.0, 1e-12);
    std::filesystem::remove(path);
}

TEST(Tms, ParseTmsJsonBadCrs) {
    const char* path = "/tmp/ip_test_tms_bad.json";
    {
        std::ofstream f(path);
        f << R"({"identifier":"x","supportedCRS":"bad-crs","tileMatrix":[]})";
    }
    iptl::TileMatrixSet t;
    std::string err;
    EXPECT_FALSE(iptl::parseTmsJson(path, t, err));
    std::filesystem::remove(path);
}
