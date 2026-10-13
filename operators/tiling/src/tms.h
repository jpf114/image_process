#pragma once
// TileMatrixSet（OGC TwoD TileMatrixSet）模型：
//  - 三内置方案：WebMercatorQuad(3857) / WorldCRS84Quad(4326) / CGCS2000(4490，天地图层级从 1 起)
//  - 自定义：OGC TMS JSON 文件（注意 4490 等纬度在前 CRS 的 topLeftCorner 须为 [90,-180]）
// 规范：输出目录层级用 identifier = 索引 + idOffset（天地图 idOffset=1）。
#include <string>
#include <vector>

namespace iptl {

struct TileMatrixLevel {
    double res = 0;       // 每像元跨度（3857：米；4326/4490：度）
    double topLeftX = 0;
    double topLeftY = 0;
    long long matrixW = 1;
    long long matrixH = 1;
};

struct TileMatrixSet {
    std::string id;
    int epsg = 3857;
    bool geographic = false;   // 坐标单位为度
    int idOffset = 0;          // 输出目录层级 = 矢量索引 + idOffset
    std::vector<TileMatrixLevel> levels;  // OGC 数组序（索引 0 起）
    int maxLevel = 24;         // 解析的最大层级数

    const TileMatrixLevel& level(int idx) const { return levels[idx]; }
    int levelCount() const { return static_cast<int>(levels.size()); }
    // 输出目录层级号
    int dirLevel(int idx) const { return idx + idOffset; }
};

// 构建三内置方案
TileMatrixSet makeWebMercatorQuad(int maxLevel = 24);
TileMatrixSet makeWorldCRS84Quad(int maxLevel = 24);
TileMatrixSet makeCGCS2000(int maxLevel = 24);  // 天地图规范：z1 全球 2x1

// 解析 OGC TMS JSON；失败返回 false 并填充 err
bool parseTmsJson(const std::string& jsonPath, TileMatrixSet& tms,
                  std::string& err);

// 按名称或 JSON 路径取方案；失败填充 err
bool resolveTms(const std::string& nameOrPath, TileMatrixSet& tms,
                std::string& err);

// xyz 瓦片（y 从上）编号 → 地理 bbox
struct TileBbox {
    double minX = 0, minY = 0, maxX = 0, maxY = 0;
};
TileBbox tileBbox(const TileMatrixSet& tms, int levelIdx, long long tx,
                  long long tyXYZ, int tileW, int tileH);

// 地理范围 → 该级覆盖的 xyz 瓦片范围（含边界）；无交集返回 false
bool levelTileRange(const TileMatrixSet& tms, int levelIdx, int tileW,
                    int tileH, double minX, double minY, double maxX,
                    double maxY, long long& tx0, long long& ty0, long long& tx1,
                    long long& ty1);

} // namespace iptl
