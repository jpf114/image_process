#include "ip/gdal_raii.h"

#include <algorithm>
#include <mutex>
#include <stdexcept>

namespace ip {

namespace {
// GDAL 驱动懒注册：进程内首个打开调用前必须完成，否则 Open 静默失败（无错误消息）
void ensureGdalRegistered() {
    static std::once_flag flag;
    std::call_once(flag, [] { GDALAllRegister(); });
}
} // namespace

DataSetPtr openReadOnly(const std::string& path) {
    ensureGdalRegistered();
    return DataSetPtr(
        GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
}

DataSetPtr openUpdate(const std::string& path) {
    ensureGdalRegistered();
    return DataSetPtr(
        GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_UPDATE));
}

void setTraditionalGisOrder(OGRSpatialReference& srs) {
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
}

OGRSpatialReference makeSrsFromEpsg(int epsg) {
    OGRSpatialReference srs;
    setTraditionalGisOrder(srs);
    if (srs.importFromEPSG(epsg) != OGRERR_NONE) {
        throw std::runtime_error("无法识别 EPSG:" + std::to_string(epsg));
    }
    return srs;
}

GeoExtent datasetExtent(const GDALDataset* ds) {
    GeoExtent e;
    double gt[6];
    const_cast<GDALDataset*>(ds)->GetGeoTransform(gt);
    const double minX = gt[0];
    const double maxY = gt[3];
    const double maxX = gt[0] + gt[1] * ds->GetRasterXSize();
    const double minY = gt[3] + gt[5] * ds->GetRasterYSize();
    e.minX = std::min(minX, maxX);
    e.maxX = std::max(minX, maxX);
    e.minY = std::min(minY, maxY);
    e.maxY = std::max(minY, maxY);
    return e;
}

bool geoBoundsToPixelWindow(const GDALDataset* ds, const GeoExtent& bounds,
                            int& x0, int& y0, int& w, int& h) {
    double gt[6];
    const_cast<GDALDataset*>(ds)->GetGeoTransform(gt);
    if (gt[1] == 0 || gt[5] == 0) return false;

    const double imgMinX = std::min(gt[0], gt[0] + gt[1] * ds->GetRasterXSize());
    const double imgMaxX = std::max(gt[0], gt[0] + gt[1] * ds->GetRasterXSize());
    const double imgMinY = std::min(gt[3], gt[3] + gt[5] * ds->GetRasterYSize());
    const double imgMaxY = std::max(gt[3], gt[3] + gt[5] * ds->GetRasterYSize());

    const double cx0 = std::max(bounds.minX, imgMinX);
    const double cx1 = std::min(bounds.maxX, imgMaxX);
    const double cy0 = std::max(bounds.minY, imgMinY);
    const double cy1 = std::min(bounds.maxY, imgMaxY);
    if (cx0 >= cx1 || cy0 >= cy1) return false;

    const int px0 = static_cast<int>(std::floor((cx0 - gt[0]) / gt[1]));
    const int px1 = static_cast<int>(std::ceil((cx1 - gt[0]) / gt[1]));
    const int py0 = static_cast<int>(std::floor((gt[3] - cy1) / -gt[5]));
    const int py1 = static_cast<int>(std::ceil((gt[3] - cy0) / -gt[5]));

    x0 = std::max(px0, 0);
    y0 = std::max(py0, 0);
    const int x1 = std::min(px1, ds->GetRasterXSize());
    const int y1 = std::min(py1, ds->GetRasterYSize());
    if (x0 >= x1 || y0 >= y1) return false;
    w = x1 - x0;
    h = y1 - y0;
    return true;
}

} // namespace ip
