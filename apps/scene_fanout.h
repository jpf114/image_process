#pragma once
// 多景（目录输入）按景并行——apps 层编排（算子库保持单输入语义）。
//
// 语义：input 为目录时枚举其中的影像；num_processes<=1 在本进程内逐景串行执行，
// >1 时派生 num_processes 个子进程（workflow --only tiling）并发按景切片，每景
// 输出到各自的 <output_dir>/<场景名>/ 下（瓦片树或包互不冲突）。
//
// 子进程镜像：与当前可执行文件同目录的 workflow（install/dist 中所有 exe 同目录）；
// 子进程读到的每景参数 JSON 与 workflow 配置同构（同一解析入口，避免行为漂移）。

#include "tiling/i_tiling.h"

#include <string>
#include <vector>

namespace ipapp {

struct FanoutStats {
    int scenes = 0;   // 枚举到的景数
    int done = 0;     // 成功景数
    int failed = 0;   // 失败景数
};

// 枚举目录下的影像（.tif/.tiff，跳过 GDAL 辅助文件，按路径排序）
bool collectScenes(const std::string& input, std::vector<std::string>& scenes,
                   std::string& err);

// 按景切片（base 的 input/output_dir 由本函数按景覆盖）
bool runScenesFanout(const iptl::TilingParams& base,
                     const std::vector<std::string>& scenes, FanoutStats& st,
                     std::string& err);

} // namespace ipapp
