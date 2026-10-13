#pragma once
// tiling 内部共享：WarpedVRT 构建、瓦片渲染、编码
#include "tiling/i_tiling.h"

#include "ip/gdal_raii.h"
#include "tms.h"

#include <gdalwarper.h>

#include <atomic>
#include <string>
#include <vector>

namespace iptl {

// 重采样字符串 → GDAL 枚举
GDALResampleAlg parseResampleAlg(const std::string& name);

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
    bool resume = true;          // 已存在瓦片跳过
    bool overwrite = false;
};

// 渲染并写出一张瓦片；返回值：
//   0=写出，1=跳过（全无效），2=跳过（阈值），3=数据外，4=续跑跳过（已存在），-1=错误
int renderTile(GDALDataset* warpDs, const TileBbox& bbox, RenderCtx& ctx,
               double levelRes, int dirLevel, long long tx, long long tyXYZ,
               std::string& err);

// 线程本地打开 warp VRT（每线程独立 handle，GDAL 线程安全前提）
GDALDataset* threadLocalWarpDs(const std::string& vrtPath);

} // namespace iptl
