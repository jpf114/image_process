// color_balance CLI（薄壳：解析参数 → 调 operator）
#include "color_balance/i_color_balance.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

void usage() {
    std::cout
        << "用法: color_balance --input <文件|目录> [--input ...] --output-dir <目录>\n"
           "  [选项]\n"
           "  --method <overlap_adjust|overlap_adjust+wallis|block_wallis|hist_match|global_adjust>\n"
           "                                           默认 overlap_adjust\n"
           "  --output-mode <tif|vrt>                  vrt=匀色 VRT+镶嵌 VRT（零像素落盘）\n"
           "  --scene-format <gtiff|cog>               每景物理输出布局（默认 gtiff）\n"
           "  --mosaic-cog                             vrt 模式下额外输出物理镶嵌 COG\n"
           "  --ref-image <路径>                       参考影像（默认排序后第一景）\n"
           "  --seam-threshold <2.0>                   接边一致性阈值（|Δμ| 低于则跳过调节）\n"
           "  --min-overlap-px <8>                     重叠窗口最小边长（像素）\n"
           "  --overlap-max-side <2048>                重叠区统计采样上限（tile 采样，控内存）\n"
           "  --chunk <2048>                           应用阶段行块高\n"
           "  --block-size <512>                       Wallis 分块大小\n"
           "  --wallis-target-std <-1|值>              Wallis 目标 σ（-1=参考景统计）\n"
           "  --use-gpu                                GPU 占位（本期纯 CPU，仅告警）\n"
           "  --profile <路径>                         方案 JSON 输出路径（默认 <out>/_profile.json）\n"
           "  --no-profile                             不写方案文件\n"
           "  --min-mem-gb <1.5>                       内存护栏阈值\n"
           "  -h | --help\n";
}

} // namespace

int main(int argc, char** argv) {
    ipcb::ColorBalanceParams p;
    std::vector<std::string> inputs;
    bool noProfile = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "缺少参数值: " << name << std::endl;
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a == "--input") {
            inputs.emplace_back(need("--input"));
        } else if (a == "--output-dir") {
            p.output_dir = need("--output-dir");
        } else if (a == "--method") {
            p.method = need("--method");
        } else if (a == "--ref-image") {
            p.ref_image = need("--ref-image");
        } else if (a == "--seam-threshold") {
            p.seam_check_threshold = std::atof(need("--seam-threshold"));
        } else if (a == "--min-overlap-px") {
            p.min_overlap_px = std::atoi(need("--min-overlap-px"));
        } else if (a == "--overlap-max-side") {
            p.overlap_max_side = std::atoi(need("--overlap-max-side"));
        } else if (a == "--chunk") {
            p.apply_chunk = std::atoi(need("--chunk"));
        } else if (a == "--block-size") {
            p.block_size = std::atoi(need("--block-size"));
        } else if (a == "--wallis-target-std") {
            p.wallis_target_std = std::atof(need("--wallis-target-std"));
        } else if (a == "--use-gpu") {
            p.use_gpu = true;
        } else if (a == "--output-mode") {
            p.output_mode = need("--output-mode");
        } else if (a == "--scene-format") {
            p.scene_format = need("--scene-format");
        } else if (a == "--mosaic-cog") {
            p.mosaic_cog = true;
        } else if (a == "--profile") {
            p.profile_path = need("--profile");
        } else if (a == "--no-profile") {
            noProfile = true;
        } else if (a == "--min-mem-gb") {
            p.min_mem_gb = std::atof(need("--min-mem-gb"));
        } else {
            std::cerr << "未知参数: " << a << std::endl;
            usage();
            return 2;
        }
    }
    p.inputs = inputs;
    p.write_profile = !noProfile;

    std::string err;
    ipcb::IColorBalance* op = createColorBalance();
    if (!op) {
        std::cerr << "创建 operator 失败" << std::endl;
        return 1;
    }
    if (!op->configure(p, err)) {
        std::cerr << "参数错误: " << err << std::endl;
        destroyColorBalance(op);
        return 2;
    }
    const bool ok = op->execute(
        [](int pct, const std::string& msg) {
            (void)pct;
            (void)msg;  // 进度已由 operator 日志输出
        },
        err);
    destroyColorBalance(op);
    if (!ok) {
        std::cerr << "执行失败: " << err << std::endl;
        return 1;
    }
    return 0;
}
