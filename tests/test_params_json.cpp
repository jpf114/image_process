// 单元测试：TilingParams ↔ JSON 全字段往返（多景按景并行的正确性前提——
// 子进程参数由序列化产生，任何字段遗漏都会导致子任务行为与主任务不一致）
#include "tiling/tiling_params_json.h"

#include <gtest/gtest.h>

namespace {

// 全字段设为非默认值
iptl::TilingParams makeNonDefault() {
    iptl::TilingParams p;
    p.input = "in/scene.tif";
    p.output_dir = "out/tiles";
    p.tiling_scheme = "CGCS2000";
    p.convention = "tms";
    p.min_zoom = 10;
    p.max_zoom = 14;
    p.tile_size = 512;
    p.format = "webp";
    p.jpeg_quality = 71;
    p.webp_quality = 66;
    p.resampling = "lanczos";
    p.nodata_mode = "fill_color";
    p.fill_value = 13;
    p.nodata_pct_threshold = 50;
    p.skip_empty_tiles = false;
    p.num_threads = 3;
    p.num_processes = 4;
    p.cache_mb = 123.5;
    p.tile_buffer = 2;
    p.min_mem_gb = 2.5;
    p.keep_tmp_vrt = true;
    p.build_overviews = "no";
    p.bbox = "105.30,29.02,105.32,29.04";
    p.resume = false;
    p.overwrite = true;
    p.use_gpu = true;
    p.package = "gpkg";
    p.package_file = "out/tiles.gpkg";
    return p;
}

} // namespace

TEST(TilingParamsJson, RoundTripAllFields) {
    const iptl::TilingParams a = makeNonDefault();
    ip::Json j;
    iptl::tilingParamsToJson(a, j);
    iptl::TilingParams b;
    std::string err;
    ASSERT_TRUE(iptl::tilingParamsFromJson(j, b, err)) << err;

    EXPECT_EQ(a.input, b.input);
    EXPECT_EQ(a.output_dir, b.output_dir);
    EXPECT_EQ(a.tiling_scheme, b.tiling_scheme);
    EXPECT_EQ(a.convention, b.convention);
    EXPECT_EQ(a.min_zoom, b.min_zoom);
    EXPECT_EQ(a.max_zoom, b.max_zoom);
    EXPECT_EQ(a.tile_size, b.tile_size);
    EXPECT_EQ(a.format, b.format);
    EXPECT_EQ(a.jpeg_quality, b.jpeg_quality);
    EXPECT_EQ(a.webp_quality, b.webp_quality);
    EXPECT_EQ(a.resampling, b.resampling);
    EXPECT_EQ(a.nodata_mode, b.nodata_mode);
    EXPECT_EQ(a.fill_value, b.fill_value);
    EXPECT_EQ(a.nodata_pct_threshold, b.nodata_pct_threshold);
    EXPECT_EQ(a.skip_empty_tiles, b.skip_empty_tiles);
    EXPECT_EQ(a.num_threads, b.num_threads);
    EXPECT_EQ(a.num_processes, b.num_processes);
    EXPECT_DOUBLE_EQ(a.cache_mb, b.cache_mb);
    EXPECT_EQ(a.tile_buffer, b.tile_buffer);
    EXPECT_DOUBLE_EQ(a.min_mem_gb, b.min_mem_gb);
    EXPECT_EQ(a.keep_tmp_vrt, b.keep_tmp_vrt);
    EXPECT_EQ(a.build_overviews, b.build_overviews);
    EXPECT_EQ(a.bbox, b.bbox);
    EXPECT_EQ(a.resume, b.resume);
    EXPECT_EQ(a.overwrite, b.overwrite);
    EXPECT_EQ(a.use_gpu, b.use_gpu);
    EXPECT_EQ(a.package, b.package);
    EXPECT_EQ(a.package_file, b.package_file);
}

TEST(TilingParamsJson, MissingKeysKeepDefaults) {
    // 只给 input：其余字段必须保持 TilingParams 的默认值
    ip::Json j = ip::Json::object();
    j["input"] = "only_scene.tif";
    iptl::TilingParams p;
    const iptl::TilingParams def;
    std::string err;
    ASSERT_TRUE(iptl::tilingParamsFromJson(j, p, err)) << err;
    EXPECT_EQ(p.input, "only_scene.tif");
    EXPECT_EQ(p.tiling_scheme, def.tiling_scheme);
    EXPECT_EQ(p.tile_size, def.tile_size);
    EXPECT_EQ(p.format, def.format);
    EXPECT_EQ(p.num_processes, def.num_processes);
    EXPECT_EQ(p.min_mem_gb, def.min_mem_gb);
    EXPECT_EQ(p.package, def.package);
    EXPECT_TRUE(p.resume);
}

TEST(TilingParamsJson, OverviewPolicyAliasWins) {
    // 等价别名：overview_policy 覆盖 build_overviews（方案 §5.2）
    ip::Json j = ip::Json::object();
    j["build_overviews"] = "no";
    j["overview_policy"] = "yes";
    iptl::TilingParams p;
    std::string err;
    ASSERT_TRUE(iptl::tilingParamsFromJson(j, p, err)) << err;
    EXPECT_EQ(p.build_overviews, "yes");
}

TEST(TilingParamsJson, TypeErrorReported) {
    ip::Json j = ip::Json::object();
    j["min_zoom"] = "not_a_number";
    iptl::TilingParams p;
    std::string err;
    EXPECT_FALSE(iptl::tilingParamsFromJson(j, p, err));
    EXPECT_FALSE(err.empty());
}
