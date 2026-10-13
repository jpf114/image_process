// color_balance operator 主实现：收集 → 全局统计 → 重叠图 → 接边检查/平差解算
#include "cb_internal.h"

#include "ip/json_io.h"
#include "ip/log.h"
#include "ip/mem_guard.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>

namespace fs = std::filesystem;

namespace ipcb {

using ip::BandStats3;
using ip::DataSetPtr;
using ip::GeoExtent;

namespace {

bool isTiffName(const std::string& name) {
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) lower.push_back(static_cast<char>(std::tolower(
        static_cast<unsigned char>(c))));
    for (const char* e : {".tif", ".tiff"}) {
        const size_t len = std::strlen(e);
        if (lower.size() > len &&
            lower.compare(lower.size() - len, len, e) == 0) {
            return true;
        }
    }
    return false;
}

void collectInputFiles(const std::string& in, std::vector<std::string>& out,
                       std::string& err) {
    std::error_code ec;
    if (fs::is_regular_file(in, ec)) {
        out.push_back(in);
        return;
    }
    if (fs::is_directory(in, ec)) {
        for (const auto& entry : fs::recursive_directory_iterator(in)) {
            if (!entry.is_regular_file(ec)) continue;
            const std::string fname = entry.path().filename().string();
            if (fname.find(".aux.xml") != std::string::npos) continue;  // GDAL 辅助文件
            if (isTiffName(fname)) out.push_back(entry.path().string());
        }
        std::sort(out.begin(), out.end());
        return;
    }
    err = "输入不存在: " + in;
}

std::string fmt3(const double v[3]) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%.2f/%.2f/%.2f", v[0], v[1], v[2]);
    return buf;
}
std::string fmt1(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

class ColorBalanceImpl : public IColorBalance {
public:
    bool configure(const ColorBalanceParams& p, std::string& err) override {
        params_ = p;
        if (params_.inputs.empty()) {
            err = "缺少输入（--input）";
            return false;
        }
        if (params_.output_dir.empty()) {
            err = "缺少输出目录（--output-dir）";
            return false;
        }
        if (params_.method != "overlap_adjust" &&
            params_.method != "overlap_adjust+wallis" &&
            params_.method != "block_wallis" &&
            params_.method != "hist_match" &&
            params_.method != "global_adjust") {
            err = "method 仅支持 overlap_adjust / overlap_adjust+wallis / "
                  "block_wallis / hist_match / global_adjust";
            return false;
        }
        if (params_.output_mode != "tif" && params_.output_mode != "vrt") {
            err = "output_mode 仅支持 tif / vrt";
            return false;
        }
        if (params_.scene_format != "gtiff" && params_.scene_format != "cog") {
            err = "scene_format 仅支持 gtiff / cog";
            return false;
        }
        if (params_.method != "block_wallis" &&
            params_.method != "overlap_adjust+wallis" && params_.mosaic_cog) {
            ip::logWarn("mosaic_cog 仅在 vrt 输出模式下有效（已忽略）");
        }
        if (params_.block_size < 64) {
            err = "block_size 最小 64";
            return false;
        }
        if (params_.seam_check_threshold <= 0 || params_.min_overlap_px <= 0 ||
            params_.overlap_max_side < 16 || params_.apply_chunk < 64) {
            err = "参数取值非法（threshold>0, min_overlap_px>0, "
                  "overlap_max_side>=16, apply_chunk>=64）";
            return false;
        }
        if (params_.profile_path.empty()) {
            params_.profile_path =
                (fs::path(params_.output_dir) / "_profile.json").string();
        }
        return true;
    }

    bool execute(const ProgressCallback& cb, std::string& err) override {
        try {
            return run(cb, err);
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

    bool run(const ProgressCallback& cb, std::string& err) {
        // 运行日志落盘（每次处理一个文件，避免持续开发无日志）
        {
            char ts[32] = {0};
            std::time_t t = std::time(nullptr);
            std::tm tmbuf{};
            localtime_r(&t, &tmbuf);
            std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tmbuf);
            const std::string logPath =
                (fs::path(params_.output_dir) / "logs" /
                 (std::string("run_color_balance_") + ts + ".log"))
                    .string();
            ip::logSetFile(logPath);
            ip::logInfo("==== color_balance 开始 ====，日志: " + logPath);
        }
        ip::MemGuard::checkOrThrow(params_.min_mem_gb, "开始");
        if (params_.use_gpu) {
            ip::logWarn("use_gpu=true 但本期为纯 CPU 实现（接口占位），按 CPU 执行");
        }

        // 1. 收集输入
        std::vector<std::string> files;
        for (const auto& in : params_.inputs) {
            collectInputFiles(in, files, err);
            if (!err.empty()) return false;
        }
        std::sort(files.begin(), files.end());
        files.erase(std::unique(files.begin(), files.end()), files.end());
        if (files.empty()) {
            err = "未找到任何输入影像";
            return false;
        }
        report(cb, 5, "共收集 " + std::to_string(files.size()) + " 景影像");

        // 2. 打开并读取基本信息
        scenes_.clear();
        for (const auto& f : files) {
            Scene s;
            s.path = f;
            s.id = fs::path(f).stem().string();
            s.ds = ip::openReadOnly(f);
            if (!s.ds) {
                err = "无法打开影像: " + f + "（GDAL: " + CPLGetLastErrorMsg() + "）";
                return false;
            }
            s.w = s.ds->GetRasterXSize();
            s.h = s.ds->GetRasterYSize();
            if (s.ds->GetRasterCount() < 3) {
                err = "影像少于 3 波段，暂不支持: " + f;
                return false;
            }
            if (s.ds->GetGeoTransform(s.gt) != CE_None) {
                err = "影像缺少地理信息: " + f;
                return false;
            }
            s.extent = ip::datasetExtent(s.ds.get());
            if (const OGRSpatialReference* srs = s.ds->GetSpatialRef()) {
                const char* code = srs->GetAuthorityCode(nullptr);
                s.crs = code ? code : "";
            }
            scenes_.push_back(std::move(s));
        }
        for (const auto& s : scenes_) {
            if (s.crs != scenes_[0].crs) {
                err = "输入影像 CRS 不一致（" + scenes_[0].id + ":EPSG:" +
                      scenes_[0].crs + " vs " + s.id + ":EPSG:" + s.crs +
                      "），跨 CRS 平差将在后续版本支持";
                return false;
            }
        }

        // 3. 参考景
        int refIdx = 0;
        if (!params_.ref_image.empty()) {
            const std::string want = fs::path(params_.ref_image).stem().string();
            bool found = false;
            for (size_t i = 0; i < scenes_.size(); ++i) {
                if (scenes_[i].id == want) {
                    refIdx = static_cast<int>(i);
                    found = true;
                    break;
                }
            }
            if (!found) {
                err = "参考影像不在输入集合中: " + params_.ref_image;
                return false;
            }
        }

        // 4. 全局精确统计（分块直方图，恒定内存）
        for (size_t i = 0; i < scenes_.size(); ++i) {
            ip::MemGuard::checkOrThrow(params_.min_mem_gb, "统计前");
            const int pct = 5 + static_cast<int>(10 * (i + 1) / scenes_.size());
            report(cb, pct, "精确统计 " + scenes_[i].id);
            scenes_[i].stats = ip::exactStatsRGB(scenes_[i].ds.get());
            if (scenes_[i].stats.validCount[0] == 0) {
                err = "影像无有效像素: " + scenes_[i].path;
                return false;
            }
            ip::logInfo("  " + scenes_[i].id + " mean=" +
                        fmt3(scenes_[i].stats.mean) + " std=" +
                        fmt3(scenes_[i].stats.std));
        }

        const bool needGraph = params_.method == "overlap_adjust" ||
                               params_.method == "overlap_adjust+wallis" ||
                               params_.method == "global_adjust";
        std::vector<OverlapEdge> edges;
        if (needGraph) {
            // 5. 重叠图（两两几何交集 + 重叠区统计）
            report(cb, 20, "构建重叠图");
            for (size_t i = 0; i < scenes_.size(); ++i) {
                for (size_t j = i + 1; j < scenes_.size(); ++j) {
                    OverlapEdge e =
                        buildEdge(static_cast<int>(i), static_cast<int>(j));
                    if (e.a >= 0) edges.push_back(std::move(e));
                }
            }
            ip::logInfo("有效重叠边 " + std::to_string(edges.size()) + " 条");
            for (const auto& e : edges) {
                ip::logInfo("  " + scenes_[e.a].id + " | " + scenes_[e.b].id +
                            "  Δμ(max)=" + fmt1(e.deltaMuMax) + "  (" +
                            fmt3(e.deltaMu) + ")");
            }
            // 6. 解算参数
            report(cb, 35, "接边一致性检查与平差解算");
            if (!solve(refIdx, edges, err)) return false;
        } else {
            // block_wallis / hist_match：不依赖图平差，目标=参考景全局统计/直方图
            for (auto& S : scenes_) S.paramsSet = true;
            scenes_[refIdx].note = "参考景";
            report(cb, 35, "方法 " + params_.method + " 无需图平差");
        }

        // 7. 应用（块式写输出；vrt 模式线性景零像素落盘）
        fs::create_directories(params_.output_dir);
        const bool useWallis = params_.method == "block_wallis" ||
                               params_.method == "overlap_adjust+wallis";
        const bool useHist = params_.method == "hist_match";
        const bool vrtMode = params_.output_mode == "vrt";
        uint64_t refHist[3][256];
        bool refHistReady = false;
        std::vector<std::string> outPaths;
        std::vector<VrtSource> vrtSources;
        for (size_t i = 0; i < scenes_.size(); ++i) {
            ip::MemGuard::checkOrThrow(params_.min_mem_gb, "应用前");
            const int pct = 45 + static_cast<int>(45 * (i + 1) / scenes_.size());
            Scene& S = scenes_[i];
            const std::string stem = S.id + "_balanced";
            std::string outPath;
            if (useWallis) {
                report(cb, pct, "应用 Wallis " + S.id);
                outPath = (fs::path(params_.output_dir) / (stem + ".tif")).string();
                // 目标 μ=参考景；σ=wallis_target_std（>0 显式值）否则参考景 std
                double tStd[3];
                for (int b = 0; b < 3; ++b) {
                    tStd[b] = params_.wallis_target_std > 0
                                  ? params_.wallis_target_std
                                  : scenes_[refIdx].stats.std[b];
                }
                const WallisGrid wg = computeWallisGrid(
                    S.ds.get(), scenes_[refIdx].stats.mean, tStd,
                    params_.block_size, S.gain, S.offset);
                if (!applyWallisToFile(S.ds.get(), outPath, wg,
                                       params_.apply_chunk, err,
                                       params_.scene_format.c_str())) {
                    return false;
                }
            } else if (useHist) {
                report(cb, pct, "应用直方图匹配 " + S.id);
                outPath = (fs::path(params_.output_dir) / (stem + ".tif")).string();
                if (!refHistReady) {
                    exactHistograms(scenes_[refIdx].ds.get(), refHist);
                    refHistReady = true;
                }
                uint64_t srcHist[3][256];
                exactHistograms(S.ds.get(), srcHist);
                uint8_t luts[3][256];
                for (int b = 0; b < 3; ++b) {
                    cdfMatchLut(srcHist[b], refHist[b], luts[b]);
                }
                if (!applyLutToFile(S.ds.get(), outPath, luts,
                                    params_.apply_chunk, err,
                                    params_.scene_format.c_str())) {
                    return false;
                }
            } else if (vrtMode) {
                report(cb, pct, "生成匀色 VRT " + S.id);
                outPath = (fs::path(params_.output_dir) / (stem + ".vrt")).string();
                if (!writeBalancedVrt(S.ds.get(), outPath, S.gain, S.offset,
                                      err)) {
                    return false;
                }
                if (!verifyBalancedVrt(S.ds.get(), outPath, S.gain, S.offset,
                                       err)) {
                    return false;
                }
            } else {
                report(cb, pct, "应用参数 " + S.id +
                                    (S.adjusted ? "" : "（无需调节，直拷）"));
                outPath = (fs::path(params_.output_dir) / (stem + ".tif")).string();
                if (!applyGainOffsetToFile(S.ds.get(), outPath, S.gain, S.offset,
                                           params_.apply_chunk, err,
                                           params_.scene_format.c_str())) {
                    return false;
                }
            }
            outPaths.push_back(outPath);
            if (vrtMode) {
                // 镶嵌 VRT 直接引用原始影像 + ComplexSource 内联匀色参数
                //（避免嵌套派生波段的 decim 缺陷；workflow --only tiling 也无需注册）
                VrtSource vs;
                vs.path = S.path;
                vs.minX = S.extent.minX;
                vs.minY = S.extent.minY;
                vs.maxX = S.extent.maxX;
                vs.maxY = S.extent.maxY;
                for (int b = 0; b < 3; ++b) {
                    vs.gain[b] = S.gain[b];
                    vs.offset[b] = S.offset[b];
                }
                vs.rasterX = S.w;
                vs.rasterY = S.h;
                vrtSources.push_back(std::move(vs));
            }
        }
        if (vrtMode) {
            report(cb, 93, "生成镶嵌 VRT");
            const std::string mosaicPath =
                (fs::path(params_.output_dir) / "mosaic.vrt").string();
            if (!writeMosaicVrt(vrtSources, scenes_[0].ds->GetRasterCount(),
                                mosaicPath, err)) {
                return false;
            }
            ip::logInfo("镶嵌 VRT: " + mosaicPath);
            // 物理镶嵌 COG（方案 §4.2 balance_and_mosaic；mosaic_cog=true 时）
            if (params_.mosaic_cog) {
                report(cb, 96, "输出物理镶嵌 COG");
                const std::string cogPath =
                    (fs::path(params_.output_dir) / "mosaic_cog.tif").string();
                GDALDriver* cog =
                    GetGDALDriverManager()->GetDriverByName("COG");
                GDALDataset* m = ip::openReadOnly(mosaicPath).release();
                if (!cog || !m) {
                    err = "COG 驱动或镶嵌 VRT 不可用";
                    return false;
                }
                char** copts = CSLSetNameValue(nullptr, "COMPRESS", "DEFLATE");
                copts = CSLSetNameValue(copts, "BLOCKSIZE", "512");
                GDALDataset* out = cog->CreateCopy(
                    cogPath.c_str(), m, FALSE, copts, nullptr, nullptr);
                CSLDestroy(copts);
                GDALClose(m);
                if (!out) {
                    err = "镶嵌 COG CreateCopy 失败: " + cogPath;
                    return false;
                }
                GDALClose(out);
                ip::logInfo("物理镶嵌 COG: " + cogPath);
            }
        }

        // 8. 方案 JSON
        if (params_.write_profile) {
            report(cb, 95, "写出方案文件");
            if (!writeProfile(refIdx, edges, outPaths, err)) return false;
        }

        // 9. 汇总
        report(cb, 100, "完成");
        ip::logInfo("==== 匀色结果摘要 ====");
        for (const auto& s : scenes_) {
            ip::logInfo("  " + s.id + ": " + (s.adjusted ? "已调节" : "未调节") +
                        (s.note.empty() ? "" : "（" + s.note + "）"));
        }
        return true;
    }

    // 两景几何交集 → 有效则统计重叠区；返回 a=-1 表示无有效重叠
    OverlapEdge buildEdge(int ia, int ib) {
        OverlapEdge e;
        Scene& A = scenes_[ia];
        Scene& B = scenes_[ib];
        GeoExtent inter;
        inter.minX = std::max(A.extent.minX, B.extent.minX);
        inter.minY = std::max(A.extent.minY, B.extent.minY);
        inter.maxX = std::min(A.extent.maxX, B.extent.maxX);
        inter.maxY = std::min(A.extent.maxY, B.extent.maxY);
        if (inter.minX >= inter.maxX || inter.minY >= inter.maxY) return e;

        int ax0, ay0, aw, ah, bx0, by0, bw, bh;
        if (!ip::geoBoundsToPixelWindow(A.ds.get(), inter, ax0, ay0, aw, ah) ||
            !ip::geoBoundsToPixelWindow(B.ds.get(), inter, bx0, by0, bw, bh)) {
            return e;
        }
        if (std::min(aw, ah) < params_.min_overlap_px ||
            std::min(bw, bh) < params_.min_overlap_px) {
            ip::logInfo("  " + A.id + "|" + B.id + " 重叠过窄(" +
                        std::to_string(std::min(aw, ah)) + "×" +
                        std::to_string(std::min(bw, bh)) + ") 忽略");
            return e;
        }
        e.a = ia;
        e.b = ib;
        e.bounds = inter;
        e.ax0 = ax0; e.ay0 = ay0; e.waW = aw; e.waH = ah;
        e.bx0 = bx0; e.by0 = by0; e.wbW = bw; e.wbH = bh;
        e.statsA = ip::windowStatsRGB(A.ds.get(), ax0, ay0, aw, ah,
                                      params_.overlap_max_side);
        e.statsB = ip::windowStatsRGB(B.ds.get(), bx0, by0, bw, bh,
                                      params_.overlap_max_side);
        for (int b = 0; b < 3; ++b) {
            e.deltaMu[b] = e.statsB.mean[b] - e.statsA.mean[b];
            e.deltaMuMax = std::max(e.deltaMuMax, std::abs(e.deltaMu[b]));
        }
        return e;
    }

    // 解算：
    //  overlap_adjust（主方法）：接边一致性门控 + 链式（BFS）重叠区平差；
    //                           无可用边的景保持原样并告警
    //  global_adjust（显式指定）：全部景对齐参考景全局 μ/σ（有接缝风险，告警）
    bool solve(int refIdx, std::vector<OverlapEdge>& edges, std::string& err) {
        (void)err;
        Scene& ref = scenes_[refIdx];
        ref.paramsSet = true;
        ref.note = "参考景";
        ip::logInfo("参考景: " + ref.id);

        if (params_.method == "global_adjust") {
            for (size_t i = 0; i < scenes_.size(); ++i) {
                if (static_cast<int>(i) == refIdx) continue;
                Scene& S = scenes_[i];
                for (int b = 0; b < 3; ++b) {
                    const double g =
                        ref.stats.std[b] / std::max(S.stats.std[b], 1e-3);
                    S.gain[b] = std::min(std::max(g, 0.2), 5.0);
                    S.offset[b] = ref.stats.mean[b] - S.gain[b] * S.stats.mean[b];
                }
                S.paramsSet = true;
                S.adjusted = true;
                S.note = "global_adjust（对齐参考景全局统计）";
                ip::logWarn("  " + S.id + " " + S.note +
                            " gain=" + fmt3(S.gain));
            }
            return true;
        }

        // 接边一致性门控
        for (auto& e : edges) {
            if (e.deltaMuMax < params_.seam_check_threshold) {
                e.skipped = true;
                ip::logInfo("  边 " + scenes_[e.a].id + "|" + scenes_[e.b].id +
                            " Δμ=" + fmt1(e.deltaMuMax) + " < 阈值" +
                            fmt1(params_.seam_check_threshold) +
                            " → 跳过调节（接边本已连续）");
            }
        }

        // 等价类缩点：skip 边合并（类内恒等共享参数）；未 skip 边构成类间约束图
        const int n = static_cast<int>(scenes_.size());
        std::vector<int> parent(n);
        for (int i = 0; i < n; ++i) parent[i] = i;
        std::function<int(int)> find = [&](int x) {
            while (parent[x] != x) parent[x] = parent[parent[x]], x = parent[x];
            return x;
        };
        for (const auto& e : edges) {
            if (e.skipped) {
                parent[find(e.a)] = find(e.b);
            }
        }
        std::map<int, int> rootToClass;
        std::vector<int> classOf(n, -1);
        int nClass = 0;
        for (int i = 0; i < n; ++i) {
            const int r = find(i);
            auto it = rootToClass.find(r);
            if (it == rootToClass.end()) it = rootToClass.emplace(r, nClass++).first;
            classOf[i] = it->second;
        }
        const int refClass = classOf[refIdx];

        // 类间约束边（未 skip、跨类）
        struct ClassEdge {
            int ca, cb;
            OverlapEdge* e;
        };
        std::vector<ClassEdge> cedges;
        for (auto& e : edges) {
            if (e.skipped) continue;
            const int ca = classOf[e.a], cb = classOf[e.b];
            if (ca == cb) {
                ip::logWarn("  边 " + scenes_[e.a].id + "|" + scenes_[e.b].id +
                            " Δμ 大但被归入同一等价类，忽略该约束（数据自洽性异常）");
                continue;
            }
            cedges.push_back({ca, cb, &e});
            e.usedForSolve = true;
        }
        ip::logInfo("图平差: " + std::to_string(nClass) + " 个等价类, " +
                    std::to_string(cedges.size()) + " 条约束边");

        // 两步最小二乘：log-gain（σ 一致约束）→ offset（μ 一致约束，固定 gain）
        // 未与参考类连通的类解为 0（log-gain=0, offset=0）→ 恒等保持
        std::vector<std::array<double, 3>> gainCls(nClass, {1, 1, 1});
        std::vector<std::array<double, 3>> offsetCls(nClass, {0, 0, 0});
        for (int b = 0; b < 3; ++b) {
            std::vector<LsEquation> eqs;
            for (const auto& ce : cedges) {
                const BandStats3& sa = ce.e->statsA;
                const BandStats3& sb = ce.e->statsB;
                // g_cb·σ_b^ov = g_ca·σ_a^ov → log g_cb - log g_ca = log(σ_a/σ_b)
                LsEquation eq;
                eq.coeffs[ce.cb] = 1.0;
                eq.coeffs[ce.ca] = -1.0;
                eq.rhs = std::log(std::max(sa.std[b], 1e-3)) -
                         std::log(std::max(sb.std[b], 1e-3));
                eq.weight = 1.0;
                eqs.push_back(std::move(eq));
            }
            LsEquation anchor;
            anchor.coeffs[refClass] = 1.0;
            anchor.rhs = 0.0;
            anchor.weight = 1000.0;  // 参考类强锚定
            eqs.push_back(std::move(anchor));
            std::vector<double> x;
            solveLeastSquares(eqs, nClass, refClass, x);
            for (int c = 0; c < nClass; ++c) {
                gainCls[c][b] = std::min(std::max(std::exp(x[c]), 0.2), 5.0);
            }
        }
        for (int b = 0; b < 3; ++b) {
            std::vector<LsEquation> eqs;
            for (const auto& ce : cedges) {
                const BandStats3& sa = ce.e->statsA;
                const BandStats3& sb = ce.e->statsB;
                // g_ca·μ_a + o_ca = g_cb·μ_b + o_cb → o_cb - o_ca = g_ca·μ_a - g_cb·μ_b
                LsEquation eq;
                eq.coeffs[ce.cb] = 1.0;
                eq.coeffs[ce.ca] = -1.0;
                eq.rhs = gainCls[ce.ca][b] * sa.mean[b] - gainCls[ce.cb][b] * sb.mean[b];
                eq.weight = 1.0;
                eqs.push_back(std::move(eq));
            }
            LsEquation anchor;
            anchor.coeffs[refClass] = 1.0;
            anchor.rhs = 0.0;
            anchor.weight = 1000.0;
            eqs.push_back(std::move(anchor));
            std::vector<double> x;
            solveLeastSquares(eqs, nClass, refClass, x);
            for (int c = 0; c < nClass; ++c) {
                offsetCls[c][b] = std::min(std::max(x[c], -100.0), 100.0);
            }
        }

        // 参数写回每景 + note
        for (int i = 0; i < n; ++i) {
            Scene& S = scenes_[i];
            const int cls = classOf[i];
            for (int b = 0; b < 3; ++b) {
                S.gain[b] = gainCls[cls][b];
                S.offset[b] = offsetCls[cls][b];
            }
            S.paramsSet = true;
            bool isIdentity = true;
            for (int b = 0; b < 3; ++b) {
                if (std::abs(S.gain[b] - 1.0) > 1e-6 ||
                    std::abs(S.offset[b]) > 1e-6) {
                    isIdentity = false;
                    break;
                }
            }
            S.adjusted = !isIdentity;
            if (cls == refClass) {
                S.note = i == refIdx ? "参考景" : "与参考景接边连续，恒等保持";
            } else if (S.adjusted) {
                S.note = "最小二乘图平差解算（" +
                         std::to_string(cedges.size()) + " 条约束边均衡）";
                ip::logInfo("  " + S.id + " gain=" + fmt3(S.gain) +
                            " offset=" + fmt3(S.offset));
            } else {
                S.note = "与参考景无连通约束，保持原样";
                ip::logWarn("  " + S.id + " " + S.note);
            }
        }

        // 各约束边的均衡后残差（A 调节后 - B 调节后，重叠区均值意义）
        for (const auto& ce : cedges) {
            for (int b = 0; b < 3; ++b) {
                const double pa = gainCls[ce.ca][b] * ce.e->statsA.mean[b] +
                                  offsetCls[ce.ca][b];
                const double pb = gainCls[ce.cb][b] * ce.e->statsB.mean[b] +
                                  offsetCls[ce.cb][b];
                ce.e->residualAfter[b] = pa - pb;
            }
            const double m = std::max({std::abs(ce.e->residualAfter[0]),
                                       std::abs(ce.e->residualAfter[1]),
                                       std::abs(ce.e->residualAfter[2])});
            ip::logInfo("  边 " + scenes_[ce.e->a].id + "|" + scenes_[ce.e->b].id +
                        " 均衡后残差(max)=" + fmt1(m) + " (" +
                        fmt3(ce.e->residualAfter) + ")");
        }
        return true;
    }

    bool writeProfile(int refIdx, const std::vector<OverlapEdge>& edges,
                      const std::vector<std::string>& outPaths, std::string& err) {
        std::vector<SceneSnapshot> snaps;
        for (size_t i = 0; i < scenes_.size(); ++i) {
            SceneSnapshot t;
            t.id = scenes_[i].id;
            t.path = scenes_[i].path;
            for (int b = 0; b < 3; ++b) {
                t.mean[b] = scenes_[i].stats.mean[b];
                t.std[b] = scenes_[i].stats.std[b];
                t.gain[b] = scenes_[i].gain[b];
                t.offset[b] = scenes_[i].offset[b];
            }
            t.adjusted = scenes_[i].adjusted;
            t.note = scenes_[i].note;
            t.outputPath = i < outPaths.size() ? outPaths[i] : "";
            snaps.push_back(std::move(t));
        }
        std::vector<EdgeSnapshot> esnaps;
        esnaps.reserve(edges.size());
        for (const auto& e : edges) {
            EdgeSnapshot t;
            t.idA = scenes_[e.a].id;
            t.idB = scenes_[e.b].id;
            for (int b = 0; b < 3; ++b) t.deltaMu[b] = e.deltaMu[b];
            t.deltaMuMax = e.deltaMuMax;
            t.skipped = e.skipped;
            t.usedForSolve = e.usedForSolve;
            for (int b = 0; b < 3; ++b) t.residualAfter[b] = e.residualAfter[b];
            esnaps.push_back(std::move(t));
        }
        const ip::Json j = buildProfileJson(
            params_.method, scenes_[refIdx].id, scenes_[refIdx].path,
            scenes_[refIdx].stats, snaps, esnaps, params_.seam_check_threshold);
        if (!ip::writeJsonFile(params_.profile_path, j, err)) return false;
        ip::logInfo("方案文件: " + params_.profile_path);
        return true;
    }

    ColorBalanceParams params_;
    std::vector<Scene> scenes_;
};

} // namespace

extern "C" {

IColorBalance* createColorBalance() { return new (std::nothrow) ColorBalanceImpl(); }

void destroyColorBalance(IColorBalance* op) { delete op; }

void ensureColorBalancePixelFunc() { ensurePixelFuncRegistered(); }

}

} // namespace ipcb
