#include "ip/block_stats.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace ip {

namespace {

// 读一个窗口（可降采样），返回 rowsPerChunk 行块循环回调由调用方控制。
// 这里直接一次性读取（窗口已经过 maxSide 限制）。
bool readWindowRGB(GDALDataset* ds, int x0, int y0, int w, int h,
                   std::vector<uint8_t>& rgb, std::vector<uint8_t>& valid) {
    const int nb = ds->GetRasterXSize() > 0 ? ds->GetRasterCount() : 0;
    if (nb < 3) return false;
    const bool hasAlpha = nb >= 4;

    std::vector<uint8_t> buf(static_cast<size_t>(nb) * w * h);
    CPLErr err = CE_None;
    for (int b = 0; b < nb && err == CE_None; ++b) {
        err = ds->GetRasterBand(b + 1)->RasterIO(
            GF_Read, x0, y0, w, h, buf.data() + static_cast<size_t>(b) * w * h,
            w, h, GDT_Byte, 0, 0, nullptr);
    }
    if (err != CE_None) return false;

    rgb.resize(static_cast<size_t>(3) * w * h);
    valid.assign(static_cast<size_t>(w) * h, 0);
    const uint8_t* a = hasAlpha ? buf.data() + static_cast<size_t>(3) * w * h : nullptr;
    double nd[3] = {0, 0, 0};
    bool hasNd[3] = {false, false, false};
    for (int b = 0; b < 3; ++b) {
        int hasVal = 0;
        nd[b] = ds->GetRasterBand(b + 1)->GetNoDataValue(&hasVal);
        hasNd[b] = hasVal != 0;
    }
    const size_t n = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < n; ++i) {
        bool ok = true;
        if (a) ok = a[i] > 0;
        if (ok) {
            for (int b = 0; b < 3 && ok; ++b) {
                if (hasNd[b] &&
                    static_cast<double>(buf[static_cast<size_t>(b) * n + i]) == nd[b]) {
                    ok = false;
                }
            }
        }
        if (ok) {
            valid[i] = 1;
            for (int b = 0; b < 3; ++b) {
                rgb[static_cast<size_t>(b) * n + i] = buf[static_cast<size_t>(b) * n + i];
            }
        }
    }
    return true;
}

BandStats3 statsFromBuffer(const std::vector<uint8_t>& rgb,
                           const std::vector<uint8_t>& valid, int w, int h) {
    BandStats3 st;
    uint64_t hist[3][256];
    std::memset(hist, 0, sizeof(hist));
    const size_t n = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < n; ++i) {
        if (!valid[i]) continue;
        for (int b = 0; b < 3; ++b) ++hist[b][rgb[static_cast<size_t>(b) * n + i]];
    }
    for (int b = 0; b < 3; ++b) {
        uint64_t cnt = 0;
        double sum = 0;
        for (int v = 0; v < 256; ++v) {
            cnt += hist[b][v];
            sum += static_cast<double>(hist[b][v]) * v;
        }
        st.validCount[b] = cnt;
        if (cnt == 0) continue;
        const double mean = sum / cnt;
        double var = 0;
        for (int v = 0; v < 256; ++v) {
            const double d = v - mean;
            var += static_cast<double>(hist[b][v]) * d * d;
        }
        st.mean[b] = mean;
        st.std[b] = std::sqrt(var / cnt);
    }
    return st;
}

} // namespace

BandStats3 exactStatsRGB(GDALDataset* ds) {
    BandStats3 st;
    const int w = ds->GetRasterXSize();
    const int h = ds->GetRasterYSize();
    const int nb = ds->GetRasterCount();
    if (nb < 3) return st;
    const bool hasAlpha = nb >= 4;
    const int chunk = 2048;

    uint64_t hist[3][256];
    std::memset(hist, 0, sizeof(hist));

    std::vector<uint8_t> buf(static_cast<size_t>(nb) * w * chunk);
    std::vector<uint8_t> mask(static_cast<size_t>(w) * chunk);
    double nd[3] = {0, 0, 0};
    bool hasNd[3] = {false, false, false};
    for (int b = 0; b < 3; ++b) {
        int hasVal = 0;
        nd[b] = ds->GetRasterBand(b + 1)->GetNoDataValue(&hasVal);
        hasNd[b] = hasVal != 0;
    }

    for (int y = 0; y < h; y += chunk) {
        const int hh = std::min(chunk, h - y);
        const size_t n = static_cast<size_t>(w) * hh;
        CPLErr err = CE_None;
        for (int b = 0; b < nb && err == CE_None; ++b) {
            err = ds->GetRasterBand(b + 1)->RasterIO(
                GF_Read, 0, y, w, hh, buf.data() + static_cast<size_t>(b) * n,
                w, hh, GDT_Byte, 0, 0, nullptr);
        }
        if (err != CE_None) return st;

        const uint8_t* a =
            hasAlpha ? buf.data() + static_cast<size_t>(3) * n : nullptr;
        for (size_t i = 0; i < n; ++i) {
            bool ok = a ? a[i] > 0 : true;
            if (ok) {
                for (int b = 0; b < 3 && ok; ++b) {
                    if (hasNd[b] &&
                        static_cast<double>(buf[static_cast<size_t>(b) * n + i]) == nd[b]) {
                        ok = false;
                    }
                }
            }
            if (ok) {
                for (int b = 0; b < 3; ++b) {
                    ++hist[b][buf[static_cast<size_t>(b) * n + i]];
                }
            }
        }
    }
    for (int b = 0; b < 3; ++b) {
        uint64_t cnt = 0;
        double sum = 0;
        for (int v = 0; v < 256; ++v) {
            cnt += hist[b][v];
            sum += static_cast<double>(hist[b][v]) * v;
        }
        st.validCount[b] = cnt;
        if (cnt == 0) continue;
        const double mean = sum / cnt;
        double var = 0;
        for (int v = 0; v < 256; ++v) {
            const double d = v - mean;
            var += static_cast<double>(hist[b][v]) * d * d;
        }
        st.mean[b] = mean;
        st.std[b] = std::sqrt(var / cnt);
    }
    return st;
}

BandStats3 windowStatsRGB(GDALDataset* ds, int x0, int y0, int w, int h,
                          int maxSide) {
    BandStats3 st;
    if (w <= 0 || h <= 0) return st;
    const int mx = std::max(1, maxSide);

    // 小窗口：全读（原始分辨率）
    if (w <= mx && h <= mx) {
        std::vector<uint8_t> rgb, valid;
        if (!readWindowRGB(ds, x0, y0, w, h, rgb, valid)) return st;
        return statsFromBuffer(rgb, valid, w, h);
    }

    // 大窗口：均匀采样 256×256 tile（原始分辨率，无重采样）
    const int tile = 256;
    const uint64_t target = static_cast<uint64_t>(mx) * mx;
    const uint64_t nTiles = std::max<uint64_t>(1, target / (tile * tile));
    const double aspect = static_cast<double>(w) / h;
    int tx = static_cast<int>(std::lround(std::sqrt(static_cast<double>(nTiles) * aspect)));
    int ty = static_cast<int>(std::lround(std::sqrt(static_cast<double>(nTiles) / aspect)));
    tx = std::max(1, std::min(tx, w / tile + 1));
    ty = std::max(1, std::min(ty, h / tile + 1));

    uint64_t hist[3][256];
    std::memset(hist, 0, sizeof(hist));

    for (int iy = 0; iy < ty; ++iy) {
        for (int ix = 0; ix < tx; ++ix) {
            // tile 中心均匀分布，再对齐到窗口内
            int cx = static_cast<int>((ix + 0.5) * w / tx);
            int cy = static_cast<int>((iy + 0.5) * h / ty);
            int sx = std::min(std::max(cx - tile / 2, 0), std::max(w - tile, 0));
            int sy = std::min(std::max(cy - tile / 2, 0), std::max(h - tile, 0));
            const int sw = std::min(tile, w - sx);
            const int sh = std::min(tile, h - sy);
            if (sw <= 0 || sh <= 0) continue;

            std::vector<uint8_t> rgb, valid;
            if (!readWindowRGB(ds, x0 + sx, y0 + sy, sw, sh, rgb, valid)) continue;
            const size_t n = static_cast<size_t>(sw) * sh;
            for (size_t i = 0; i < n; ++i) {
                if (!valid[i]) continue;
                for (int b = 0; b < 3; ++b) ++hist[b][rgb[static_cast<size_t>(b) * n + i]];
            }
        }
    }

    for (int b = 0; b < 3; ++b) {
        uint64_t cnt = 0;
        double sum = 0;
        for (int v = 0; v < 256; ++v) {
            cnt += hist[b][v];
            sum += static_cast<double>(hist[b][v]) * v;
        }
        st.validCount[b] = cnt;
        if (cnt == 0) continue;
        const double mean = sum / cnt;
        double var = 0;
        for (int v = 0; v < 256; ++v) {
            const double d = v - mean;
            var += static_cast<double>(hist[b][v]) * d * d;
        }
        st.mean[b] = mean;
        st.std[b] = std::sqrt(var / cnt);
    }
    return st;
}

} // namespace ip
