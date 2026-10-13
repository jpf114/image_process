#include "ip/json_io.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ip {

bool writeJsonFile(const std::string& path, const Json& j, std::string& err) {
    namespace fs = std::filesystem;
    try {
        const fs::path p(path);
        if (p.has_parent_path()) {
            fs::create_directories(p.parent_path());
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "无法打开文件写入: " + path;
            return false;
        }
        out << j.dump(2) << "\n";
        out.flush();
        if (!out.good()) {
            err = "写入失败: " + path;
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        err = std::string("writeJsonFile 异常: ") + e.what();
        return false;
    }
}

bool readJsonFile(const std::string& path, Json& j, std::string& err) {
    try {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            err = "无法打开文件读取: " + path;
            return false;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        j = Json::parse(ss.str(), nullptr, true, true);
        return true;
    } catch (const std::exception& e) {
        err = std::string("readJsonFile 解析失败 ") + path + ": " + e.what();
        return false;
    }
}

} // namespace ip
