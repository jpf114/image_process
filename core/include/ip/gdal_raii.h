#pragma once
// GDAL dataset RAII 与坐标系助手
// 规范：EPSG:4490 等权威轴序为纬度在前，所有 SRS 必须设置
// OAMS_TRADITIONAL_GIS_ORDER（经度在前），否则坐标转换全流程报错。
#include <gdal_priv.h>
#include <ogr_spatialref.h>

#include <memory>
#include <string>

namespace ip {

struct DataSetDeleter {
    void operator()(GDALDataset* p) const {
        if (p) GDALClose(p);
    }
};
using DataSetPtr = std::unique_ptr<GDALDataset, DataSetDeleter>;

// 打开影像（进程内首次调用自动注册 GDAL 驱动）；失败返回空指针
DataSetPtr openReadOnly(const std::string& path);
DataSetPtr openUpdate(const std::string& path);

// 为 SRS 设置传统 GIS 轴序（经度在前）；所有导出/转换前必须调用
void setTraditionalGisOrder(OGRSpatialReference& srs);

// 由 EPSG 代码构造已设置 GIS 轴序的 SRS；失败抛 std::runtime_error
OGRSpatialReference makeSrsFromEpsg(int epsg);

// 数据集地理范围（minX, minY, maxX, maxY，按 geotransform 计算）
struct GeoExtent {
    double minX = 0, minY = 0, maxX = 0, maxY = 0;
};
GeoExtent datasetExtent(const GDALDataset* ds);

// 地理范围转像素窗口（溢出部分裁剪到影像内）；返回 false 表示无有效重叠
bool geoBoundsToPixelWindow(const GDALDataset* ds, const GeoExtent& bounds,
                            int& x0, int& y0, int& w, int& h);

} // namespace ip
