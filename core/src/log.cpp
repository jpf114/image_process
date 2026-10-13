#include "ip/log.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>

namespace ip {

namespace {
std::mutex g_logMutex;
std::FILE* g_logFile = nullptr;
} // namespace

void logSetFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) {
        std::fclose(g_logFile);
        g_logFile = nullptr;
    }
    // 递归创建父目录
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        std::error_code ec;
        std::filesystem::create_directories(path.substr(0, slash), ec);
    }
    g_logFile = std::fopen(path.c_str(), "ab");
}

void logMessage(LogLevel level, const std::string& msg) {
    const char* tag = level == LogLevel::Info ? "INFO" :
                      level == LogLevel::Warn ? "WARN" : "ERROR";
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::time_t t = std::time(nullptr);
    std::tm tmbuf{};
#if defined(_WIN32)
    localtime_s(&tmbuf, &t);
#else
    localtime_r(&t, &tmbuf);
#endif
    char timeBuf[32];
    std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &tmbuf);
    char line[4096];
    const int n = std::snprintf(line, sizeof(line), "[%s %s] %s\n", timeBuf, tag,
                                msg.c_str());
    if (n < 0) return;
    // snprintf 返回"应写长度"（可 > 缓冲区），必须夹取后再写，否则越界读栈
    const size_t len = static_cast<size_t>(n) < sizeof(line)
                           ? static_cast<size_t>(n)
                           : sizeof(line) - 1;
    std::FILE* out = level == LogLevel::Error ? stderr : stdout;
    std::fwrite(line, 1, len, out);
    std::fflush(out);
    if (g_logFile) {
        std::fwrite(line, 1, len, g_logFile);
        std::fflush(g_logFile);
    }
}

} // namespace ip
