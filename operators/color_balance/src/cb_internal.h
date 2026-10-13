#pragma once
// color_balance 内部共享类型（仅 operator 实现使用）
#include "color_balance/i_color_balance.h"

#include "ip/block_stats.h"
#include "ip/gdal_raii.h"
#include "ip/json_io.h"

#include <map>
#include <string>
#include <vector>

namespace ipcb {

struct Scene {
    std::string path;
    std::string id;          // 无扩展名文件名
    ip::DataSetPtr ds;
    int w = 0, h = 0;
    double gt[6] = {0, 1, 0, 0, 0, 1};
    std::string crs;         // authority code，如 "4490"；空 = 未声明
    ip::GeoExtent extent;
    ip::BandStats3 stats;
    double gain[3] = {1, 1, 1};
    double offset[3] = {0, 0, 0};
    bool adjusted = false;   // 参数非恒等
    std::string note;
};

struct OverlapEdge {
    int a = -1, b = -1;      // scene 下标
    ip::GeoExtent bounds;    // 地理交集
    ip::BandStats3 statsA, statsB;
    double deltaMu[3] = {0, 0, 0};
    double deltaMuMax = 0;
    bool skipped = false;        // 一致性门控跳过
    bool usedForSolve = false;   // 阶段 2 中实际用于解算参数
    double residualAfter[3] = {0, 0, 0};  // 解算后该边重叠区预测残差
};

// 方案文件快照（跨编译单元传递）
struct SceneSnapshot {
    std::string id, path;
    double mean[3] = {0, 0, 0};
    double std[3] = {0, 0, 0};
    double gain[3] = {1, 1, 1};
    double offset[3] = {0, 0, 0};
    bool adjusted = false;
    std::string note;
    std::string outputPath;
};
struct EdgeSnapshot {
    std::string idA, idB;
    double deltaMu[3] = {0, 0, 0};
    double deltaMuMax = 0;
    bool skipped = false;
    bool usedForSolve = false;
    double residualAfter[3] = {0, 0, 0};  // 解算后该边重叠区预测残差（A 调节后 - B 调节后）
};

// ---- 最小二乘图平差（迭代 2）----
// 稀疏线性方程：Σ coeff[i]·x[i] = rhs，权重 weight
struct LsEquation {
    std::map<int, double> coeffs;
    double rhs = 0.0;
    double weight = 1.0;
};

// 解 min Σ w·(Σ a_ij x_j - b_i)^2（法方程 + 部分主元高斯消元）。
// 仅求解"与 refVar 连通"的变量（在方程图上 BFS）；其余变量置 0 并返回 false 表示部分无解。
// 返回 false 时 out 中未连通变量为 0，调用方需自行按连通性处理。
bool solveLeastSquares(const std::vector<LsEquation>& eqs, int nVars,
                       int refVar, std::vector<double>& out);

// 块式应用 gain/offset 到新 GeoTIFF（保持地理信息/alpha/nodata；无效像素原样）
// sceneFormat: "gtiff" | "cog"（COG：先写临时 GTiff 再 COG CreateCopy）
bool applyGainOffsetToFile(GDALDataset* src, const std::string& outPath,
                           const double gain[3], const double offset[3],
                           int chunkRows, std::string& err,
                           const char* sceneFormat = "gtiff");

// ---- 迭代 2：Wallis / 直方图匹配 ----

// Wallis 分块参数场（逐波段 a/b，块中心语义），双线性插值后逐像元应用
struct WallisGrid {
    int nbx = 0, nby = 0, block = 512;
    std::vector<float> a;  // 3 * nby * nbx
    std::vector<float> b;  // 3 * nby * nbx
};

// 计算 Wallis 参数网格：块直方图统计（块式读，恒定内存）→
// 先行景级变换 μ'=g·μ+o, σ'=g·σ（组合模式用；单独 wallis 传 1/0）→
// a = clip(targetStd/σ', 0.4, 2.5)，b = targetMean - a·μ'；
// 有效像素 <20% 的块用列均值填充，兜底 1/0
WallisGrid computeWallisGrid(GDALDataset* ds, const double targetMean[3],
                             const double targetStd[3], int block,
                             const double sceneGain[3],
                             const double sceneOffset[3]);

// 块式应用 Wallis 参数场（双线性采样，参数值位于块中心 (i+0.5)*block）
bool applyWallisToFile(GDALDataset* src, const std::string& outPath,
                       const WallisGrid& wg, int chunkRows, std::string& err,
                       const char* sceneFormat = "gtiff");

// 直方图匹配：CDF 匹配 LUT（src 分布 → ref 分布）
void exactHistograms(GDALDataset* ds, uint64_t hist[3][256]);
void cdfMatchLut(const uint64_t srcHist[256], const uint64_t refHist[256],
                 uint8_t lut[256]);
bool applyLutToFile(GDALDataset* src, const std::string& outPath,
                    const uint8_t lut[3][256], int chunkRows, std::string& err,
                    const char* sceneFormat = "gtiff");

// ---- 迭代 2：VRT 输出（零像素落盘）----

// 注册匀色像素函数（进程内一次）："ip_gain_offset"，参数经 <PixelFunctionArguments>
void ensurePixelFuncRegistered();

// 每景"匀色 VRT"：VRTDerivedRasterBand + ip_gain_offset + 引用原始影像；alpha 波段直拷
bool writeBalancedVrt(GDALDataset* src, const std::string& vrtPath,
                      const double gain[3], const double offset[3],
                      std::string& err);

// 自检：VRT 样本块与手工应用参数对比（±1 舍入容差）
bool verifyBalancedVrt(GDALDataset* src, const std::string& vrtPath,
                       const double gain[3], const double offset[3],
                       std::string& err);

// 镶嵌 VRT：多景按地理足迹拼装（ComplexSource 内联匀色参数 ScaleRatio/ScaleOffset，
// 直接引用原始影像——避免嵌套派生波段的 decim 缺陷，完全对齐 gdalbuildvrt 成熟路径）
struct VrtSource {
    std::string path;  // 原始影像路径
    double minX = 0, minY = 0, maxX = 0, maxY = 0;
    double gain[3] = {1, 1, 1};
    double offset[3] = {0, 0, 0};
    int rasterX = 0, rasterY = 0;  // 源尺寸（SourceProperties）
};
bool writeMosaicVrt(const std::vector<VrtSource>& sources, int bands,
                    const std::string& vrtPath, std::string& err);

// 构建方案 JSON（结构见技术方案 4.4）
ip::Json buildProfileJson(const std::string& method, const std::string& refId,
                          const std::string& refPath,
                          const ip::BandStats3& refStats,
                          const std::vector<SceneSnapshot>& scenes,
                          const std::vector<EdgeSnapshot>& edges,
                          double seamThreshold);

} // namespace ipcb
