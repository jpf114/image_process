// workflow exe：JSON 配置驱动的「匀色 → 切片」一键管线
//
// 核心架构（技术方案 4.3/4.4）：color_balance 以 VRT 模式输出（零像素落盘），
// tiling 直接读取匀色 VRT（同进程，像素函数已注册）——省 TB 级磁盘与一次读写。
// 断点续跑：瓦片级（已存在跳过，由 tiling resume 实现）。
//
// 配置文件（JSON）示例：
// {
//   "color_balance": {
//     "input": "data/2024", "output_dir": "out/balanced",
//     "method": "overlap_adjust", "output_mode": "vrt",
//     "ref_image": "H48E018014C"
//   },
//   "tiling": {
//     "input": "",              // 留空 = 自动接匀色镶嵌 VRT
//     "output_dir": "out/tiles",
//     "tiling_scheme": "CGCS2000", "min_zoom": 10, "max_zoom": 14,
//     "format": "png", "build_overviews": "auto"
//   }
// }
#include "color_balance/i_color_balance.h"
#include "tiling/i_tiling.h"

#include "ip/json_io.h"
#include "ip/log.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void usage() {
    std::cout
        << "用法: workflow --config <workflow.json> [--only color|tiling]\n"
           "\n配置 JSON 结构见项目 AGENT.md §7 或源码头部注释。\n"
           "  --only color    仅执行匀色\n"
           "  --only tiling   仅执行切片（tiling.input 须在配置中给出）\n";
}

// 从 JSON 读数值列表（数字 → 1 值；数组 → 3 值），供 target_mean/std
void readNumList(const ip::Json& j, const char* key, std::vector<double>& out) {
    if (!j.contains(key)) return;
    const auto& v = j[key];
    if (v.is_number()) {
        out = {v.get<double>()};
    } else if (v.is_array()) {
        out.clear();
        for (const auto& e : v) out.push_back(e.get<double>());
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string configPath;
    std::string only;  // 空=全部；color；tiling
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a == "--config") {
            if (i + 1 >= argc) {
                std::cerr << "缺少 --config 值" << std::endl;
                return 2;
            }
            configPath = argv[++i];
        } else if (a == "--only") {
            if (i + 1 >= argc) {
                std::cerr << "缺少 --only 值" << std::endl;
                return 2;
            }
            only = argv[++i];
        } else {
            std::cerr << "未知参数: " << a << std::endl;
            usage();
            return 2;
        }
    }
    if (configPath.empty()) {
        usage();
        return 2;
    }
    // 匀色 VRT 需要进程内像素函数（幂等注册；--only tiling 读既有 VRT 时同样必需）
    ensureColorBalancePixelFunc();

    ip::Json cfg;
    std::string err;
    if (!ip::readJsonFile(configPath, cfg, err)) {
        std::cerr << "读取配置失败: " << err << std::endl;
        return 1;
    }

    // ============ 匀色 ============
    std::string mosaicVrt;  // 匀色镶嵌 VRT（tiling 输入）
    if (only.empty() || only == "color") {
        if (!cfg.contains("color_balance")) {
            std::cerr << "配置缺少 color_balance 段" << std::endl;
            return 2;
        }
        const ip::Json jc = cfg["color_balance"];
        ipcb::ColorBalanceParams p;
        if (jc.contains("input")) {
            for (const auto& it : jc["input"]) p.inputs.push_back(it.get<std::string>());
        }
        if (jc.contains("output_dir")) p.output_dir = jc["output_dir"].get<std::string>();
        if (jc.contains("method")) p.method = jc["method"].get<std::string>();
        if (jc.contains("output_mode")) p.output_mode = jc["output_mode"].get<std::string>();
        if (jc.contains("scene_format")) p.scene_format = jc["scene_format"].get<std::string>();
        if (jc.contains("mosaic_cog")) p.mosaic_cog = jc["mosaic_cog"].get<bool>();
        if (jc.contains("profile_mode")) p.profile_mode = jc["profile_mode"].get<std::string>();
        if (jc.contains("ref_image")) p.ref_image = jc["ref_image"].get<std::string>();
        readNumList(jc, "target_mean", p.target_mean);
        readNumList(jc, "target_std", p.target_std);
        if (jc.contains("seam_check_threshold"))
            p.seam_check_threshold = jc["seam_check_threshold"].get<double>();
        if (jc.contains("min_overlap_px")) p.min_overlap_px = jc["min_overlap_px"].get<int>();
        if (jc.contains("overlap_max_side"))
            p.overlap_max_side = jc["overlap_max_side"].get<int>();
        if (jc.contains("apply_chunk")) p.apply_chunk = jc["apply_chunk"].get<int>();
        if (jc.contains("block_size")) p.block_size = jc["block_size"].get<int>();
        if (jc.contains("wallis_target_std"))
            p.wallis_target_std = jc["wallis_target_std"].get<double>();
        if (jc.contains("use_gpu")) p.use_gpu = jc["use_gpu"].get<bool>();
        if (jc.contains("min_mem_gb")) p.min_mem_gb = jc["min_mem_gb"].get<double>();
        if (jc.contains("profile_path")) p.profile_path = jc["profile_path"].get<std::string>();
        if (jc.contains("write_profile")) p.write_profile = jc["write_profile"].get<bool>();

        ipcb::IColorBalance* cb = createColorBalance();
        if (!cb) {
            std::cerr << "创建匀色 operator 失败" << std::endl;
            return 1;
        }
        if (!cb->configure(p, err)) {
            std::cerr << "匀色参数错误: " << err << std::endl;
            destroyColorBalance(cb);
            return 2;
        }
        ip::logInfo("==== 工作流步骤 1：匀色 ====");
        if (!cb->execute(nullptr, err)) {
            std::cerr << "匀色失败: " << err << std::endl;
            destroyColorBalance(cb);
            return 1;
        }
        destroyColorBalance(cb);
        // 匀色镶嵌 VRT 路径（vrt 模式下由 color_balance 生成）
        mosaicVrt = (std::filesystem::path(p.output_dir) / "mosaic.vrt").string();
    }

    // ============ 切片 ============
    if (only.empty() || only == "tiling") {
        if (!cfg.contains("tiling")) {
            std::cerr << "配置缺少 tiling 段" << std::endl;
            return 2;
        }
        const ip::Json jt = cfg["tiling"];
        iptl::TilingParams p;
        if (jt.contains("input")) p.input = jt["input"].get<std::string>();
        if (p.input.empty()) {
            p.input = mosaicVrt;  // 自动接匀色镶嵌 VRT
            ip::logInfo("tiling.input 未指定 → 使用匀色镶嵌 VRT: " + p.input);
        }
        if (jt.contains("output_dir")) p.output_dir = jt["output_dir"].get<std::string>();
        if (jt.contains("tiling_scheme")) p.tiling_scheme = jt["tiling_scheme"].get<std::string>();
        if (jt.contains("convention")) p.convention = jt["convention"].get<std::string>();
        if (jt.contains("min_zoom")) p.min_zoom = jt["min_zoom"].get<int>();
        if (jt.contains("max_zoom")) p.max_zoom = jt["max_zoom"].get<int>();
        if (jt.contains("tile_size")) p.tile_size = jt["tile_size"].get<int>();
        if (jt.contains("format")) p.format = jt["format"].get<std::string>();
        if (jt.contains("resampling")) p.resampling = jt["resampling"].get<std::string>();
        if (jt.contains("nodata_mode")) p.nodata_mode = jt["nodata_mode"].get<std::string>();
        if (jt.contains("fill_value")) p.fill_value = jt["fill_value"].get<int>();
        if (jt.contains("nodata_pct_threshold"))
            p.nodata_pct_threshold = jt["nodata_pct_threshold"].get<int>();
        if (jt.contains("skip_empty_tiles")) p.skip_empty_tiles = jt["skip_empty_tiles"].get<bool>();
        if (jt.contains("num_threads")) p.num_threads = jt["num_threads"].get<int>();
        if (jt.contains("num_processes")) p.num_processes = jt["num_processes"].get<int>();
        if (jt.contains("cache_mb")) p.cache_mb = jt["cache_mb"].get<double>();
        if (jt.contains("tile_buffer")) p.tile_buffer = jt["tile_buffer"].get<int>();
        if (jt.contains("build_overviews")) p.build_overviews = jt["build_overviews"].get<std::string>();
        if (jt.contains("overview_policy")) p.build_overviews = jt["overview_policy"].get<std::string>();
        if (jt.contains("bbox")) p.bbox = jt["bbox"].get<std::string>();
        if (jt.contains("resume")) p.resume = jt["resume"].get<bool>();
        if (jt.contains("overwrite")) p.overwrite = jt["overwrite"].get<bool>();
        if (jt.contains("keep_tmp_vrt")) p.keep_tmp_vrt = jt["keep_tmp_vrt"].get<bool>();
        if (jt.contains("use_gpu")) p.use_gpu = jt["use_gpu"].get<bool>();
        if (jt.contains("min_mem_gb")) p.min_mem_gb = jt["min_mem_gb"].get<double>();

        iptl::ITiling* tl = createTiling();
        if (!tl) {
            std::cerr << "创建切片 operator 失败" << std::endl;
            return 1;
        }
        if (!tl->configure(p, err)) {
            std::cerr << "切片参数错误: " << err << std::endl;
            destroyTiling(tl);
            return 2;
        }
        ip::logInfo("==== 工作流步骤 2：切片 ====");
        iptl::TilingStats stats;
        if (!tl->execute(nullptr, stats, err)) {
            std::cerr << "切片失败: " << err << std::endl;
            destroyTiling(tl);
            return 1;
        }
        destroyTiling(tl);
    }

    ip::logInfo("==== 工作流完成 ====");
    return 0;
}
