// tiling operator 主实现：zoom 推算 → WarpedVRT → 瓦片枚举 → 线程池并行渲染
#include "tile_ctx.h"

#include "ip/log.h"
#include "ip/mem_guard.h"
#include "ip/thread_pool.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <mutex>

namespace fs = std::filesystem;

namespace iptl {

namespace {

std::string fmtL(long long v) { return std::to_string(v); }

class TilingImpl : public ITiling {
public:
    bool configure(const TilingParams& p, std::string& err) override {
        params_ = p;
        if (params_.input.empty()) {
            err = "缺少输入（--input）";
            return false;
        }
        if (params_.output_dir.empty()) {
            err = "缺少输出目录（--output-dir）";
            return false;
        }
        if (params_.format != "png" && params_.format != "jpeg" &&
            params_.format != "webp") {
            err = "format 仅支持 png / jpeg / webp";
            return false;
        }
        if (params_.nodata_mode != "transparent" &&
            params_.nodata_mode != "fill_color") {
            err = "nodata_mode 仅支持 transparent / fill_color（sample_fill 后期实现）";
            return false;
        }
        if (params_.convention != "xyz" && params_.convention != "tms") {
            err = "convention 仅支持 xyz / tms";
            return false;
        }
        if (params_.tile_size < 64 || params_.tile_size > 1024 ||
            (params_.tile_size & (params_.tile_size - 1)) != 0) {
            err = "tile_size 须为 64~1024 的 2 的幂";
            return false;
        }
        if (params_.nodata_pct_threshold < 0 ||
            params_.nodata_pct_threshold > 100) {
            err = "nodata_pct_threshold 取值 0~100";
            return false;
        }
        if (params_.tiles_scope != "full" && params_.tiles_scope != "footprint") {
            err = "tiles_scope 仅支持 full / footprint";
            return false;
        }
        if (params_.tiles_scope == "footprint" && params_.bbox.empty()) {
            err = "tiles_scope=footprint 需同时给出增量范围 bbox";
            return false;
        }
        if (params_.build_overviews != "auto" && params_.build_overviews != "yes" &&
            params_.build_overviews != "no") {
            err = "overview_policy/build_overviews 仅支持 auto / yes / no";
            return false;
        }
        return true;
    }

    bool execute(const ProgressCallback& cb, TilingStats& stats,
                 std::string& err) override {
        try {
            return run(cb, stats, err);
        } catch (const std::exception& e) {
            err = std::string("执行异常: ") + e.what();
            return false;
        }
    }

private:
    void report(const ProgressCallback& cb, int pct, const std::string& msg) {
        ip::logInfo("[" + std::to_string(pct) + "%] " + msg);
        if (cb) cb(pct, msg);
    }

    bool run(const ProgressCallback& cb, TilingStats& stats, std::string& err) {
        const auto t0 = std::chrono::steady_clock::now();
        // 运行日志落盘（每次处理一个文件，避免持续开发无日志）
        {
            char ts[32] = {0};
            std::time_t t = std::time(nullptr);
            std::tm tmbuf{};
            localtime_r(&t, &tmbuf);
            std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tmbuf);
            const std::string logPath =
                (fs::path(params_.output_dir) / "logs" /
                 (std::string("run_tiling_") + ts + ".log"))
                    .string();
            ip::logSetFile(logPath);
            ip::logInfo("==== tiling 开始 ====，日志: " + logPath);
        }
        // 块缓存与 GPU 占位
        GDALSetCacheMax(static_cast<int>(params_.cache_mb * 1024));  // KB
        if (params_.use_gpu) {
#if defined(IP_ENABLE_GPU)
            ip::logWarn("use_gpu=true 且已启用 IP_ENABLE_GPU，但 GPU 路径尚未实现，按 CPU 执行");
#else
            ip::logWarn("use_gpu=true 但未以 IP_ENABLE_GPU 编译（本期纯 CPU），按 CPU 执行");
#endif
        }
        if (params_.num_processes > 1) {
            ip::logWarn("num_processes>1 的多进程并行随 Phase 4 服务化提供，V1 按单进程执行");
        }
        ip::MemGuard::checkOrThrow(params_.min_mem_gb, "开始");

        // 1. TMS
        if (!resolveTms(params_.tiling_scheme, tms_, err)) return false;
        ip::logInfo("TMS: " + tms_.id + " EPSG:" + std::to_string(tms_.epsg) +
                    " 层级数 " + std::to_string(tms_.levelCount()) +
                    "（目录层级 " + fmtL(tms_.dirLevel(0)) + " 起）");

        // 2. 打开输入
        ip::DataSetPtr src = ip::openReadOnly(params_.input);
        if (!src) {
            err = "无法打开输入: " + params_.input + "（" +
                  CPLGetLastErrorMsg() + "）";
            return false;
        }
        if (src->GetRasterCount() < 3) {
            err = "影像少于 3 波段: " + params_.input;
            return false;
        }

        // 2.5 overview（低 zoom 切片读取性能关键，Phase 0 bench/方案 4.3）
        if (params_.build_overviews != "no") {
            const int nOvr = src->GetRasterBand(1)->GetOverviewCount();
            if (nOvr <= 0) {
                if (params_.build_overviews == "yes" ||
                    params_.build_overviews == "auto") {
                    report(cb, 3, "构建 overview（AVERAGE 2~64，一次性成本）");
                    std::vector<int> lv;
                    for (int f = 2; f <= 64; f *= 2) lv.push_back(f);
                    const CPLErr oe = src->BuildOverviews(
                        "AVERAGE", static_cast<int>(lv.size()), lv.data(), 0,
                        nullptr, nullptr, nullptr, nullptr);
                    if (oe != CE_None) {
                        ip::logWarn("overview 构建失败（继续切片，低 zoom 会较慢）: " +
                                    std::string(CPLGetLastErrorMsg()));
                    }
                }
            }
        }

        // 3. WarpedVRT（落盘供多线程各开 handle）
        fs::create_directories(params_.output_dir);
        const std::string tmpDir =
            (fs::path(params_.output_dir) / "_tmp").string();
        fs::create_directories(tmpDir);
        const std::string warpVrt = (fs::path(tmpDir) / "warp.vrt").string();
        double warpRes = 0;
        ip::GeoExtent extent;
        report(cb, 5, "构建 WarpedVRT（" + params_.resampling + "）");
        if (!buildWarpedVrt(src.get(), tms_, params_.resampling, warpVrt,
                            warpRes, extent, err)) {
            return false;
        }
        ip::logInfo("Warp 后分辨率 " + std::to_string(warpRes) +
                    (tms_.geographic ? "°/px" : "m/px"));
        ip::logInfo("Warp 数据范围: [" + std::to_string(extent.minX) + ", " +
                    std::to_string(extent.minY) + ", " +
                    std::to_string(extent.maxX) + ", " +
                    std::to_string(extent.maxY) + "]");

        // 3.5 增量范围（bbox 按 TMS CRS 坐标；与数据范围求交）
        if (!params_.bbox.empty()) {
            double bx0, by0, bx1, by1;
            if (std::sscanf(params_.bbox.c_str(), "%lf,%lf,%lf,%lf", &bx0,
                            &by0, &bx1, &by1) != 4) {
                err = "bbox 格式应为 minX,minY,maxX,maxY: " + params_.bbox;
                return false;
            }
            extent.minX = std::max(extent.minX, std::min(bx0, bx1));
            extent.maxX = std::min(extent.maxX, std::max(bx0, bx1));
            extent.minY = std::max(extent.minY, std::min(by0, by1));
            extent.maxY = std::min(extent.maxY, std::max(by0, by1));
            if (extent.minX >= extent.maxX || extent.minY >= extent.maxY) {
                err = "bbox 与数据范围无交集";
                return false;
            }
            ip::logInfo("增量范围交集: [" + std::to_string(extent.minX) + ", " +
                        std::to_string(extent.minY) + ", " +
                        std::to_string(extent.maxX) + ", " +
                        std::to_string(extent.maxY) + "]");
        }

        // 4. zoom 推算：max = 最大 z 使 levelRes >= warpRes（不超采样）
        //    CLI 的 zoom 语义 = identifier（天地图层级），换算到矢量索引
        int maxZ = 0;
        for (int z = 0; z < tms_.levelCount(); ++z) {
            if (tms_.level(z).res >= warpRes) maxZ = z;
        }
        int minZ = params_.min_zoom >= 0 ? params_.min_zoom - tms_.idOffset
                                         : 0;
        if (params_.max_zoom >= 0) maxZ = params_.max_zoom - tms_.idOffset;
        maxZ = std::min(maxZ, tms_.levelCount() - 1);
        minZ = std::min(minZ, maxZ);
        ip::logInfo("zoom 范围: " + std::to_string(tms_.dirLevel(minZ)) + "~" +
                    std::to_string(tms_.dirLevel(maxZ)) +
                    "（源分辨率锚定，over-zoom 可 --max-zoom 覆盖）");

        // 5. 枚举任务
        struct TileJob {
            int levelIdx;
            long long tx, ty;  // xyz
        };
        std::vector<TileJob> jobs;
        for (int z = minZ; z <= maxZ; ++z) {
            long long tx0, ty0, tx1, ty1;
            if (!levelTileRange(tms_, z, params_.tile_size, params_.tile_size,
                                extent.minX, extent.minY, extent.maxX,
                                extent.maxY, tx0, ty0, tx1, ty1)) {
                continue;
            }
            // 增量切片外扩缓冲（覆盖接边带；levelTileRange 已裁剪，此处手动扩并夹取）
            const long long buf = params_.tile_buffer;
            if (buf > 0) {
                tx0 = std::max<long long>(tx0 - buf, 0);
                ty0 = std::max<long long>(ty0 - buf, 0);
                tx1 = std::min<long long>(tx1 + buf,
                                          tms_.level(z).matrixW - 1);
                ty1 = std::min<long long>(ty1 + buf,
                                          tms_.level(z).matrixH - 1);
            }
            for (long long ty = ty0; ty <= ty1; ++ty) {
                for (long long tx = tx0; tx <= tx1; ++tx) {
                    jobs.push_back({z, tx, ty});
                }
            }
        }
        ip::logInfo("候选瓦片 " + std::to_string(jobs.size()) + " 张");
        report(cb, 10, "候选瓦片 " + std::to_string(jobs.size()) + " 张");

        // 6. 渲染上下文
        RenderCtx ctx;
        ctx.params = &params_;
        ctx.nBands = src->GetRasterCount();
        ctx.tileSize = params_.tile_size;
        ctx.threshold = params_.nodata_pct_threshold;
        ctx.skipEmpty = params_.skip_empty_tiles;
        ctx.fillMode = params_.nodata_mode == "fill_color";
        ctx.fillValue = params_.fill_value;
        ctx.format = params_.format;
        ctx.jpegQuality = params_.jpeg_quality;
        ctx.webpQuality = params_.webp_quality;
        ctx.alg = parseResampleAlg(params_.resampling);
        ctx.outputDir = params_.output_dir;
        ctx.dataExtent = extent;
        ctx.resume = params_.resume;
        ctx.overwrite = params_.overwrite;

        // 7. 线程池并行渲染
        const int threads = params_.num_threads > 0
                                ? params_.num_threads
                                : static_cast<int>(std::thread::hardware_concurrency());
        ip::logInfo("线程数 " + std::to_string(threads));
        ip::ThreadPool pool(static_cast<size_t>(threads));
        std::atomic<long> written{0}, skipEmpty{0}, skipTh{0}, outside{0}, done{0};
        std::atomic<long> resumed{0};
        std::mutex errMutex;
        std::atomic<bool> failed{false};
        std::string firstErr;
        std::vector<std::future<void>> futs;
        futs.reserve(jobs.size());
        const int total = static_cast<int>(jobs.size());

        for (const auto& job : jobs) {
            futs.push_back(pool.submit([&]() {
                if (failed.load()) return;
                GDALDataset* wds = threadLocalWarpDs(warpVrt);
                if (!wds) {
                    std::lock_guard<std::mutex> lk(errMutex);
                    if (!failed.exchange(true)) firstErr = "线程打开 warp VRT 失败";
                    return;
                }
                const TileBbox bbox = tileBbox(tms_, job.levelIdx, job.tx,
                                               job.ty, params_.tile_size,
                                               params_.tile_size);
                const int dirLevel = tms_.dirLevel(job.levelIdx);
                const long long tyOut = params_.convention == "xyz"
                                            ? job.ty
                                            : (tms_.level(job.levelIdx).matrixH - 1 - job.ty);
                std::string terr;
                const int rc = renderTile(wds, bbox, ctx,
                                          tms_.level(job.levelIdx).res,
                                          dirLevel, job.tx, tyOut, terr);
                switch (rc) {
                    case 0: written++; break;
                    case 1: skipEmpty++; break;
                    case 2: skipTh++; break;
                    case 3: outside++; break;
                    case 4: resumed++; break;
                    default:
                        std::lock_guard<std::mutex> lk(errMutex);
                        if (!failed.exchange(true)) firstErr = terr;
                        return;
                }
                const int d = ++done;
                if (d % std::max(1, total / 20) == 0 || d == total) {
                    const int pct = 10 + static_cast<int>(85.0 * d / total);
                    report(cb, pct, "瓦片进度 " + std::to_string(d) + "/" +
                                        std::to_string(total));
                }
            }));
        }
        // 等待全部任务完成
        for (auto& f : futs) f.get();
        if (failed.load()) {
            err = firstErr;
            return false;
        }

        // 8. 清理临时 VRT
        if (!params_.keep_tmp_vrt) {
            std::error_code ec;
            fs::remove(warpVrt, ec);
            fs::remove(fs::path(tmpDir), ec);
        }

        stats.tilesWritten = written;
        stats.tilesResumed = resumed;
        stats.tilesSkippedEmpty = skipEmpty;
        stats.tilesSkippedThreshold = skipTh;
        stats.tilesOutsideData = outside;
        stats.elapsedSec =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
                .count();
        report(cb, 100, "切片完成");
        ip::logInfo("==== 切片结果 ====");
        ip::logInfo("  写出 " + std::to_string(written) + "，续跑跳过 " +
                    std::to_string(resumed) + "，跳过(全无效) " +
                    std::to_string(skipEmpty) + "，跳过(阈值) " +
                    std::to_string(skipTh) + "，数据外 " +
                    std::to_string(outside) + "，耗时 " +
                    std::to_string(stats.elapsedSec) + "s");
        return true;
    }

    TilingParams params_;
    TileMatrixSet tms_;
};

} // namespace

extern "C" {

ITiling* createTiling() { return new (std::nothrow) TilingImpl(); }

void destroyTiling(ITiling* op) { delete op; }

}

} // namespace iptl
