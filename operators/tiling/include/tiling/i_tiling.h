#pragma once
// tiling operator 公共接口（动态库）
//
// 切片引擎：GDAL Warp API（每 zoom 级 WarpedVRT）+ 瓦片级线程池并行。
// TMS：WebMercatorQuad(3857) / WorldCRS84Quad(4326) / CGCS2000(4490 天地图规范，
//      层级从 1 起，输出目录按 identifier) / 任意 OGC TwoD TileMatrixSet JSON。
// nodata：transparent（PNG/WebP 带 alpha）/ fill_color（填色，无 alpha）；
//         全无效瓦片可跳过；无效占比阈值控制边缘瓦片。
// 输入支持匀色 VRT（需与 color_balance 同进程——像素函数进程内注册）。
#include <functional>
#include <string>
#include <vector>

namespace iptl {

struct TilingParams {
    std::string input;                 // 影像 / VRT（含匀色 VRT）
    std::string output_dir;            // 瓦片根目录 <out>/<z>/<x>/<y>.<ext>

    std::string tiling_scheme = "WebMercatorQuad";
    // WebMercatorQuad | WorldCRS84Quad | CGCS2000 | <自定义 tms json 路径>
    std::string convention = "xyz";    // xyz（y 从上）| tms（y 从下）
    int min_zoom = -1;                 // -1 = 自动（0/1 起）
    int max_zoom = -1;                 // -1 = 由源分辨率推算（不超采样）
    int tile_size = 256;               // 256 / 512 / 1024
    std::string format = "png";        // png | jpeg | webp
    int jpeg_quality = 85;
    int webp_quality = 85;
    std::string resampling = "bilinear"; // near|bilinear|cubic|average|lanczos
    std::string nodata_mode = "transparent"; // transparent | fill_color
    int fill_value = 255;              // fill_color / JPEG 无 alpha 时的填充值
    int nodata_pct_threshold = 100;    // 无效占比 ≥ 阈值 → 整瓦片透明/填充（边缘瓦片可 50）
    bool skip_empty_tiles = true;      // 全无效瓦片不落盘
    int num_threads = 0;               // 0 = 物理核数
    int num_processes = 1;             // 多景任务级并行（V1=1 串行，多进程随 Phase 4 服务化）
    double cache_mb = 512;             // GDAL 块缓存（MB，execute 时生效）
    int tile_buffer = 0;               // 增量切片时范围外扩的缓冲瓦片数（覆盖接边带）
    double min_mem_gb = 1.5;           // 内存护栏
    bool keep_tmp_vrt = false;         // 保留每级 WarpedVRT 临时文件（调试）
    std::string build_overviews = "auto"; // auto（无 ovr 时自动建，AVERAGE 2~64）| yes | no
    std::string bbox;                  // 增量范围 "minX,minY,maxX,maxY"（空=全量，源 SRS 坐标）
    bool resume = true;                // 断点续跑：已存在瓦片跳过
    bool overwrite = false;            // true 时强制重写已存在瓦片
    bool use_gpu = false;              // GPU 占位（本期纯 CPU，开启仅告警）
};

using ProgressCallback = std::function<void(int percent, const std::string& msg)>;

struct TilingStats {
    long tilesWritten = 0;
    long tilesResumed = 0;     // 断点续跑跳过（已存在）
    long tilesSkippedEmpty = 0;
    long tilesSkippedThreshold = 0;
    long tilesOutsideData = 0;
    double elapsedSec = 0;
};

class ITiling {
public:
    virtual ~ITiling() = default;

    virtual bool configure(const TilingParams& p, std::string& err) = 0;
    virtual bool execute(const ProgressCallback& cb, TilingStats& stats,
                         std::string& err) = 0;
};

} // namespace iptl

extern "C" {

iptl::ITiling* createTiling();
void destroyTiling(iptl::ITiling* op);

}
