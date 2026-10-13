#include "ip/mem_guard.h"

#include <cstdio>
#include <stdexcept>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fstream>
#endif

namespace ip {

double MemGuard::availableGB() {
#if defined(_WIN32)
    MEMORYSTATUSEX st{};
    st.dwLength = sizeof(st);
    if (!GlobalMemoryStatusEx(&st)) return -1.0;
    return static_cast<double>(st.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
#else
    std::ifstream f("/proc/meminfo");
    std::string key;
    long long kb = -1;
    while (f >> key) {
        if (key == "MemAvailable:") {
            f >> kb;
            break;
        }
        f.ignore(4096, '\n');
    }
    if (kb < 0) return -1.0;
    return static_cast<double>(kb) / (1024.0 * 1024.0);
#endif
}

void MemGuard::checkOrThrow(double minGB, const std::string& tag) {
    double g = availableGB();
    if (g >= 0 && g < minGB) {
        throw std::runtime_error("内存护栏[" + tag + "]: 可用 " +
                                 std::to_string(g) + "GB < 最低要求 " +
                                 std::to_string(minGB) + "GB");
    }
}

} // namespace ip
