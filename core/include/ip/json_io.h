#pragma once
// JSON 读写（nlohmann）。输出统一 UTF-8 + LF（项目规范）
#include <nlohmann/json.hpp>

#include <string>

namespace ip {

using Json = nlohmann::json;

// 写 JSON 到文件（dump(2)，LF）；父目录不存在则创建
bool writeJsonFile(const std::string& path, const Json& j, std::string& err);

// 读 JSON 文件；失败返回 false 并填充 err
bool readJsonFile(const std::string& path, Json& j, std::string& err);

} // namespace ip
