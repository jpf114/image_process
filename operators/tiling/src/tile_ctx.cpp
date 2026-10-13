// tiling：WarpedVRT 构建、瓦片渲染与编码
#include "tile_ctx.h"

#include "ip/log.h"
#include "ip/mem_guard.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>

namespace fs = std::filesystem;

namespace iptl {

GDALResampleAlg parseResampleAlg(const std::string& name) {
    if (name == "near" || name == "nearest") return GRA_NearestNeighbour;
    if (name == "bilinear") return GRA_Bilinear;
    if (name == "cubic") return GRA_Cubic;
    if (name == "average") return GRA_Average;
    if (name == "lanczos") return GRA_Lanczos;
    return GRA_Bilinear;
}

bool buildWarpedVrt(GDALDataset* src, const TileMatrixSet& tms,
                    const std::string& resampling, const std::string& vrtPath,
                    double& resX, ip::GeoExtent& extentInTarget,
                    std::string& err) {
    // warp 内部（GenImgProj）用权威轴序（与 gdal CLI 行为一致，教训 1h）
    OGRSpatialReference dstSrs = ip::makeSrsFromEpsg(tms.epsg);
    dstSrs.SetAxisMappingStrategy(OAMS_AUTHORITY_COMPLIANT);
    char* dstWkt = nullptr;
    dstSrs.exportToWkt(&dstWkt);
    if (!dstWkt) {
        err = "导出目标 WKT 失败";
        return false;
    }

    const char* srcWkt = src->GetProjectionRef();
    if (!srcWkt || !srcWkt[0]) {
        err = "输入影像无投影信息，无法切片";
        CPLFree(dstWkt);
        return false;
    }

    // AutoCreateWarpedVRT：自动推算目标网格/变换/DstGeoTransform，规避手工
    // warp options 的 Validate/DstGeoTransform 系列陷阱（教训 1h）；支持指定
    // 重采样算法；同 CRS 恒等变换无重采样损失，跨 CRS 由瓦片层重采样兜底
    GDALDataset* warped = static_cast<GDALDataset*>(GDALAutoCreateWarpedVRT(
        src, srcWkt, dstWkt, parseResampleAlg(resampling), 0.25, nullptr));
    CPLFree(dstWkt);
    if (!warped) {
        err = "创建 WarpedVRT 失败";
        return false;
    }

    double gt[6];
    warped->GetGeoTransform(gt);
    resX = std::abs(gt[1]);
    extentInTarget = ip::datasetExtent(warped);

    // 落盘（多线程各自打开，线程安全）
    GDALDriver* vdrv = GetGDALDriverManager()->GetDriverByName("VRT");
    GDALDataset* file = vdrv->CreateCopy(vrtPath.c_str(), warped, FALSE,
                                         nullptr, nullptr, nullptr);
    GDALClose(warped);
    if (!file) {
        err = "写出 WarpedVRT 失败: " + vrtPath;
        return false;
    }
    GDALClose(file);
    return true;
}

GDALDataset* threadLocalWarpDs(const std::string& vrtPath) {
    // 每线程至多缓存一个 VRT：路径变更即重开（同一渲染任务内路径恒定，热路径
    // 仅一次字符串比较；多景顺序处理/测试也不会错拿旧句柄）
    thread_local std::string cachedPath;
    thread_local ip::DataSetPtr ds;
    if (cachedPath != vrtPath) {
        ds = ip::openReadOnly(vrtPath);
        cachedPath = vrtPath;
    }
    return ds.get();
}

namespace {

inline uint8_t clamp255(double v) {
    int i = static_cast<int>(std::lround(v));
    return static_cast<uint8_t>(std::min(std::max(i, 0), 255));
}

// 将窗口缓冲（winW×winH）重采样到 tile（S×S）：
// 缩小 → box-average（有效像素加权平均，无效不参与）；放大 → near。
// validMask 可空（全部有效）。
void resampleToTile(const uint8_t* win, int winW, int winH, int nBands,
                    const std::vector<uint8_t>& valid, int S,
                    std::vector<uint8_t>& tile, std::vector<uint8_t>& tValid) {
    tile.assign(static_cast<size_t>(nBands) * S * S, 0);
    tValid.assign(static_cast<size_t>(S) * S, 0);
    const double sx = static_cast<double>(winW) / S;
    const double sy = static_cast<double>(winH) / S;
    for (int j = 0; j < S; ++j) {
        const int y0 = static_cast<int>(std::floor(j * sy));
        const int y1 = std::min(static_cast<int>(std::ceil((j + 1) * sy)), winH);
        if (y0 >= y1) continue;
        for (int i = 0; i < S; ++i) {
            const int x0 = static_cast<int>(std::floor(i * sx));
            const int x1 = std::min(static_cast<int>(std::ceil((i + 1) * sx)), winW);
            if (x0 >= x1) continue;
            const size_t ti = static_cast<size_t>(j) * S + i;
            if (sx >= 1.0 || sy >= 1.0) {
                // 缩小：box-average（有效加权）
                uint64_t cnt = 0;
                double acc[4] = {0, 0, 0, 0};
                for (int y = y0; y < y1; ++y) {
                    const size_t row = static_cast<size_t>(y) * winW;
                    for (int x = x0; x < x1; ++x) {
                        const size_t si = row + x;
                        if (!valid.empty() && !valid[si]) continue;
                        ++cnt;
                        for (int b = 0; b < nBands; ++b) {
                            acc[b] += win[static_cast<size_t>(b) * winW * winH + si];
                        }
                    }
                }
                if (cnt == 0) continue;
                tValid[ti] = 1;
                const double inv = 1.0 / cnt;
                for (int b = 0; b < nBands; ++b) {
                    tile[static_cast<size_t>(b) * S * S + ti] =
                        clamp255(acc[b] * inv);
                }
            } else {
                // 放大：near
                const int sxx = std::min(static_cast<int>(i / sx), winW - 1);
                const int syy = std::min(static_cast<int>(j / sy), winH - 1);
                const size_t si = static_cast<size_t>(syy) * winW + sxx;
                if (!valid.empty() && !valid[si]) continue;
                tValid[ti] = 1;
                for (int b = 0; b < nBands; ++b) {
                    tile[static_cast<size_t>(b) * S * S + ti] =
                        win[static_cast<size_t>(b) * winW * winH + si];
                }
            }
        }
    }
}

// 编码写出：MEM → CreateCopy（png/webp 4 波段 RGBA；jpeg 3 波段）
bool encodeTile(const std::vector<uint8_t>& tile, int nBands, int S,
                const RenderCtx& ctx, const std::string& path,
                std::string& err) {
    GDALDriver* mem = GetGDALDriverManager()->GetDriverByName("MEM");
    GDALDriver* drv =
        GetGDALDriverManager()->GetDriverByName(ctx.format.c_str());
    if (!mem || !drv) {
        err = "驱动不可用: " + ctx.format;
        return false;
    }
    int outBands = ctx.format == "jpeg" ? 3 : nBands;
    if (ctx.format == "png" && nBands < 4) outBands = nBands;
    GDALDataset* m = mem->Create("", S, S, outBands, GDT_Byte, nullptr);
    if (!m) {
        err = "MEM 创建失败";
        return false;
    }
    for (int b = 0; b < outBands; ++b) {
        const uint8_t* src = tile.data() + static_cast<size_t>(b) * S * S;
        m->GetRasterBand(b + 1)->RasterIO(GF_Write, 0, 0, S, S,
                                          const_cast<uint8_t*>(src), S, S,
                                          GDT_Byte, 0, 0, nullptr);
    }
    if (outBands == 4) {
        m->GetRasterBand(4)->SetColorInterpretation(GCI_AlphaBand);
    }
    char** opts = nullptr;
    if (ctx.format == "jpeg") {
        opts = CSLSetNameValue(opts, "QUALITY", std::to_string(ctx.jpegQuality).c_str());
    } else if (ctx.format == "webp") {
        opts = CSLSetNameValue(opts, "QUALITY", std::to_string(ctx.webpQuality).c_str());
    } else if (ctx.format == "png") {
        opts = CSLSetNameValue(opts, "ZLEVEL", "6");
    }
    GDALDataset* out = drv->CreateCopy(path.c_str(), m, FALSE, opts, nullptr,
                                       nullptr);
    CSLDestroy(opts);
    GDALClose(m);
    if (!out) {
        err = "编码失败 " + ctx.format + ": " + path + "（" +
              CPLGetLastErrorMsg() + "）";
        return false;
    }
    GDALClose(out);
    return true;
}

} // namespace

int renderTile(GDALDataset* warpDs, const TileBbox& bbox, RenderCtx& ctx,
               double levelRes, int dirLevel, long long tx, long long tyXYZ,
               std::string& err) {
    const int S = ctx.tileSize;
    const double lr = levelRes;
    // 1. 瓦片与数据的地理交集 → 瓦片子区（像素）
    const double iMinX = std::max(bbox.minX, ctx.dataExtent.minX);
    const double iMinY = std::max(bbox.minY, ctx.dataExtent.minY);
    const double iMaxX = std::min(bbox.maxX, ctx.dataExtent.maxX);
    const double iMaxY = std::min(bbox.maxY, ctx.dataExtent.maxY);
    if (iMinX >= iMaxX || iMinY >= iMaxY) return 3;  // 数据外
    const int ox = static_cast<int>(std::lround((iMinX - bbox.minX) / lr));
    const int oy = static_cast<int>(std::lround((bbox.maxY - iMaxY) / lr));
    const int ow = static_cast<int>(std::lround((iMaxX - iMinX) / lr));
    const int oh = static_cast<int>(std::lround((iMaxY - iMinY) / lr));
    if (ow <= 0 || oh <= 0 || ox >= S || oy >= S) return 3;
    const int cW = std::min(ow, S - ox);
    const int cH = std::min(oh, S - oy);

    // 2. 交集窗口（warp 像素坐标）→ 按 GDAL 重采样直接读到瓦片子区尺寸
    int x0 = 0, y0 = 0, w = 0, h = 0;
    const ip::GeoExtent interGeo{iMinX, iMinY, iMaxX, iMaxY};
    if (!ip::geoBoundsToPixelWindow(warpDs, interGeo, x0, y0, w, h)) return 3;

    std::vector<uint8_t> tile(static_cast<size_t>(ctx.nBands) * S * S, 0);
    std::vector<uint8_t> buf(static_cast<size_t>(ctx.nBands) * cW * cH);
    CPLErr e = CE_None;
    for (int b = 0; b < ctx.nBands && e == CE_None; ++b) {
        GDALRasterIOExtraArg arg;
        INIT_RASTERIO_EXTRA_ARG(arg);
        arg.eResampleAlg = static_cast<GDALRIOResampleAlg>(ctx.alg);  // 数值对应
        e = warpDs->GetRasterBand(b + 1)->RasterIO(
            GF_Read, x0, y0, w, h, buf.data() + static_cast<size_t>(b) * cW * cH,
            cW, cH, GDT_Byte, 0, 0, &arg);
    }
    if (e != CE_None) {
        err = "瓦片窗口读取失败 z=" + std::to_string(dirLevel) + " x=" +
              std::to_string(tx) + " y=" + std::to_string(tyXYZ) + "（" +
              CPLGetLastErrorMsg() + "）";
        return -1;
    }

    // 3. 有效掩码（alpha>0）并放置到瓦片子区（1:1）
    const size_t cn = static_cast<size_t>(cW) * cH;
    std::vector<uint8_t> valid(cn, 1);
    if (ctx.nBands >= 4) {
        const uint8_t* a = buf.data() + static_cast<size_t>(3) * cn;
        for (size_t i = 0; i < cn; ++i) valid[i] = a[i] > 0 ? 1 : 0;
    }
    for (int b = 0; b < ctx.nBands; ++b) {
        uint8_t* dst = tile.data() + static_cast<size_t>(b) * S * S;
        const uint8_t* s = buf.data() + static_cast<size_t>(b) * cn;
        for (int j = 0; j < cH; ++j) {
            std::memcpy(dst + static_cast<size_t>(oy + j) * S + ox,
                        s + static_cast<size_t>(j) * cW, cW);
        }
    }
    std::vector<uint8_t> tValid(static_cast<size_t>(S) * S, 0);
    for (int j = 0; j < cH; ++j) {
        std::memcpy(tValid.data() + static_cast<size_t>(oy + j) * S + ox,
                    valid.data() + static_cast<size_t>(j) * cW, cW);
    }

    uint64_t cnt = 0;
    for (int i = 0; i < S * S; ++i) cnt += tValid[i];
    const double ratio = 100.0 * cnt / (static_cast<double>(S) * S);

    const std::string ext =
        ctx.format == "jpeg" ? ".jpg" : "." + ctx.format;
    const std::string dir = (fs::path(ctx.outputDir) / std::to_string(dirLevel) /
                             std::to_string(tx))
                                .string();
    const std::string path =
        (fs::path(dir) / (std::to_string(tyXYZ) + ext)).string();

    // 0. 断点续跑：已存在且不强制覆盖 → 跳过（返回 4，与"写出"区分计数）
    if (ctx.resume && !ctx.overwrite && fs::exists(fs::path(path))) {
        ctx.resumed++;
        return 4;
    }

    // 4. 无效占比判定
    if (cnt == 0) {
        if (ctx.skipEmpty) return 1;  // 全无效且跳过
        // 全无效但需落盘：transparent → 全透明 png/webp；fill → 全填色
        if (!ctx.fillMode && ctx.format != "jpeg") {
            std::fill(tile.begin(), tile.end(), 0);
            std::fill(tValid.begin(), tValid.end(), 0);
        }
    } else if (ratio <= ctx.threshold && ctx.threshold < 100) {
        return 2;  // 超阈值：整瓦片不落盘（阈值语义与 Phase 0 E 一致）
    }

    // 5. fill 模式 / jpeg：无效像素填色，去掉 alpha
    if (ctx.fillMode || ctx.format == "jpeg") {
        if (ctx.format == "jpeg" && !ctx.fillMode && !ctx.jpegWarned) {
            ip::logWarn("JPEG 无透明通道：无效像素以 fill_value 填充"
                        "（transparent 仅对 png/webp 有效）");
            ctx.jpegWarned = true;
        }
        for (int j = 0; j < S; ++j) {
            for (int i = 0; i < S; ++i) {
                const size_t ti = static_cast<size_t>(j) * S + i;
                if (tValid[ti]) continue;
                for (int b = 0; b < 3; ++b) {
                    tile[static_cast<size_t>(b) * S * S + ti] =
                        static_cast<uint8_t>(ctx.fillValue);
                }
            }
        }
    }

    // 6. 编码写出
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!encodeTile(tile, ctx.nBands, S, ctx, path, err)) return -1;
    return 0;
}

} // namespace iptl
