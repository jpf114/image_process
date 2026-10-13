#pragma once
// tiling 内部共享：WarpedVRT 构建、瓦片渲染、编码
#include "tiling/i_tiling.h"

#include "ip/gdal_raii.h"
#include "tms.h"

#include <gdalwarper.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace iptl {

// 重采样字符串 → GDAL 枚举
GDALResampleAlg parseResampleAlg(const std::string& name);

// 瓦片落盘目标：默认散文件；打包模式（package=mbtiles|gpkg）走此接口。
// 实现内部自行保证线程安全（多 worker 并发调用）。
class TileSink {
public:
    virtual ~TileSink() = default;

    // 写入一张瓦片：S×S×outBands 的带平面（band-planar）原始像素。
    // dirLevel 为输出层级号（= 矢量索引 + idOffset，见 tms.dirLevel）。
    // 返回 false 并填充 err 表示失败。
    virtual bool write(int dirLevel, long long tx, long long tyXYZ,
                       const uint8_t* data, std::string& err) = 0;

    // 全部瓦片写完后收尾（关闭句柄、补写包元数据）
    virtual bool finish(std::string& err) = 0;
};

// 构建打包 sink。firstIdx/lastIdx 为本轮要写入的最小/最大层级矢量索引
// （调用方已夹取）；失败返回 nullptr 并填充 err。
TileSink* createPackageSink(const TilingParams& params, const TileMatrixSet& tms,
                            int firstIdx, int lastIdx, int outBands,
                            int tileSize, std::string& err);

// 将 TileMatrixSet 导出为 OGC TMS JSON（GPKG 的 TILING_SCHEME 需要）
bool writeTmsJsonFile(const TileMatrixSet& tms, int tileSize,
                      const std::string& path, std::string& err);

// 构建每级瓦片渲染用的 WarpedVRT（可控重采样算法），落盘 vrt 供多线程各自打开。
// 返回其分辨率（目标 CRS 下）与总 bbox。
bool buildWarpedVrt(GDALDataset* src, const TileMatrixSet& tms,
                    const std::string& resampling, const std::string& vrtPath,
                    double& resX, ip::GeoExtent& extentInTarget,
                    std::string& err);

// 瓦片渲染参数
struct RenderCtx {
    int nBands = 0;              // warp 后波段数（3 或 4）
    int tileSize = 256;
    int threshold = 100;         // 无效占比阈值（%）：无效占比 ≥ 阈值 → 整瓦片不落盘
    bool skipEmpty = true;
    bool fillMode = false;       // nodata_mode=fill_color
    int fillValue = 255;
    std::string format = "png";
    int jpegQuality = 85;
    int webpQuality = 85;
    GDALResampleAlg alg = GRA_Bilinear;
    std::string outputDir;
    bool jpegWarned = false;
    ip::GeoExtent dataExtent;    // warp 数据范围（交集计算）
    bool resume = true;          // 已存在瓦片跳过（仅散文件模式）
    bool overwrite = false;
    TileSink* sink = nullptr;    // 非空 → 写包（不落散文件）
    int outBands = 0;            // 包内波段数（jpeg=3；否则 = nBands）
};

// 渲染并写出一张瓦片；返回值：
//   0=写出，1=跳过（全无效），2=跳过（阈值），3=数据外，4=续跑跳过（已存在），-1=错误
int renderTile(GDALDataset* warpDs, const TileBbox& bbox, RenderCtx& ctx,
               double levelRes, int dirLevel, long long tx, long long tyXYZ,
               std::string& err);

// 线程本地打开 warp VRT（每线程独立 handle，GDAL 线程安全前提）
GDALDataset* threadLocalWarpDs(const std::string& vrtPath);

} // namespace iptl
