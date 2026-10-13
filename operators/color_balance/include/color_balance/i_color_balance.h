#pragma once
// color_balance operator 公共接口（动态库）
//
// 方法（Phase 0 §4 修正后的优先级，详见文档资料/关于栅格切片和匀色/Phase0验证报告.md）：
//  - overlap_adjust（默认，主方法）：重叠区辐射平差 + 接边一致性检查门控
//    重叠对 |Δμ| < seam_check_threshold 的自动跳过调节（本就接边连续的数据不做任何改动）
//    链式解算：以参考景为根（恒等），邻景按重叠区 (μ,σ) 对齐邻景"调节后"统计
//    完全无重叠的景回退 global target（参考景全局 μ/σ）并告警
//  - global_adjust：全局统计目标对齐（仅完全无重叠时使用，有接缝风险）
//  - block_wallis / hist_match：Phase 1 后续迭代实现
//
// 硬性指标规范：评价接缝只能用"接缝局部台阶"与"重叠区 Δμ"，禁止全局均值差当接缝指标。
#include <functional>
#include <string>
#include <vector>

namespace ipcb {

struct ColorBalanceParams {
    // 输入：文件或目录（目录时递归收集 .tif/.tiff/.TIF/.TIFF）
    std::vector<std::string> inputs;
    std::string output_dir;

    std::string method = "overlap_adjust";
    // overlap_adjust：重叠区辐射平差（主方法，含接边一致性门控 + 最小二乘图平差）
    // overlap_adjust+wallis：图平差 + 分块 Wallis 景内精化（物理输出）
    // block_wallis：分块 Wallis（目标=参考景全局统计）
    // hist_match：直方图匹配（回退，全局参考，有接缝风险）
    // global_adjust：全局统计目标（仅完全无重叠时回退）
    std::string ref_image;              // 空 = 排序后第一景
    // 目标统计（方案 §5.1）：直接指定目标 μ/σ，优先级高于 ref_image。
    // 空 = 未指定（用参考景统计）；1 个值 = 三波段统一；3 个值 = 逐波段。
    // 生效范围：global_adjust、block_wallis（Wallis 目标）；hist_match 用参考直方图，忽略此项（告警）。
    std::vector<double> target_mean;
    std::vector<double> target_std;
    // 输出形态（方案 §5.1）：vrt（默认，每景匀色 VRT + 镶嵌 VRT，零像素落盘）
    //   | cog_mosaic（= vrt + 额外物理镶嵌 COG）| per_scene_cog（= 每景 COG 布局）
    //   | tif（每景 GeoTIFF，兼容旧值）
    std::string output_mode = "vrt";
    // 方案模式（方案 §5.1）：create（新建）/ append（新影像按既有方案对齐并追加）/
    //   apply（仅按既有方案处理给定影像，参数冻结，不改方案）/ recreate（全局重算，等价 create 全量）
    std::string profile_mode = "create";
    std::string scene_format = "gtiff"; // 物理每景输出布局：gtiff（DEFLATE tiled）| cog（COG 布局，方案 §4.2 per_scene_cog）
    bool mosaic_cog = false;            // vrt 模式下额外输出物理镶嵌 COG（方案 §4.2 balance_and_mosaic）
    double seam_check_threshold = 2.0;  // 重叠对 |Δμ|（波段最大值）低于此值 → 跳过调节
    int min_overlap_px = 8;             // 重叠窗口最小边长（像素），小于视为无重叠
    int overlap_max_side = 2048;        // 重叠区统计读取最大边（tile 采样上限，控内存）
    int apply_chunk = 2048;             // 应用阶段行块高（块式处理，恒定内存）
    int block_size = 512;               // Wallis 分块大小（像素）
    double wallis_target_std = -1.0;    // Wallis 目标 σ（-1 = 参考景统计；>0 用显式值，μ 仍取参考景）
    bool write_profile = true;          // 是否输出匀色方案 JSON
    std::string profile_path;           // 默认 <output_dir>/_profile.json
    double min_mem_gb = 1.5;            // 内存护栏阈值
    bool use_gpu = false;               // GPU 占位（本期纯 CPU，开启仅告警）
};

using ProgressCallback = std::function<void(int percent, const std::string& msg)>;

class IColorBalance {
public:
    virtual ~IColorBalance() = default;

    // 参数校验（不含 IO 与重活），失败给出可读错误
    virtual bool configure(const ColorBalanceParams& p, std::string& err) = 0;

    // 执行：统计 → 重叠图构建 → 接边检查与平差解算 → 块式应用 → 方案 JSON
    virtual bool execute(const ProgressCallback& cb, std::string& err) = 0;
};

} // namespace ipcb

extern "C" {

ipcb::IColorBalance* createColorBalance();
void destroyColorBalance(ipcb::IColorBalance* op);

// 注册匀色像素函数（"ip_gain_offset"）。同进程内打开匀色 VRT 前必须调用一次
// （VRT 是引用不是拷贝，像素函数按进程注册）。
void ensureColorBalancePixelFunc();

}
