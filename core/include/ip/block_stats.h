#pragma once
// 分块统计（恒定内存）：
//  - exactStatsRGB：整景分块直方图精确统计（逐 256 级直方图，仅统计有效像素）
//  - windowStatsRGB：指定像素窗口统计（窗口过大时按最大边降采样读取）
// 规范：统计必须排除 nodata/无效像素（alpha=0 或 nodata 值）
#include "ip/gdal_raii.h"

#include <cstdint>

namespace ip {

struct BandStats3 {
    double mean[3] = {0, 0, 0};
    double std[3]  = {0, 0, 0};
    uint64_t validCount[3] = {0, 0, 0};
};

// 整景精确统计（前 3 个波段，BYTE）。有效掩码：波段 4 为 alpha 时取 alpha>0，
// 否则取 RGB 波段各自 nodata 之外的像素。分块读取，内存恒定。
BandStats3 exactStatsRGB(GDALDataset* ds);

// 像素窗口统计（前 3 波段，BYTE），**保持像素原始值**（禁止 buf 降采样——
// 重采样会平滑高频使 σ 被系统性压小，导致 gain 解算偏大，Phase 1 实测教训）：
//  - 窗口两边均 <= maxSide：直接全读（原始分辨率）
//  - 否则：均匀采样 ~maxSide² 像素量的 256×256 tile（覆盖全窗口，无重采样）
BandStats3 windowStatsRGB(GDALDataset* ds, int x0, int y0, int w, int h,
                          int maxSide = 2048);

} // namespace ip
