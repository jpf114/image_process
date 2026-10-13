// tiling 打包输出（S4）：把瓦片写入 MBTiles / GeoPackage 单文件包
//
// 实现依据（2026-10-13 对随包发布的 GDAL 3.12.4 实测）：
//  - MBTiles / GPKG 驱动均支持 Create + 逐瓦片块写（DCAP_CREATE=YES）；
//    tiles 表行由 GDAL 维护，无需自建 SQLite schema，也不引入新依赖。
//  - 包内栅格 = 该层级「全球满网格」虚拟栅格（原点 = TMS topLeftCorner，
//    分辨率 = 层级 res，尺寸 = matrixW×matrixH × tileSize），因此
//    块索引 (col,row) 即瓦片编号 (tx, tyXYZ)，无需偏移换算。
//    虚拟栅格按块访问，不分配整幅内存（16777216×16777216 实测可用）。
//  - MBTiles 驱动拒绝在「低于 tiles 表已有最小 zoom」的层级打开，
//    故必须按层级升序写入（调用方已按 z 升序分批提交）。
//  - GPKG 驱动要求目标层级已登记在 gpkg_tile_matrix，故 Create 时以
//    ZOOM_LEVEL=max 一并登记 0..max 全部层级。
//  - GDAL 关闭时会把 minzoom/maxzoom 写成「当次句柄的层级」，与真实
//    内容不符；收尾时经 OGR SQLite 驱动直接修正 metadata 表。
#include "tile_ctx.h"

#include "ip/gdal_raii.h"
#include "ip/log.h"

#include <gdal_priv.h>
#include <ogr_spatialref.h>
#include <ogrsf_frmts.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace fs = std::filesystem;

namespace iptl {

namespace {

// 参数 format → GDAL 驱动的 TILE_FORMAT
const char* tileFormatName(const std::string& format) {
    if (format == "jpeg") return "JPEG";
    if (format == "webp") return "WEBP";
    return "PNG";
}

// 以 double 往返精度输出：GDAL 的 TILING_SCHEME 解析会校验相邻层级分辨率
// 严格为 2 倍，低精度截断（如 %.10g）会触发 "resolution ... not always 2"
std::string trimNum(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

class PackageSink : public TileSink {
public:
    PackageSink(const TilingParams& p, const TileMatrixSet& tms, int firstIdx,
                int lastIdx, int outBands, int tileSize)
        : tms_(tms),
          path_(p.package_file),
          format_(p.format),
          isMbtiles_(p.package == "mbtiles"),
          firstIdx_(firstIdx),
          lastIdx_(lastIdx),
          outBands_(outBands),
          tileSize_(tileSize),
          overwrite_(p.overwrite),
          jpegQuality_(p.jpeg_quality),
          webpQuality_(p.webp_quality) {}

    ~PackageSink() override { closeHandle(); }

    // 建包：文件不存在（或 overwrite）时新建；否则按增量追加打开
    bool init(const std::string& tmsJsonPath, std::string& err) {
        tmsJsonPath_ = tmsJsonPath;
        const bool exists = fs::exists(path_);
        if (exists && !overwrite_) {
            ip::logInfo("打包：追加到既有包 " + path_);
            return true;
        }
        if (exists) {
            std::error_code ec;
            fs::remove(path_, ec);
            if (ec) {
                err = "无法覆盖既有包: " + path_ + "（" + ec.message() + "）";
                return false;
            }
        }
        return create(err);
    }

    bool write(int dirLevel, long long tx, long long tyXYZ,
               const uint8_t* data, std::string& err) override {
        std::lock_guard<std::mutex> lk(mu_);
        const int idx = dirLevel - tms_.idOffset;
        if (idx < 0 || idx >= tms_.levelCount()) {
            err = "层级越界（dirLevel=" + std::to_string(dirLevel) + "）";
            return false;
        }
        // MBTiles：层级必须单调不减（驱动限制），乱序即显式报错而非静默丢瓦片
        if (isMbtiles_ && idx < maxIdxOpened_) {
            err = "MBTiles 打包要求按层级升序写入（先 " +
                  std::to_string(maxIdxOpened_ + tms_.idOffset) + " 后 " +
                  std::to_string(dirLevel) + "）";
            return false;
        }
        if (!ensureHandle(idx, err)) return false;

        const TileMatrixLevel& lv = tms_.level(idx);
        if (tx < 0 || tyXYZ < 0 || tx >= lv.matrixW || tyXYZ >= lv.matrixH) {
            err = "瓦片编号越界 z=" + std::to_string(dirLevel) + " x=" +
                  std::to_string(tx) + " y=" + std::to_string(tyXYZ);
            return false;
        }
        // 整块写入：块索引 = 瓦片编号；带平面缓冲经 nPixelSpace/nLineSpace/
        // nBandSpace 直接映射，一次 RasterIO 完成全部波段（避免逐波段重复编码）
        const int px = static_cast<int>(tx) * tileSize_;
        const int py = static_cast<int>(tyXYZ) * tileSize_;
        int bandMap[4] = {1, 2, 3, 4};
        const CPLErr e = ds_->RasterIO(
            GF_Write, px, py, tileSize_, tileSize_, const_cast<uint8_t*>(data),
            tileSize_, tileSize_, GDT_Byte, outBands_, bandMap, 1,
            static_cast<GSpacing>(tileSize_),
            static_cast<GSpacing>(static_cast<size_t>(tileSize_) * tileSize_),
            nullptr);
        if (e != CE_None) {
            err = "写入包失败 z=" + std::to_string(dirLevel) + " x=" +
                  std::to_string(tx) + " y=" + std::to_string(tyXYZ) + "（" +
                  CPLGetLastErrorMsg() + "）";
            return false;
        }
        return true;
    }

    bool finish(std::string& err) override {
        std::lock_guard<std::mutex> lk(mu_);
        closeHandle();
        if (!isMbtiles_) return true;  // GPKG 元数据由驱动维护
        return fixMbtilesZoomMeta(err);
    }

private:
    bool create(std::string& err) {
        GDALDriver* drv =
            GetGDALDriverManager()->GetDriverByName(isMbtiles_ ? "MBTiles" : "GPKG");
        if (!drv) {
            err = std::string("GDAL 缺少驱动: ") + (isMbtiles_ ? "MBTiles" : "GPKG");
            return false;
        }
        // 满网格尺寸：MBTiles 以最浅层级建包（其驱动要求后续层级 ≥ 已有最小层级）；
        // GPKG 以最深层级建包（gpkg_tile_matrix 一次登记 0..max 全部层级）
        const int gridIdx = isMbtiles_ ? firstIdx_ : lastIdx_;
        const TileMatrixLevel& lv = tms_.level(gridIdx);
        const long long w = lv.matrixW * tileSize_;
        const long long h = lv.matrixH * tileSize_;
        if (w > INT_MAX || h > INT_MAX) {
            err = "层级 " + std::to_string(tms_.dirLevel(gridIdx)) +
                  " 满网格尺寸超出 32 位栅格上限，请下调最大层级";
            return false;
        }

        char** opts = nullptr;
        opts = CSLSetNameValue(opts, "BLOCKSIZE", std::to_string(tileSize_).c_str());
        opts = CSLSetNameValue(opts, "TILE_FORMAT", tileFormatName(format_));
        if (format_ == "jpeg") {
            opts = CSLSetNameValue(opts, "QUALITY", std::to_string(jpegQuality_).c_str());
        } else if (format_ == "webp") {
            opts = CSLSetNameValue(opts, "QUALITY", std::to_string(webpQuality_).c_str());
        } else {
            opts = CSLSetNameValue(opts, "ZLEVEL", "6");
        }
        if (!isMbtiles_) {
            opts = CSLSetNameValue(opts, "RASTER_TABLE", "tiles");
            opts = CSLSetNameValue(opts, "TILING_SCHEME", tmsJsonPath_.c_str());
            opts = CSLSetNameValue(opts, "ZOOM_LEVEL", std::to_string(gridIdx).c_str());
        }

        CPLErrorReset();
        GDALDataset* ds = drv->Create(path_.c_str(), static_cast<int>(w),
                                      static_cast<int>(h), outBands_, GDT_Byte, opts);
        CSLDestroy(opts);
        if (!ds) {
            err = "创建包失败: " + path_ + "（" + CPLGetLastErrorMsg() + "）";
            return false;
        }
        setFullGrid(ds, gridIdx);
        if (isMbtiles_) {
            // 记录层级以初始化 tiles 表；后续按 ZOOM_LEVEL 重开逐层写入
            ds->SetMetadataItem("ZOOM_LEVEL", std::to_string(gridIdx).c_str());
        }
        GDALClose(ds);
        ip::logInfo("打包：新建 " + std::string(isMbtiles_ ? "MBTiles" : "GPKG") +
                    " " + path_ + "（层级 " + std::to_string(tms_.dirLevel(firstIdx_)) +
                    "~" + std::to_string(tms_.dirLevel(lastIdx_)) + "，块 " +
                    std::to_string(tileSize_) + "）");
        return true;
    }

    // 满网格 geotransform + 投影：块索引即瓦片编号
    void setFullGrid(GDALDataset* ds, int idx) {
        const TileMatrixLevel& lv = tms_.level(idx);
        double gt[6] = {lv.topLeftX, lv.res, 0.0, lv.topLeftY, 0.0, -lv.res};
        ds->SetGeoTransform(gt);
        OGRSpatialReference srs = ip::makeSrsFromEpsg(tms_.epsg);
        srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        char* wkt = nullptr;
        srs.exportToWkt(&wkt);
        ds->SetProjection(wkt);
        CPLFree(wkt);
    }

    bool ensureHandle(int idx, std::string& err) {
        if (ds_ != nullptr && curIdx_ == idx) return true;
        closeHandle();
        char** opts = nullptr;
        const char* allow[2] = {nullptr, nullptr};
        allow[0] = isMbtiles_ ? "MBTiles" : "GPKG";
        opts = CSLSetNameValue(opts, "ZOOM_LEVEL", std::to_string(idx).c_str());
        if (!isMbtiles_) opts = CSLSetNameValue(opts, "TABLE", "tiles");
        CPLErrorReset();
        // 注意实参顺序：GDALOpenEx(file, flags, allowedDrivers, openOptions, siblingFiles)
        ds_ = static_cast<GDALDataset*>(GDALOpenEx(path_.c_str(),
                                                   GDAL_OF_RASTER | GDAL_OF_UPDATE,
                                                   allow, opts, nullptr));
        CSLDestroy(opts);
        if (!ds_) {
            err = "打开包层级失败 " + std::to_string(tms_.dirLevel(idx)) + ": " +
                  path_ + "（" + CPLGetLastErrorMsg() + "）";
            return false;
        }
        curIdx_ = idx;
        maxIdxOpened_ = std::max(maxIdxOpened_, idx);
        return true;
    }

    void closeHandle() {
        if (ds_ != nullptr) {
            GDALClose(ds_);
            ds_ = nullptr;
            curIdx_ = -1;
        }
    }

    // MBTiles 元数据修正：驱动按「当次句柄层级」写 minzoom/maxzoom，与包内
    // 实际内容不符；这里读旧值 → 与本次范围取并集 → 覆写
    bool fixMbtilesZoomMeta(std::string& err) {
        const char* allow[2] = {"SQLite", nullptr};
        GDALDataset* db = static_cast<GDALDataset*>(GDALOpenEx(
            path_.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE, allow, nullptr, nullptr));
        if (!db) {
            ip::logWarn("MBTiles 元数据修正跳过（无法以 SQLite 打开）: " +
                        std::string(CPLGetLastErrorMsg()));
            return true;
        }
        int lo = firstIdx_, hi = lastIdx_;
        OGRLayer* l = db->ExecuteSQL(
            "SELECT name,value FROM metadata WHERE name IN ('minzoom','maxzoom')",
            nullptr, nullptr);
        if (l) {
            OGRFeature* f = nullptr;
            while ((f = l->GetNextFeature()) != nullptr) {
                const std::string n = f->GetFieldAsString(0);
                const int v = std::atoi(f->GetFieldAsString(1));
                if (n == "minzoom") lo = std::min(lo, v);
                if (n == "maxzoom") hi = std::max(hi, v);
                OGRFeature::DestroyFeature(f);
            }
            db->ReleaseResultSet(l);
        }
        // OGR 的 ExecuteSQL 一次只执行一条语句，必须分开发送
        CPLErrorReset();
        OGRLayer* r1 = db->ExecuteSQL(
            "DELETE FROM metadata WHERE name IN ('minzoom','maxzoom')", nullptr,
            nullptr);
        if (r1) db->ReleaseResultSet(r1);
        const std::string ins =
            "INSERT INTO metadata(name,value) VALUES('minzoom','" +
            std::to_string(lo) + "'),('maxzoom','" + std::to_string(hi) + "')";
        OGRLayer* r2 = db->ExecuteSQL(ins.c_str(), nullptr, nullptr);
        if (r2) db->ReleaseResultSet(r2);
        const char* msg = CPLGetLastErrorMsg();
        if (msg != nullptr && msg[0] != '\0') {
            // 非致命：包已可用，仅提示
            ip::logWarn("MBTiles 元数据修正告警: " + std::string(msg));
        }
        GDALClose(db);
        ip::logInfo("MBTiles 元数据: minzoom=" + std::to_string(lo) + " maxzoom=" +
                    std::to_string(hi));
        return true;
    }

    TileMatrixSet tms_;
    std::string path_;
    std::string tmsJsonPath_;
    std::string format_;
    bool isMbtiles_ = false;
    int firstIdx_ = 0;
    int lastIdx_ = 0;
    int outBands_ = 3;
    int tileSize_ = 256;
    bool overwrite_ = false;
    int jpegQuality_ = 85;
    int webpQuality_ = 85;

    std::mutex mu_;
    GDALDataset* ds_ = nullptr;
    int curIdx_ = -1;
    int maxIdxOpened_ = -1;
};

} // namespace

bool writeTmsJsonFile(const TileMatrixSet& tms, int tileSize,
                      const std::string& path, std::string& err) {
    // 轴序：EPSG:4326/4490 权威轴序为 lat-first（写 [lat,lon]），其余写 [x,y]
    const bool latFirst = (tms.epsg == 4326 || tms.epsg == 4490);
    std::string s;
    s += "{\n";
    s += "  \"type\": \"TileMatrixSetType\",\n";
    s += "  \"identifier\": \"" + tms.id + "\",\n";
    s += "  \"supportedCRS\": \"http://www.opengis.net/def/crs/EPSG/0/" +
         std::to_string(tms.epsg) + "\",\n";
    s += "  \"tileMatrix\": [\n";
    for (size_t i = 0; i < tms.levels.size(); ++i) {
        const TileMatrixLevel& lv = tms.levels[i];
        const double a = latFirst ? lv.topLeftY : lv.topLeftX;
        const double b = latFirst ? lv.topLeftX : lv.topLeftY;
        s += "    {\"type\": \"TileMatrixType\", \"identifier\": \"" +
             std::to_string(tms.dirLevel(static_cast<int>(i))) + "\", ";
        s += "\"scaleDenominator\": " +
             trimNum(scaleDenominator(lv, tms.geographic)) + ", ";
        s += "\"topLeftCorner\": [" + trimNum(a) + ", " + trimNum(b) + "], ";
        s += "\"tileWidth\": " + std::to_string(tileSize) +
             ", \"tileHeight\": " + std::to_string(tileSize) + ", ";
        s += "\"matrixWidth\": " + std::to_string(lv.matrixW) +
             ", \"matrixHeight\": " + std::to_string(lv.matrixH) + "}";
        if (i + 1 < tms.levels.size()) s += ",";
        s += "\n";
    }
    s += "  ]\n}\n";

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        err = "无法写出 TMS JSON: " + path;
        return false;
    }
    out << s;
    out.close();
    if (!out) {
        err = "写出 TMS JSON 失败: " + path;
        return false;
    }
    return true;
}

TileSink* createPackageSink(const TilingParams& params, const TileMatrixSet& tms,
                            int firstIdx, int lastIdx, int outBands,
                            int tileSize, std::string& err) {
    PackageSink* sink =
        new (std::nothrow) PackageSink(params, tms, firstIdx, lastIdx, outBands,
                                       tileSize);
    if (sink == nullptr) {
        err = "内存不足，无法创建打包 sink";
        return nullptr;
    }
    // GPKG 需要 TILING_SCHEME JSON（由当前 TileMatrixSet 现场导出）
    std::string jsonPath;
    if (params.package == "gpkg") {
        jsonPath = (fs::path(params.output_dir) / "_tmp" / "tiling_scheme.json")
                       .string();
        if (!writeTmsJsonFile(tms, tileSize, jsonPath, err)) {
            delete sink;
            return nullptr;
        }
    }
    if (!sink->init(jsonPath, err)) {
        delete sink;
        return nullptr;
    }
    return sink;
}

} // namespace iptl
