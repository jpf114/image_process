// 多景（目录输入）按景并行实现（见 scene_fanout.h）
#include "scene_fanout.h"

#include "ip/json_io.h"
#include "ip/log.h"
#include "tiling/tiling_params_json.h"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <thread>

extern char** environ;

namespace fs = std::filesystem;

namespace ipapp {

namespace {

bool isTiffName(const std::string& name) {
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) {
        lower.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    for (const char* e : {".tif", ".tiff"}) {
        const size_t len = std::strlen(e);
        if (lower.size() > len &&
            lower.compare(lower.size() - len, len, e) == 0) {
            return true;
        }
    }
    return false;
}

// 当前可执行文件所在目录（用于定位同目录的 workflow 镜像）
std::string selfDir() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return std::string();
    buf[n] = '\0';
    return fs::path(buf).parent_path().string();
}

// 每景输出目录名：场景文件 stem；同名（不同子目录下的同名文件）加 _2/_3 去重
std::vector<std::string> sceneDirNames(const std::vector<std::string>& scenes) {
    std::vector<std::string> names(scenes.size());
    std::vector<std::string> used;
    for (size_t i = 0; i < scenes.size(); ++i) {
        const std::string stem = fs::path(scenes[i]).stem().string();
        std::string name = stem;
        int n = 1;
        while (std::find(used.begin(), used.end(), name) != used.end()) {
            name = stem + "_" + std::to_string(++n);
        }
        used.push_back(name);
        names[i] = name;
    }
    return names;
}

// 单景参数：覆盖 input/output_dir，num_processes 归 1（每次执行都是单输入）
iptl::TilingParams sceneParams(const iptl::TilingParams& base,
                               const std::string& scene,
                               const std::string& outDir, int threads) {
    iptl::TilingParams p = base;
    p.input = scene;
    p.output_dir = outDir;
    p.num_processes = 1;
    p.num_threads = threads;
    return p;
}

bool spawnChild(const std::string& exe, const std::vector<std::string>& args,
                pid_t& pid, std::string& err) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    // 不设独立进程组：Ctrl-C 送达父进程所在进程组时可一并终止子进程；
    // 且 GDAL 要求"任何 GDAL 操作之前 fork"——这里是全新 exec，父进程此时未做 GDAL 操作
    const int rc =
        posix_spawn(&pid, exe.c_str(), &fa, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        err = "派生子进程失败（posix_spawn rc=" + std::to_string(rc) + "）: " + exe;
        return false;
    }
    return true;
}

} // namespace

bool collectScenes(const std::string& input, std::vector<std::string>& scenes,
                   std::string& err) {
    std::error_code ec;
    if (!fs::is_directory(input, ec)) {
        err = "输入不是目录: " + input;
        return false;
    }
    for (const auto& entry : fs::recursive_directory_iterator(input, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string fname = entry.path().filename().string();
        if (fname.find(".aux.xml") != std::string::npos) continue;  // GDAL 辅助文件
        if (isTiffName(fname)) scenes.push_back(entry.path().string());
    }
    if (ec) {
        err = "遍历输入目录失败: " + input + "（" + ec.message() + "）";
        return false;
    }
    std::sort(scenes.begin(), scenes.end());
    if (scenes.empty()) {
        err = "目录中未找到影像（.tif/.tiff）: " + input;
        return false;
    }
    return true;
}

bool runScenesFanout(const iptl::TilingParams& base,
                     const std::vector<std::string>& scenes, FanoutStats& st,
                     std::string& err) {
    st = FanoutStats{};
    st.scenes = static_cast<int>(scenes.size());
    const std::vector<std::string> names = sceneDirNames(scenes);
    std::vector<std::string> outs(scenes.size());
    for (size_t i = 0; i < scenes.size(); ++i) {
        outs[i] = (fs::path(base.output_dir) / names[i]).string();
    }
    const int procs = std::max(1, base.num_processes);
    ip::logInfo("按景切片：目录 " + base.input + " 共 " + std::to_string(scenes.size()) +
                " 景，进程数 " + std::to_string(procs) +
                (procs > 1 ? "（每景子进程）" : "（本进程逐景串行）"));
    // 多进程时每景线程数：显式 num_threads 则沿用，否则按 核数/进程数 分摊（避免超订）
    const int childThreads =
        base.num_threads > 0
            ? base.num_threads
            : std::max(1, static_cast<int>(std::thread::hardware_concurrency()) /
                              std::max(1, procs));
    if (procs > 1) {
        ip::logInfo("每景线程数 " + std::to_string(childThreads) + "（核数 " +
                    std::to_string(std::thread::hardware_concurrency()) + " / 进程数 " +
                    std::to_string(procs) + "）");
    }

    // ---------- 单进程：本进程逐景串行（无派生开销；也作为多进程的回退路径）----------
    if (procs <= 1) {
        for (size_t i = 0; i < scenes.size(); ++i) {
            ip::logInfo("[景 " + std::to_string(i + 1) + "/" +
                        std::to_string(scenes.size()) + "] " + names[i]);
            iptl::ITiling* op = createTiling();
            if (op == nullptr) {
                err = "创建切片 operator 失败";
                return false;
            }
            // 串行逐景：沿用用户显式线程数（未指定=0 视为自动，与单输入路径一致）
            const iptl::TilingParams p =
                sceneParams(base, scenes[i], outs[i], base.num_threads);
            std::string e;
            bool ok = op->configure(p, e);
            if (ok) {
                iptl::TilingStats sc;
                ok = op->execute(nullptr, sc, e);
            }
            destroyTiling(op);
            if (ok) {
                ++st.done;
            } else {
                ++st.failed;
                ip::logWarn("景失败（继续其余景）: " + names[i] + " — " + e);
                err = e;
            }
        }
        if (st.failed > 0) {
            err = std::to_string(st.failed) + "/" + std::to_string(st.scenes) +
                  " 景失败（首条错误: " + err + "）";
            return false;
        }
        return true;
    }

    // ---------- 多进程：派生 workflow --only tiling 子进程池 ----------
    const std::string dir = selfDir();
    const std::string childExe =
        dir.empty() ? std::string() : (fs::path(dir) / "workflow").string();
    std::error_code ec;
    if (childExe.empty() || !fs::is_regular_file(childExe, ec)) {
        err = "多进程模式需要与 tiling 同目录的 workflow 可执行文件，未找到: " +
              childExe;
        return false;
    }
    // 每景子任务 JSON（与 workflow 配置同构）
    const std::string tmpDir = (fs::path(base.output_dir) / "_fanout").string();
    fs::create_directories(tmpDir, ec);
    if (ec) {
        err = "无法创建多进程临时目录: " + tmpDir + "（" + ec.message() + "）";
        return false;
    }
    std::vector<std::string> cfgs(scenes.size());
    for (size_t i = 0; i < scenes.size(); ++i) {
        const iptl::TilingParams p = sceneParams(base, scenes[i], outs[i], childThreads);
        ip::Json t;
        iptl::tilingParamsToJson(p, t);
        ip::Json doc;
        doc["tiling"] = t;
        cfgs[i] = (fs::path(tmpDir) / ("scene_" + std::to_string(i) + ".json")).string();
        std::string e;
        if (!ip::writeJsonFile(cfgs[i], doc, e)) {
            err = e;
            return false;
        }
    }

    // 有界并发：始终维持最多 procs 个子进程在跑
    struct Running {
        pid_t pid;
        size_t idx;
    };
    std::vector<Running> running;
    running.reserve(static_cast<size_t>(procs));
    // 异常退出（派生失败 / waitpid 真错误）时回收在跑的子进程，避免留下孤儿进程
    auto abortReap = [&](const std::string& msg) {
        for (const auto& r : running) ::kill(r.pid, SIGTERM);
        for (const auto& r : running) {
            int s = 0;
            ::waitpid(r.pid, &s, 0);
        }
        running.clear();
        err = msg;
        return false;
    };
    size_t next = 0;
    int firstFailIdx = -1;
    std::string firstFailMsg;
    while (next < scenes.size() || !running.empty()) {
        while (next < scenes.size() && running.size() < static_cast<size_t>(procs)) {
            const size_t i = next;
            pid_t pid = -1;
            std::string e;
            if (!spawnChild(childExe, {"--config", cfgs[i], "--only", "tiling"}, pid, e)) {
                return abortReap(e);
            }
            ip::logInfo("[景 " + std::to_string(i + 1) + "/" +
                        std::to_string(scenes.size()) + "] pid " + std::to_string(pid) +
                        " " + names[i]);
            running.push_back({pid, i});
            ++next;
        }
        int status = 0;
        pid_t done = -1;
        do {
            done = ::waitpid(-1, &status, 0);
        } while (done < 0 && errno == EINTR);  // 信号打断则重试
        if (done < 0) {
            return abortReap("等待子进程失败（waitpid errno=" +
                             std::to_string(errno) + "）");
        }
        size_t k = 0;
        while (k < running.size() && running[k].pid != done) ++k;
        if (k == running.size()) continue;  // 非本批次（不应发生）
        const size_t idx = running[k].idx;
        running.erase(running.begin() + static_cast<long>(k));
        const bool okRun = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (okRun) {
            ++st.done;
        } else {
            ++st.failed;
            if (firstFailIdx < 0) {
                firstFailIdx = static_cast<int>(idx);
                if (WIFSIGNALED(status)) {
                    firstFailMsg = "信号 " + std::to_string(WTERMSIG(status)) + " 终止";
                } else {
                    firstFailMsg = "退出码 " + std::to_string(WEXITSTATUS(status));
                }
            }
            ip::logWarn("景失败（继续其余景）: " + names[idx] + " — " +
                        (WIFSIGNALED(status)
                             ? "信号 " + std::to_string(WTERMSIG(status)) + " 终止"
                             : "退出码 " + std::to_string(WEXITSTATUS(status))));
        }
    }

    if (!base.keep_tmp_vrt) {
        std::error_code ec2;
        fs::remove_all(tmpDir, ec2);
    }
    if (st.failed > 0) {
        err = std::to_string(st.failed) + "/" + std::to_string(st.scenes) +
              " 景失败（首个失败 " + names[static_cast<size_t>(firstFailIdx)] + "：" +
              firstFailMsg + "）";
        return false;
    }
    return true;
}

} // namespace ipapp
