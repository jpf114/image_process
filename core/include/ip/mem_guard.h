#pragma once
// 内存护栏：本机/服务器内存受限环境（Phase 0 教训：并行全分辨率负载曾致 WSL OOM）
#include <string>

namespace ip {

class MemGuard {
public:
    // 当前可用内存（GB），读取失败返回 -1
    static double availableGB();

    // 可用内存低于 minGB 时抛 std::runtime_error（含 tag 与当前值）
    static void checkOrThrow(double minGB, const std::string& tag);
};

} // namespace ip
