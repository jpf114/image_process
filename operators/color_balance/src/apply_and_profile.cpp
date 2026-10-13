// color_balance：块式应用（gain/offset、Wallis、LUT 共用骨架）+ 方案 JSON 构建
#include "cb_internal.h"

#include "ip/json_io.h"
#include "ip/log.h"

#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

namespace ipcb {

namespace {

// 公共输出骨架：创建 GeoTIFF（DEFLATE+tiled 512）→ 块式读 → 有效像素掩码 →
// transform(buf, n, y0, hh, valid) 原地修改 RGB → 写出。
// 无效像素（alpha=0 或 RGB nodata）保持原样。
bool writeTransformedTiff(
    GDALDataset* src, const std::string& outPath, int chunkRows,
    const std::function<void(uint8_t* buf, size_t n, int y0, int hh,
                             const std::vector<uint8_t>& valid)>& transform,
    std::string& err) {
    const int w = src->GetRasterXSize();
    const int h = src->GetRasterYSize();
    const int nb = src->GetRasterCount();
    if (nb < 3) {
        err = "波段数不足 3";
        return false;
    }

    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
    if (!drv) {
        err = "GTiff 驱动不可用";
        return false;
    }
    char** opts = CSLSetNameValue(nullptr, "COMPRESS", "DEFLATE");
    opts = CSLSetNameValue(opts, "PREDICTOR", "2");
    opts = CSLSetNameValue(opts, "TILED", "YES");
    opts = CSLSetNameValue(opts, "BLOCKXSIZE", "512");
    opts = CSLSetNameValue(opts, "BLOCKYSIZE", "512");
    GDALDataset* out = drv->Create(outPath.c_str(), w, h, nb, GDT_Byte, opts);
    CSLDestroy(opts);
    if (!out) {
        err = "创建输出失败: " + outPath;
        return false;
    }

    double gt[6];
    if (src->GetGeoTransform(gt) == CE_None) out->SetGeoTransform(gt);
    if (const OGRSpatialReference* srs = src->GetSpatialRef()) {
        char* wkt = nullptr;
        OGRSpatialReference tmp(*srs);
        tmp.exportToWkt(&wkt);
        if (wkt) {
            out->SetProjection(wkt);
            CPLFree(wkt);
        }
    }

    const bool hasAlpha = nb >= 4;
    std::vector<int> hasNd(3, 0);
    std::vector<double> nd(3, 0.0);
    for (int b = 0; b < 3; ++b) {
        int hv = 0;
        nd[b] = src->GetRasterBand(b + 1)->GetNoDataValue(&hv);
        hasNd[b] = hv;
    }

    const int chunk = std::max(64, chunkRows);
    std::vector<uint8_t> buf(static_cast<size_t>(nb) * w * chunk);
    std::vector<uint8_t> valid;
    for (int y = 0; y < h; y += chunk) {
        const int hh = std::min(chunk, h - y);
        const size_t n = static_cast<size_t>(w) * hh;
        CPLErr e = CE_None;
        for (int b = 0; b < nb && e == CE_None; ++b) {
            e = src->GetRasterBand(b + 1)->RasterIO(
                GF_Read, 0, y, w, hh, buf.data() + static_cast<size_t>(b) * n,
                w, hh, GDT_Byte, 0, 0, nullptr);
        }
        if (e != CE_None) {
            GDALClose(out);
            err = "读取失败 @y=" + std::to_string(y);
            return false;
        }

        const uint8_t* alpha =
            hasAlpha ? buf.data() + static_cast<size_t>(3) * n : nullptr;
        valid.assign(n, 0);
        for (size_t i = 0; i < n; ++i) {
            bool ok = alpha ? alpha[i] > 0 : true;
            for (int b = 0; b < 3 && ok; ++b) {
                if (hasNd[b] &&
                    static_cast<double>(buf[static_cast<size_t>(b) * n + i]) == nd[b]) {
                    ok = false;
                }
            }
            valid[i] = ok ? 1 : 0;
        }

        transform(buf.data(), n, y, hh, valid);

        for (int b = 0; b < nb && e == CE_None; ++b) {
            e = out->GetRasterBand(b + 1)->RasterIO(
                GF_Write, 0, y, w, hh, buf.data() + static_cast<size_t>(b) * n,
                w, hh, GDT_Byte, 0, 0, nullptr);
        }
        if (e != CE_None) {
            GDALClose(out);
            err = "写入失败 @y=" + std::to_string(y);
            return false;
        }
    }

    for (int b = 0; b < nb; ++b) {
        GDALRasterBand* ob = out->GetRasterBand(b + 1);
        if (b < 3 && hasNd[b]) ob->SetNoDataValue(nd[b]);
        if (hasAlpha && b == 3) ob->SetColorInterpretation(GCI_AlphaBand);
    }
    GDALClose(out);
    return true;
}

} // namespace

bool applyGainOffsetToFile(GDALDataset* src, const std::string& outPath,
                           const double gain[3], const double offset[3],
                           int chunkRows, std::string& err) {
    return writeTransformedTiff(
        src, outPath, chunkRows,
        [&](uint8_t* buf, size_t n, int, int, const std::vector<uint8_t>& valid) {
            for (int b = 0; b < 3; ++b) {
                uint8_t* p = buf + static_cast<size_t>(b) * n;
                for (size_t i = 0; i < n; ++i) {
                    if (!valid[i]) continue;
                    const double v = p[i] * gain[b] + offset[b];
                    int vi = static_cast<int>(std::lround(v));
                    p[i] = static_cast<uint8_t>(std::min(std::max(vi, 0), 255));
                }
            }
        },
        err);
}

// ============ Wallis ============

WallisGrid computeWallisGrid(GDALDataset* ds, const double targetMean[3],
                             const double targetStd[3], int block,
                             const double sceneGain[3],
                             const double sceneOffset[3]) {
    const int w = ds->GetRasterXSize();
    const int h = ds->GetRasterYSize();
    const int nb = ds->GetRasterCount();
    const bool hasAlpha = nb >= 4;
    WallisGrid wg;
    wg.block = std::max(64, block);
    wg.nbx = (w + wg.block - 1) / wg.block;
    wg.nby = (h + wg.block - 1) / wg.block;
    wg.a.assign(static_cast<size_t>(3) * wg.nby * wg.nbx, 1.0f);
    wg.b.assign(static_cast<size_t>(3) * wg.nby * wg.nbx, 0.0f);
    if (nb < 3) return wg;

    std::vector<char> hasData(static_cast<size_t>(wg.nby) * wg.nbx, 0);
    for (int iy = 0; iy < wg.nby; ++iy) {
        for (int ix = 0; ix < wg.nbx; ++ix) {
            const int x0 = ix * wg.block;
            const int y0 = iy * wg.block;
            const int ww = std::min(wg.block, w - x0);
            const int hh = std::min(wg.block, h - y0);
            const size_t n = static_cast<size_t>(ww) * hh;
            std::vector<uint8_t> blk(static_cast<size_t>(nb) * n);
            CPLErr e = CE_None;
            for (int b = 0; b < nb && e == CE_None; ++b) {
                e = ds->GetRasterBand(b + 1)->RasterIO(
                    GF_Read, x0, y0, ww, hh,
                    blk.data() + static_cast<size_t>(b) * n, ww, hh, GDT_Byte,
                    0, 0, nullptr);
            }
            if (e != CE_None) continue;
            const uint8_t* alpha =
                hasAlpha ? blk.data() + static_cast<size_t>(3) * n : nullptr;
            uint64_t cnt = 0;
            double sum[3] = {0, 0, 0}, sumsq[3] = {0, 0, 0};
            for (size_t i = 0; i < n; ++i) {
                if (alpha && alpha[i] == 0) continue;
                ++cnt;
                for (int b = 0; b < 3; ++b) {
                    const double v = blk[static_cast<size_t>(b) * n + i];
                    sum[b] += v;
                    sumsq[b] += v * v;
                }
            }
            if (cnt < 0.2 * static_cast<double>(n)) continue;  // 视为无效块
            hasData[static_cast<size_t>(iy) * wg.nbx + ix] = 1;
            const double inv = 1.0 / static_cast<double>(cnt);
            for (int b = 0; b < 3; ++b) {
                const double muRaw = sum[b] * inv;
                const double sdRaw =
                    std::sqrt(std::max(sumsq[b] * inv - muRaw * muRaw, 0.0));
                // 先行景级变换（组合模式；单独 wallis 时 g=1,o=0）
                const double muAdj = sceneGain[b] * muRaw + sceneOffset[b];
                const double sdAdj =
                    std::max(sceneGain[b] * sdRaw, 1e-3);
                const double a =
                    std::min(std::max(targetStd[b] / sdAdj, 0.4), 2.5);
                const size_t idx =
                    (static_cast<size_t>(b) * wg.nby + iy) * wg.nbx + ix;
                wg.a[idx] = static_cast<float>(a);
                wg.b[idx] = static_cast<float>(targetMean[b] - a * muAdj);
            }
        }
    }

    // 无效块填充：列均值 → 整列无效则兜底 1/0（与 Phase 0 语义一致）
    for (int b = 0; b < 3; ++b) {
        const size_t base = static_cast<size_t>(b) * wg.nby * wg.nbx;
        for (int ix = 0; ix < wg.nbx; ++ix) {
            double sa = 0, sb = 0;
            int cnt = 0;
            for (int iy = 0; iy < wg.nby; ++iy) {
                if (hasData[static_cast<size_t>(iy) * wg.nbx + ix]) {
                    const size_t idx = base + static_cast<size_t>(iy) * wg.nbx + ix;
                    sa += wg.a[idx];
                    sb += wg.b[idx];
                    ++cnt;
                }
            }
            if (cnt == 0) continue;  // 整列无效，下面兜底
            for (int iy = 0; iy < wg.nby; ++iy) {
                if (!hasData[static_cast<size_t>(iy) * wg.nbx + ix]) {
                    const size_t idx = base + static_cast<size_t>(iy) * wg.nbx + ix;
                    wg.a[idx] = static_cast<float>(sa / cnt);
                    wg.b[idx] = static_cast<float>(sb / cnt);
                }
            }
        }
        // 整列无效的兜底：置恒等
        for (int ix = 0; ix < wg.nbx; ++ix) {
            bool colHas = false;
            for (int iy = 0; iy < wg.nby; ++iy) {
                if (hasData[static_cast<size_t>(iy) * wg.nbx + ix]) {
                    colHas = true;
                    break;
                }
            }
            if (colHas) continue;
            for (int iy = 0; iy < wg.nby; ++iy) {
                const size_t idx = base + static_cast<size_t>(iy) * wg.nbx + ix;
                wg.a[idx] = 1.0f;
                wg.b[idx] = 0.0f;
            }
        }
    }
    return wg;
}

namespace {

// 参数场双线性采样：参数值位于块中心 (i+0.5)*block；边界 clamp
inline float sampleGrid(const std::vector<float>& g, int nbx, int nby, int band,
                        double fx, double fy) {
    fx = std::min(std::max(fx, 0.0), static_cast<double>(nbx - 1));
    fy = std::min(std::max(fy, 0.0), static_cast<double>(nby - 1));
    const int x0 = static_cast<int>(fx);
    const int y0 = static_cast<int>(fy);
    const int x1 = std::min(x0 + 1, nbx - 1);
    const int y1 = std::min(y0 + 1, nby - 1);
    const double tx = fx - x0;
    const double ty = fy - y0;
    const size_t base = static_cast<size_t>(band) * nby * nbx;
    const double v00 = g[base + static_cast<size_t>(y0) * nbx + x0];
    const double v10 = g[base + static_cast<size_t>(y0) * nbx + x1];
    const double v01 = g[base + static_cast<size_t>(y1) * nbx + x0];
    const double v11 = g[base + static_cast<size_t>(y1) * nbx + x1];
    return static_cast<float>((v00 * (1 - tx) + v10 * tx) * (1 - ty) +
                              (v01 * (1 - tx) + v11 * tx) * ty);
}

} // namespace

bool applyWallisToFile(GDALDataset* src, const std::string& outPath,
                       const WallisGrid& wg, int chunkRows, std::string& err) {
    return writeTransformedTiff(
        src, outPath, chunkRows,
        [&](uint8_t* buf, size_t n, int y0, int hh,
            const std::vector<uint8_t>& valid) {
            const int w = src->GetRasterXSize();
            for (int r = 0; r < hh; ++r) {
                const double fy =
                    (y0 + r + 0.5) / wg.block - 0.5;  // 块中心语义
                for (int c = 0; c < w; ++c) {
                    const size_t i = static_cast<size_t>(r) * w + c;
                    if (!valid[i]) continue;
                    const double fx = (c + 0.5) / wg.block - 0.5;
                    for (int b = 0; b < 3; ++b) {
                        const float a =
                            sampleGrid(wg.a, wg.nbx, wg.nby, b, fx, fy);
                        const float bval =
                            sampleGrid(wg.b, wg.nbx, wg.nby, b, fx, fy);
                        const double v =
                            buf[static_cast<size_t>(b) * n + i] * a + bval;
                        int vi = static_cast<int>(std::lround(v));
                        buf[static_cast<size_t>(b) * n + i] =
                            static_cast<uint8_t>(std::min(std::max(vi, 0), 255));
                    }
                }
            }
        },
        err);
}

// ============ 直方图匹配 ============

void exactHistograms(GDALDataset* ds, uint64_t hist[3][256]) {
    std::memset(hist, 0, sizeof(uint64_t) * 3 * 256);
    const int w = ds->GetRasterXSize();
    const int h = ds->GetRasterYSize();
    const int nb = ds->GetRasterCount();
    const bool hasAlpha = nb >= 4;
    const int chunk = 2048;
    std::vector<uint8_t> buf(static_cast<size_t>(nb) * w * chunk);
    for (int y = 0; y < h; y += chunk) {
        const int hh = std::min(chunk, h - y);
        const size_t n = static_cast<size_t>(w) * hh;
        CPLErr e = CE_None;
        for (int b = 0; b < nb && e == CE_None; ++b) {
            e = ds->GetRasterBand(b + 1)->RasterIO(
                GF_Read, 0, y, w, hh, buf.data() + static_cast<size_t>(b) * n,
                w, hh, GDT_Byte, 0, 0, nullptr);
        }
        if (e != CE_None) return;
        const uint8_t* alpha =
            hasAlpha ? buf.data() + static_cast<size_t>(3) * n : nullptr;
        for (size_t i = 0; i < n; ++i) {
            if (alpha && alpha[i] == 0) continue;
            for (int b = 0; b < 3; ++b) {
                ++hist[b][buf[static_cast<size_t>(b) * n + i]];
            }
        }
    }
}

void cdfMatchLut(const uint64_t srcHist[256], const uint64_t refHist[256],
                 uint8_t lut[256]) {
    double cdfS[256], cdfR[256];
    uint64_t s = 0, r = 0;
    for (int i = 0; i < 256; ++i) s += srcHist[i];
    for (int i = 0; i < 256; ++i) r += refHist[i];
    if (s == 0 || r == 0) {
        for (int i = 0; i < 256; ++i) lut[i] = static_cast<uint8_t>(i);
        return;
    }
    double cs = 0, cr = 0;
    for (int i = 0; i < 256; ++i) {
        cs += static_cast<double>(srcHist[i]) / s;
        cr += static_cast<double>(refHist[i]) / r;
        cdfS[i] = cs;
        cdfR[i] = cr;
    }
    int j = 0;
    for (int i = 0; i < 256; ++i) {
        while (j < 255 && cdfR[j] < cdfS[i]) ++j;
        lut[i] = static_cast<uint8_t>(j);
    }
}

bool applyLutToFile(GDALDataset* src, const std::string& outPath,
                    const uint8_t lut[3][256], int chunkRows, std::string& err) {
    return writeTransformedTiff(
        src, outPath, chunkRows,
        [&](uint8_t* buf, size_t n, int, int,
            const std::vector<uint8_t>& valid) {
            for (int b = 0; b < 3; ++b) {
                uint8_t* p = buf + static_cast<size_t>(b) * n;
                for (size_t i = 0; i < n; ++i) {
                    if (!valid[i]) continue;
                    p[i] = lut[b][p[i]];
                }
            }
        },
        err);
}

// ============ 方案 JSON ============

ip::Json buildProfileJson(const std::string& method, const std::string& refId,
                          const std::string& refPath,
                          const ip::BandStats3& refStats,
                          const std::vector<SceneSnapshot>& scenes,
                          const std::vector<EdgeSnapshot>& edges,
                          double seamThreshold) {
    ip::Json target;
    target["type"] = "reference_image";
    target["ref_image"] = refPath;
    target["mean"] = std::vector<double>(refStats.mean, refStats.mean + 3);
    target["std"] = std::vector<double>(refStats.std, refStats.std + 3);

    ip::Json jimgs = ip::Json::array();
    for (const auto& s : scenes) {
        ip::Json ji;
        ji["id"] = s.id;
        ji["source"] = s.path;
        ji["stats"] = {{"mean", std::vector<double>(s.mean, s.mean + 3)},
                       {"std", std::vector<double>(s.std, s.std + 3)}};
        ji["transform"] = {{"gain", std::vector<double>(s.gain, s.gain + 3)},
                           {"offset", std::vector<double>(s.offset, s.offset + 3)}};
        ji["adjusted"] = s.adjusted;
        ji["note"] = s.note;
        ji["balanced_output"] = s.outputPath;
        jimgs.push_back(ji);
    }

    ip::Json jedges = ip::Json::array();
    for (const auto& e : edges) {
        ip::Json je;
        je["a"] = e.idA;
        je["b"] = e.idB;
        je["delta_mu"] = std::vector<double>(e.deltaMu, e.deltaMu + 3);
        je["delta_mu_max"] = e.deltaMuMax;
        je["skipped_by_seam_check"] = e.skipped;
        je["used_for_solving"] = e.usedForSolve;
        je["residual_after"] = std::vector<double>(e.residualAfter, e.residualAfter + 3);
        jedges.push_back(je);
    }

    ip::Json j;
    j["profile_version"] = "1.0";
    j["method"] = method;
    j["seam_check_threshold"] = seamThreshold;
    j["reference"] = refId;
    j["target"] = target;
    j["images"] = jimgs;
    j["edges"] = jedges;
    return j;
}

} // namespace ipcb
