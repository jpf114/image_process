// tiling CLI（薄壳：解析参数 → 调 operator）
#include "tiling/i_tiling.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void usage() {
    std::cout
        << "用法: tiling --input <影像|VRT> --output-dir <目录>\n"
           "  [选项]\n"
           "  --tiling-scheme <WebMercatorQuad|WorldCRS84Quad|CGCS2000|tms.json>\n"
           "                                           默认 WebMercatorQuad\n"
           "  --convention <xyz|tms>                   默认 xyz\n"
           "  --min-zoom <n>   --max-zoom <n>          默认自动（源分辨率锚定）\n"
           "  --tile-size <256>                        2 的幂（64~1024）\n"
           "  --format <png|jpeg|webp>                 默认 png\n"
           "  --jpeg-quality <85>  --webp-quality <85>\n"
           "  --resampling <near|bilinear|cubic|average|lanczos>  默认 bilinear\n"
           "  --nodata-mode <transparent|fill_color>   默认 transparent\n"
           "  --fill-value <255>\n"
           "  --nodata-pct-threshold <100>             无效占比阈值（边缘瓦片可 50）\n"
           "  --skip-empty-tiles <true|false>          默认 true\n"
           "  --num-threads <n>                        默认物理核数\n"
           "  --build-overviews <auto|yes|no>          无 ovr 时自动建（低 zoom 提速）\n"
           "  --bbox <minX,minY,maxX,maxY>             增量范围（TMS CRS 坐标，空=全量）\n"
           "  --no-resume                              忽略已存在瓦片（默认续跑跳过）\n"
           "  --overwrite                              强制重写已存在瓦片\n"
           "  --keep-tmp-vrt                           保留 WarpedVRT（调试）\n"
           "  -h | --help\n";
}

bool parseBool(const std::string& s) { return s == "true" || s == "1" || s == "yes"; }

} // namespace

int main(int argc, char** argv) {
    iptl::TilingParams p;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "缺少参数值: " << name << std::endl;
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a == "--input") {
            p.input = need("--input");
        } else if (a == "--output-dir") {
            p.output_dir = need("--output-dir");
        } else if (a == "--tiling-scheme") {
            p.tiling_scheme = need("--tiling-scheme");
        } else if (a == "--convention") {
            p.convention = need("--convention");
        } else if (a == "--min-zoom") {
            p.min_zoom = std::atoi(need("--min-zoom"));
        } else if (a == "--max-zoom") {
            p.max_zoom = std::atoi(need("--max-zoom"));
        } else if (a == "--tile-size") {
            p.tile_size = std::atoi(need("--tile-size"));
        } else if (a == "--format") {
            p.format = need("--format");
        } else if (a == "--jpeg-quality") {
            p.jpeg_quality = std::atoi(need("--jpeg-quality"));
        } else if (a == "--webp-quality") {
            p.webp_quality = std::atoi(need("--webp-quality"));
        } else if (a == "--resampling") {
            p.resampling = need("--resampling");
        } else if (a == "--nodata-mode") {
            p.nodata_mode = need("--nodata-mode");
        } else if (a == "--fill-value") {
            p.fill_value = std::atoi(need("--fill-value"));
        } else if (a == "--nodata-pct-threshold") {
            p.nodata_pct_threshold = std::atoi(need("--nodata-pct-threshold"));
        } else if (a == "--skip-empty-tiles") {
            p.skip_empty_tiles = parseBool(need("--skip-empty-tiles"));
        } else if (a == "--num-threads") {
            p.num_threads = std::atoi(need("--num-threads"));
        } else if (a == "--build-overviews") {
            p.build_overviews = need("--build-overviews");
        } else if (a == "--bbox") {
            p.bbox = need("--bbox");
        } else if (a == "--no-resume") {
            p.resume = false;
        } else if (a == "--overwrite") {
            p.overwrite = true;
        } else if (a == "--keep-tmp-vrt") {
            p.keep_tmp_vrt = true;
        } else {
            std::cerr << "未知参数: " << a << std::endl;
            usage();
            return 2;
        }
    }

    std::string err;
    iptl::ITiling* op = createTiling();
    if (!op) {
        std::cerr << "创建 operator 失败" << std::endl;
        return 1;
    }
    if (!op->configure(p, err)) {
        std::cerr << "参数错误: " << err << std::endl;
        destroyTiling(op);
        return 2;
    }
    iptl::TilingStats stats;
    const bool ok = op->execute(
        [](int, const std::string&) {}, stats, err);
    destroyTiling(op);
    if (!ok) {
        std::cerr << "执行失败: " << err << std::endl;
        return 1;
    }
    return 0;
}
