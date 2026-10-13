// TileMatrixSet：三内置方案 + OGC TMS JSON 解析 + 瓦片几何计算
#include "tms.h"

#include "ip/json_io.h"
#include "ip/log.h"

#include <cmath>

namespace iptl {

namespace {
constexpr double OGC_PIXEL_M = 0.28e-3;              // OGC 像元米/像素
constexpr double M_PER_DEG = 111319.49079327358;     // WGS84 赤道 米/度
constexpr double ORIGIN_M = 20037508.3427892476;     // WebMercator 半周长
} // namespace

TileMatrixSet makeWebMercatorQuad(int maxLevel) {
    TileMatrixSet t;
    t.id = "WebMercatorQuad";
    t.epsg = 3857;
    t.geographic = false;
    t.idOffset = 0;
    t.maxLevel = maxLevel;
    for (int z = 0; z < maxLevel; ++z) {
        TileMatrixLevel m;
        m.res = ORIGIN_M * 2 / 256.0 / (1 << z);
        m.topLeftX = -ORIGIN_M;
        m.topLeftY = ORIGIN_M;
        m.matrixW = 1LL << z;
        m.matrixH = 1LL << z;
        t.levels.push_back(m);
    }
    return t;
}

TileMatrixSet makeWorldCRS84Quad(int maxLevel) {
    TileMatrixSet t;
    t.id = "WorldCRS84Quad";
    t.epsg = 4326;
    t.geographic = true;
    t.idOffset = 0;
    t.maxLevel = maxLevel;
    for (int z = 0; z < maxLevel; ++z) {
        TileMatrixLevel m;
        m.res = 0.703125 / (1 << z);
        m.topLeftX = -180.0;
        m.topLeftY = 90.0;
        m.matrixW = 2LL << z;
        m.matrixH = 1LL << z;
        t.levels.push_back(m);
    }
    return t;
}

TileMatrixSet makeCGCS2000(int maxLevel) {
    TileMatrixSet t = makeWorldCRS84Quad(maxLevel);
    t.id = "CGCS2000";
    t.epsg = 4490;
    t.idOffset = 1;  // 天地图层级从 1 起（z1 全球 2x1）
    return t;
}

bool parseTmsJson(const std::string& jsonPath, TileMatrixSet& tms,
                  std::string& err) {
    ip::Json j;
    if (!ip::readJsonFile(jsonPath, j, err)) return false;
    try {
        tms = TileMatrixSet{};
        tms.id = j.value("identifier", std::string("custom"));
        const std::string crs = j.value("supportedCRS", std::string(""));
        // 支持 ".../EPSG/0/4490" 与 "EPSG:4490" 两种写法
        const std::string digits = crs.size() >= 4
                                       ? crs.substr(crs.size() - 4)
                                       : std::string();
        if (digits.find_first_not_of("0123456789") == std::string::npos) {
            tms.epsg = std::atoi(digits.c_str());
        } else {
            err = "无法解析 supportedCRS: " + crs;
            return false;
        }
        tms.geographic = (tms.epsg == 4326 || tms.epsg == 4490);
        const auto& mats = j.at("tileMatrix");
        for (const auto& m : mats) {
            TileMatrixLevel lv;
            const double scale = m.at("scaleDenominator").get<double>();
            double span = scale * OGC_PIXEL_M;  // 米
            if (tms.geographic) span /= M_PER_DEG;  // → 度
            lv.res = span;
            const auto& tl = m.at("topLeftCorner");
            lv.topLeftX = tl.at(0).get<double>();
            lv.topLeftY = tl.at(1).get<double>();
            lv.matrixW = m.at("matrixWidth").get<long long>();
            lv.matrixH = m.at("matrixHeight").get<long long>();
            tms.levels.push_back(lv);
        }
        if (tms.levels.empty()) {
            err = "tileMatrix 为空: " + jsonPath;
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        err = std::string("TMS JSON 解析失败 ") + jsonPath + ": " + e.what();
        return false;
    }
}

bool resolveTms(const std::string& nameOrPath, TileMatrixSet& tms,
                std::string& err) {
    if (nameOrPath == "WebMercatorQuad") {
        tms = makeWebMercatorQuad();
        return true;
    }
    if (nameOrPath == "WorldCRS84Quad") {
        tms = makeWorldCRS84Quad();
        return true;
    }
    if (nameOrPath == "CGCS2000") {
        tms = makeCGCS2000();
        return true;
    }
    return parseTmsJson(nameOrPath, tms, err);
}

TileBbox tileBbox(const TileMatrixSet& tms, int levelIdx, long long tx,
                  long long tyXYZ, int tileW, int tileH) {
    const TileMatrixLevel& lv = tms.level(levelIdx);
    TileBbox b;
    b.minX = lv.topLeftX + tx * lv.res * tileW;
    b.maxY = lv.topLeftY - tyXYZ * lv.res * tileH;
    b.maxX = b.minX + lv.res * tileW;
    b.minY = b.maxY - lv.res * tileH;
    return b;
}

bool levelTileRange(const TileMatrixSet& tms, int levelIdx, int tileW,
                    int tileH, double minX, double minY, double maxX,
                    double maxY, long long& tx0, long long& ty0, long long& tx1,
                    long long& ty1) {
    const TileMatrixLevel& lv = tms.level(levelIdx);
    const double spanX = lv.res * tileW;
    const double spanY = lv.res * tileH;
    const double fx0 = (minX - lv.topLeftX) / spanX;
    const double fy0 = (lv.topLeftY - maxY) / spanY;
    const double fx1 = (maxX - lv.topLeftX) / spanX;
    const double fy1 = (lv.topLeftY - minY) / spanY;
    tx0 = static_cast<long long>(std::floor(fx0));
    ty0 = static_cast<long long>(std::floor(fy0));
    tx1 = static_cast<long long>(std::ceil(fx1)) - 1;
    ty1 = static_cast<long long>(std::ceil(fy1)) - 1;
    tx0 = std::max<long long>(tx0, 0);
    ty0 = std::max<long long>(ty0, 0);
    tx1 = std::min<long long>(tx1, lv.matrixW - 1);
    ty1 = std::min<long long>(ty1, lv.matrixH - 1);
    return tx0 <= tx1 && ty0 <= ty1;
}

} // namespace iptl
