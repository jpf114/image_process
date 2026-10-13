#pragma once
// 线程安全日志：INFO/WARN/ERROR 前缀 + 时间戳，输出到 stdout/stderr；
// 可选文件落盘（logSetFile，追加模式）——每次处理的运行日志，避免持续开发无日志。
#include <string>

namespace ip {

enum class LogLevel { Info, Warn, Error };

void logMessage(LogLevel level, const std::string& msg);

// 启用文件日志（追加模式；目录不存在则创建；失败仅回退 stdout）
// 建议：<output_dir>/logs/run_<时间戳>.log
void logSetFile(const std::string& path);

inline void logInfo(const std::string& msg)  { logMessage(LogLevel::Info, msg); }
inline void logWarn(const std::string& msg)  { logMessage(LogLevel::Warn, msg); }
inline void logError(const std::string& msg) { logMessage(LogLevel::Error, msg); }

} // namespace ip
