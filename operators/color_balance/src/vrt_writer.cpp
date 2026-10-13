// color_balance：VRT 输出（匀色 VRT + 镶嵌 VRT，零像素落盘）
// 机制：VRTDerivedRasterBand + 进程内注册像素函数 "ip_gain_offset"（参数经
// <PixelFunctionArguments> 注入，Phase 0 验证读取开销 0.96x）。
// 约束：像素函数为本进程注册，VRT 需在同进程（如切片 operator）内打开。
#include "cb_internal.h"

#include "ip/log.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <vector>

namespace ipcb {

namespace {

// 像素函数：out = clip(src * gain + offset, 0, 255)（BYTE）
// GDAL 3.12 GDALDerivedPixelFuncWithArgs 签名
CPLErr gainOffsetPixelFunc(void** papoSources, int nSources, void* pData,
                           int nXSize, int nYSize, GDALDataType eSrcType,
                           GDALDataType eBufType, int nBandCount,
                           int nBufferRadius,
                           const char* const* apszFunctionArgs) {
    if (nSources != 1 || nBandCount != 1) {
        std::fprintf(stderr, "[pixelfunc] FAIL nSources=%d nBandCount=%d\n",
                     nSources, nBandCount);
        return CE_Failure;
    }
    if (eSrcType != GDT_Byte || eBufType != GDT_Byte) {
        std::fprintf(stderr, "[pixelfunc] FAIL srcType=%d bufType=%d\n",
                     static_cast<int>(eSrcType), static_cast<int>(eBufType));
        return CE_Failure;
    }
    // GDAL 3.12 默认 nBufferRadius=64（数据带边框缓冲），仅处理中心区，
    // 边框值由 GDAL 忽略。

    double gain = 1.0, offset = 0.0;
    if (apszFunctionArgs) {
        for (const char* const* it = apszFunctionArgs; *it; ++it) {
            const std::string item(*it);
            const size_t eq = item.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = item.substr(0, eq);
            const double v = std::atof(item.c_str() + eq + 1);
            if (k == "gain") gain = v;
            if (k == "offset") offset = v;
        }
    }

    // 缓冲布局（GDAL 3.12 实测确认）：nBufferRadius 虽传 64，但源缓冲
    // **未扩边**——就是原始请求窗（nXSize×nYSize，行宽 nXSize，无偏移）；
    // 输出 pData 同布局。radius 仅是信息参数（XML 声明 <BufferRadius> 才扩边）。
    const uint8_t* src = static_cast<const uint8_t*>(papoSources[0]);
    uint8_t* dst = static_cast<uint8_t*>(pData);
    const size_t n = static_cast<size_t>(nXSize) * nYSize;
    for (size_t i = 0; i < n; ++i) {
        const double v = src[i] * gain + offset;
        int vi = static_cast<int>(std::lround(v));
        dst[i] = static_cast<uint8_t>(std::min(std::max(vi, 0), 255));
    }
    return CE_None;
}

std::string xmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

// SRS XML 属性：dataAxisToSRSAxisMapping 必须显式透传（缺省时 GDAL 按默认轴序
// 解释 4490 等 lat-first CRS，warp 坐标交换导致内容错位——实测坑）
std::string axisMappingAttr(const OGRSpatialReference* srs) {
    return srs->GetAxisMappingStrategy() == OAMS_TRADITIONAL_GIS_ORDER ? "1,2"
                                                                       : "2,1";
}

std::string srsXml(const OGRSpatialReference* srs) {
    char* wkt = nullptr;
    OGRSpatialReference tmp(*srs);
    tmp.exportToWkt(&wkt);
    if (!wkt) return "";
    std::string out = "  <SRS dataAxisToSRSAxisMapping=\"" +
                      axisMappingAttr(srs) + "\">" + xmlEscape(wkt) +
                      "</SRS>\n";
    CPLFree(wkt);
    return out;
}

} // namespace

void ensurePixelFuncRegistered() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        const CPLErr rc = GDALAddDerivedBandPixelFuncWithArgs(
            "ip_gain_offset", gainOffsetPixelFunc, nullptr);
        if (rc != CE_None) {
            ip::logError("注册像素函数 ip_gain_offset 失败: " +
                         std::string(CPLGetLastErrorMsg()));
        }
    });
}

bool writeBalancedVrt(GDALDataset* src, const std::string& vrtPath,
                      const double gain[3], const double offset[3],
                      std::string& err) {
    ensurePixelFuncRegistered();
    const int w = src->GetRasterXSize();
    const int h = src->GetRasterYSize();
    const int nb = src->GetRasterCount();
    if (nb < 3) {
        err = "波段数不足 3";
        return false;
    }

    double gt[6];
    src->GetGeoTransform(gt);
    const char* srcPath = src->GetDescription();

    std::ostringstream xml;
    xml << "<VRTDataset rasterXSize=\"" << w << "\" rasterYSize=\"" << h
        << "\">\n";
    static const char* interp[4] = {"Red", "Green", "Blue", "Alpha"};
    for (int b = 0; b < nb; ++b) {
        xml << "  <VRTRasterBand dataType=\"Byte\" band=\"" << (b + 1) << "\"";
        if (b < 3) {
            xml << " subClass=\"VRTDerivedRasterBand\"";
        }
        xml << ">\n";
        // ColorInterp 名表仅 4 项（RGB+Alpha）；>4 波段时用 Undefined，避免越界
        const char* ci = b < 4 ? interp[b] : "Undefined";
        xml << "    <ColorInterp>" << ci << "</ColorInterp>\n";
        if (b < 3) {
            xml << "    <PixelFunctionType>ip_gain_offset</PixelFunctionType>\n";
            xml << "    <PixelFunctionArguments gain=\"" << gain[b]
                << "\" offset=\"" << offset[b] << "\"/>\n";
        }
        xml << "    <SimpleSource>\n";
        xml << "      <SourceFilename relativeToVRT=\"0\">" << xmlEscape(srcPath)
            << "</SourceFilename>\n";
        xml << "      <SourceBand>" << (b + 1) << "</SourceBand>\n";
        xml << "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"" << w
            << "\" ySize=\"" << h << "\"/>\n";
        xml << "      <DstRect xOff=\"0\" yOff=\"0\" xSize=\"" << w
            << "\" ySize=\"" << h << "\"/>\n";
        xml << "    </SimpleSource>\n";
        xml << "  </VRTRasterBand>\n";
    }
    xml << "  <GeoTransform>" << gt[0] << ", " << gt[1] << ", " << gt[2] << ", "
        << gt[3] << ", " << gt[4] << ", " << gt[5] << "</GeoTransform>\n";
    if (const OGRSpatialReference* srs = src->GetSpatialRef()) {
        xml << srsXml(srs);
    }
    xml << "</VRTDataset>\n";

    std::ofstream out(vrtPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        err = "无法创建 VRT: " + vrtPath;
        return false;
    }
    out << xml.str();
    out.flush();
    if (!out.good()) {
        err = "写入 VRT 失败: " + vrtPath;
        return false;
    }
    return true;
}

bool writeMosaicVrt(const std::vector<VrtSource>& sources, int bands,
                    const std::string& vrtPath, std::string& err) {
    if (sources.empty()) {
        err = "无输入源";
        return false;
    }
    // 镶嵌范围与分辨率（取第一源）
    double minX = sources[0].minX, minY = sources[0].minY;
    double maxX = sources[0].maxX, maxY = sources[0].maxY;
    for (const auto& s : sources) {
        minX = std::min(minX, s.minX);
        minY = std::min(minY, s.minY);
        maxX = std::max(maxX, s.maxX);
        maxY = std::max(maxY, s.maxY);
    }
    // 源分辨率：用像素尺寸/地理尺寸推算（假设方正好方形像元）
    GDALDataset* first = GDALDataset::Open(sources[0].path.c_str(),
                                           GDAL_OF_RASTER | GDAL_OF_READONLY);
    if (!first) {
        err = "无法打开源: " + sources[0].path;
        return false;
    }
    double gt[6];
    first->GetGeoTransform(gt);
    const int srcW = first->GetRasterXSize();
    const int srcH = first->GetRasterYSize();
    const int nb = first->GetRasterCount();
    GDALClose(first);
    if (nb != bands) {
        err = "源波段数不一致";
        return false;
    }
    const double resX = std::abs(gt[1]);
    const double resY = std::abs(gt[5]);
    const int mW = static_cast<int>(std::lround((maxX - minX) / resX));
    const int mH = static_cast<int>(std::lround((maxY - minY) / resY));

    std::ostringstream xml;
    // 浮点 rect 精度（gdalbuildvrt 同款语义，避免累积取整错位）
    xml << std::setprecision(15);
    xml << "<VRTDataset rasterXSize=\"" << mW << "\" rasterYSize=\"" << mH
        << "\">\n";
    static const char* interp[4] = {"Red", "Green", "Blue", "Alpha"};
    for (int b = 0; b < bands; ++b) {
        xml << "  <VRTRasterBand dataType=\"Byte\" band=\"" << (b + 1)
            << "\" subClass=\"VRTSourcedRasterBand\">\n";
        const char* ci = b < 4 ? interp[b] : "Undefined";
        xml << "    <ColorInterp>" << ci << "</ColorInterp>\n";
        for (const auto& s : sources) {
            const int dstX = static_cast<int>(std::lround((s.minX - minX) / resX));
            const int dstY = static_cast<int>(std::lround((maxY - s.maxY) / resY));
            xml << "    <ComplexSource>\n";
            xml << "      <SourceFilename relativeToVRT=\"0\">"
                << xmlEscape(s.path) << "</SourceFilename>\n";
            xml << "      <SourceBand>" << (b + 1) << "</SourceBand>\n";
            xml << "      <SourceProperties RasterXSize=\"" << s.rasterX
                << "\" RasterYSize=\"" << s.rasterY
                << "\" DataType=\"Byte\" />\n";
            xml << "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"" << s.rasterX
                << "\" ySize=\"" << s.rasterY << "\"/>\n";
            xml << "      <DstRect xOff=\"" << dstX << "\" yOff=\"" << dstY
                << "\" xSize=\"" << s.rasterX << "\" ySize=\"" << s.rasterY
                << "\"/>\n";
            // 匀色线性变换内联（GDAL ComplexSource 原生支持，避免派生层）；
            // alpha 波段（b>=3）不做变换（gain 数组仅 3 波段）
            if (b < 3) {
                xml << "      <ScaleRatio>" << s.gain[b] << "</ScaleRatio>\n";
                xml << "      <ScaleOffset>" << s.offset[b] << "</ScaleOffset>\n";
            }
            xml << "      <UseMaskBand>true</UseMaskBand>\n";
            xml << "    </ComplexSource>\n";
        }
        xml << "  </VRTRasterBand>\n";
    }
    xml << "  <GeoTransform>" << minX << ", " << gt[1] << ", 0, " << maxY
        << ", 0, " << (-resY) << "</GeoTransform>\n";
    // SRS：从第一源继承（含轴序属性；镶嵌 VRT 必须携带投影，否则下游切片报"无投影"）
    {
        GDALDataset* d0 = GDALDataset::Open(sources[0].path.c_str(),
                                            GDAL_OF_RASTER | GDAL_OF_READONLY);
        if (d0) {
            if (const OGRSpatialReference* srs = d0->GetSpatialRef()) {
                xml << srsXml(srs);
            }
            GDALClose(d0);
        }
    }
    xml << "</VRTDataset>\n";

    std::ofstream out(vrtPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        err = "无法创建 VRT: " + vrtPath;
        return false;
    }
    out << xml.str();
    out.flush();
    if (!out.good()) {
        err = "写入 VRT 失败: " + vrtPath;
        return false;
    }
    return true;
}

// 自检：打开生成的匀色 VRT 读样本块，与"直接读源+手工应用参数"对比
bool verifyBalancedVrt(GDALDataset* src, const std::string& vrtPath,
                       const double gain[3], const double offset[3],
                       std::string& err) {
    ensurePixelFuncRegistered();
    auto v = ip::openReadOnly(vrtPath);
    if (!v) {
        err = "自检失败：无法打开 VRT: " + vrtPath;
        return false;
    }
    const int w = std::min(64, src->GetRasterXSize());
    const int h = std::min(64, src->GetRasterYSize());
    if (w <= 0 || h <= 0) return true;
    const int x0 = std::min(100, src->GetRasterXSize() - w);
    const int y0 = std::min(100, src->GetRasterYSize() - h);
    if (x0 < 0 || y0 < 0) return true;

    std::vector<uint8_t> a(static_cast<size_t>(3) * w * h);
    std::vector<uint8_t> b(static_cast<size_t>(3) * w * h);
    for (int band = 0; band < 3; ++band) {
        if (src->GetRasterBand(band + 1)->RasterIO(
                GF_Read, x0, y0, w, h,
                a.data() + static_cast<size_t>(band) * w * h, w, h, GDT_Byte,
                0, 0, nullptr) != CE_None) {
            err = "自检失败：读源样本块错误 band" + std::to_string(band) + " " +
                  CPLGetLastErrorMsg();
            return false;
        }
        if (v->GetRasterBand(band + 1)->RasterIO(
                GF_Read, x0, y0, w, h,
                b.data() + static_cast<size_t>(band) * w * h, w, h, GDT_Byte,
                0, 0, nullptr) != CE_None) {
            err = "自检失败：读 VRT 样本块错误 band" + std::to_string(band) +
                  " " + CPLGetLastErrorMsg();
            return false;
        }
    }
    for (int band = 0; band < 3; ++band) {
        for (int i = 0; i < w * h; ++i) {
            const double expect = std::min(
                std::max(a[static_cast<size_t>(band) * w * h + i] * gain[band] +
                             offset[band],
                         0.0),
                255.0);
            const int ei = static_cast<int>(std::lround(expect));
            const int got = b[static_cast<size_t>(band) * w * h + i];
            if (std::abs(got - ei) > 1) {  // 允许 ±1 舍入差
                std::fprintf(stderr,
                             "[verify] mismatch band=%d i=%ld expect=%d got=%d\n"
                             "[verify] src 3x3: %d %d %d / %d %d %d / %d %d %d\n"
                             "[verify] vrt 3x3: %d %d %d / %d %d %d / %d %d %d\n",
                             band, (long)i, ei, got,
                             a[band * w * h], a[band * w * h + 1], a[band * w * h + 2],
                             a[band * w * h + w], a[band * w * h + w + 1], a[band * w * h + w + 2],
                             a[band * w * h + 2 * w], a[band * w * h + 2 * w + 1], a[band * w * h + 2 * w + 2],
                             b[band * w * h], b[band * w * h + 1], b[band * w * h + 2],
                             b[band * w * h + w], b[band * w * h + w + 1], b[band * w * h + w + 2],
                             b[band * w * h + 2 * w], b[band * w * h + 2 * w + 1],
                             b[band * w * h + 2 * w + 2]);
                err = "自检失败：VRT 像素值不匹配（band " +
                      std::to_string(band) + " 期望 " + std::to_string(ei) +
                      " 实得 " + std::to_string(got) + "）: " + vrtPath;
                return false;
            }
        }
    }
    return true;
}

} // namespace ipcb
