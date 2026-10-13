// 服务化：任务队列/状态机/worker 进程池实现（posix_spawn + exec，避免线程化进程裸 fork）
#include "job_manager.h"

#include "ip/log.h"
#include "ip/mem_guard.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

extern char** environ;

namespace fs = std::filesystem;

namespace ipsrv {

namespace {

constexpr size_t kMaxJobsInMemory = 2000;  // 内存任务表上限（终态最旧优先驱逐，磁盘状态保留）
constexpr size_t kMaxListItems = 200;      // 列表接口单次返回上限

std::string nowStamp() {
    char ts[32] = {0};
    std::time_t t = std::time(nullptr);
    std::tm tb{};
    localtime_r(&t, &tb);
    std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tb);
    return ts;
}

long long nowSec() { return static_cast<long long>(std::time(nullptr)); }

// 原子写文件：先写 .tmp 再 rename（R6：状态不丢失）
bool writeAtomic(const std::string& path, const std::string& data) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << data;
        f.flush();
        if (!f.good()) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

// 读取文件尾部至多 maxBytes（进度/日志查询，恒定内存）
std::string readTail(const std::string& path, size_t maxBytes) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size <= 0) return "";
    const std::streamoff start =
        size > static_cast<std::streamoff>(maxBytes) ? size - maxBytes : 0;
    f.seekg(start);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// 从 operator 日志的 "[NN%]" 提取最后进度
int parseProgress(const std::string& text) {
    int best = -1;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '[') continue;
        size_t j = i + 1;
        int v = 0;
        bool has = false;
        while (j < text.size() && text[j] >= '0' && text[j] <= '9') {
            v = v * 10 + (text[j] - '0');
            has = true;
            ++j;
        }
        if (has && j + 1 < text.size() && text[j] == '%' && text[j + 1] == ']') {
            best = v;
        }
    }
    return best;
}

} // namespace

JobManager::JobManager(std::string dataDir, std::string workerExe, int maxWorkers,
                       double minMemGb)
    : dataDir_(std::move(dataDir)),
      workerExe_(std::move(workerExe)),
      maxWorkers_(std::max(1, maxWorkers)),
      minMemGb_(minMemGb) {}

JobManager::~JobManager() { stop(); }

std::string JobManager::submit(const ip::Json& body, std::string& err) {
    if (!body.is_object()) {
        err = "任务体必须是 JSON 对象（workflow 同构配置）";
        return "";
    }
    Job j;
    j.id = nowStamp() + "_" + std::to_string(++seq_);
    j.name = body.value("name", std::string("job"));
    j.only = body.value("only", std::string(""));
    if (!j.only.empty() && j.only != "color" && j.only != "tiling") {
        err = "only 仅支持 color / tiling（或省略）";
        return "";
    }
    // 去掉服务层字段，其余即 workflow 配置
    ip::Json task = body;
    task.erase("name");
    task.erase("only");
    j.state = "queued";
    j.createdAt = nowSec();
    j.jobDir = (fs::path(dataDir_) / "jobs" / j.id).string();

    std::error_code ec;
    fs::create_directories(j.jobDir, ec);
    if (ec) {
        err = "无法创建任务目录: " + j.jobDir;
        return "";
    }
    if (!writeAtomic((fs::path(j.jobDir) / "task.json").string(),
                     task.dump(2))) {
        err = "写入 task.json 失败";
        return "";
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        jobs_[j.id] = j;
        queue_.push_back(j.id);
        persist(jobs_[j.id]);
        pruneLocked();
    }
    cv_.notify_all();
    return j.id;
}

ip::Json JobManager::get(const std::string& id, bool& found) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(id);
    found = it != jobs_.end();
    if (!found) return ip::Json::object();
    const Job& j = it->second;
    ip::Json o;
    o["id"] = j.id;
    o["name"] = j.name;
    o["only"] = j.only;
    o["state"] = j.state;
    o["message"] = j.message;
    o["percent"] = j.percent;
    o["exit_code"] = j.exitCode;
    o["pid"] = j.pid;
    o["created_at"] = j.createdAt;
    o["started_at"] = j.startedAt;
    o["finished_at"] = j.finishedAt;
    o["job_dir"] = j.jobDir;
    return o;
}

ip::Json JobManager::list() const {
    std::lock_guard<std::mutex> lk(mu_);
    ip::Json arr = ip::Json::array();
    // 仅返回最新 kMaxListItems 条（id 为时间前缀，map 已按时间升序）
    const size_t skip = jobs_.size() > kMaxListItems ? jobs_.size() - kMaxListItems : 0;
    size_t idx = 0;
    for (const auto& [id, j] : jobs_) {
        if (idx++ < skip) continue;
        ip::Json o;
        o["id"] = j.id;
        o["name"] = j.name;
        o["state"] = j.state;
        o["percent"] = j.percent;
        o["created_at"] = j.createdAt;
        o["finished_at"] = j.finishedAt;
        arr.push_back(std::move(o));
    }
    return arr;
}

std::string JobManager::logs(const std::string& id, bool& found) const {
    std::string jobDir;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = jobs_.find(id);
        found = it != jobs_.end();
        if (!found) return "";
        jobDir = it->second.jobDir;
    }
    std::string out = "===== stdout (tail) =====\n" +
                      readTail((fs::path(jobDir) / "stdout.log").string(), 128 * 1024);
    const std::string err = readTail((fs::path(jobDir) / "stderr.log").string(),
                                     64 * 1024);
    if (!err.empty()) out += "\n===== stderr (tail) =====\n" + err;
    return out;
}

bool JobManager::cancel(const std::string& id, std::string& outcome,
                        std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) {
        err = "任务不存在";
        return false;
    }
    Job& j = it->second;
    if (j.state == "queued") {
        queue_.erase(std::remove(queue_.begin(), queue_.end(), id), queue_.end());
        setStateLocked(j, "canceled", "提交后取消");
        persist(j);
        outcome = "canceled";
        return true;
    }
    if (j.state == "running") {
        // 仅发送取消请求：worker 可能仍在收尾，最终状态由回收线程确认
        if (j.pid > 0) ::kill(j.pid, SIGTERM);
        j.cancelRequested = true;
        j.message = "取消请求已发送";
        persist(j);
        outcome = "cancel_requested";
        return true;
    }
    err = "任务已结束（" + j.state + "），无需取消";
    return false;
}

// 内存任务表上限保护：驱逐最旧的终态任务（磁盘 state.json 保留，历史不丢）
void JobManager::pruneLocked() {
    if (jobs_.size() <= kMaxJobsInMemory) return;
    for (auto it = jobs_.begin();
         it != jobs_.end() && jobs_.size() > kMaxJobsInMemory;) {
        const Job& j = it->second;
        const bool terminal = j.state == "done" || j.state == "failed" ||
                              j.state == "canceled";
        if (terminal && j.pid <= 0) {
            it = jobs_.erase(it);
        } else {
            ++it;
        }
    }
}

int JobManager::runningCountLocked() const {
    int n = 0;
    for (const auto& [id, j] : jobs_) {
        if (j.state == "running") ++n;
    }
    return n;
}

void JobManager::setStateLocked(Job& j, const std::string& st,
                                const std::string& msg) {
    j.state = st;
    if (!msg.empty()) j.message = msg;
    if (st == "running" && j.startedAt == 0) j.startedAt = nowSec();
    if ((st == "done" || st == "failed" || st == "canceled") &&
        j.finishedAt == 0) {
        j.finishedAt = nowSec();
    }
}

// 派发（持有 mu_）：posix_spawn 全新进程，stdout/stderr 落盘
void JobManager::dispatchLocked(Job& j) {
    const std::string taskPath = (fs::path(j.jobDir) / "task.json").string();
    const std::string outPath = (fs::path(j.jobDir) / "stdout.log").string();
    const std::string errPath = (fs::path(j.jobDir) / "stderr.log").string();

    std::vector<std::string> args;
    args.push_back(workerExe_);
    args.push_back("--config");
    args.push_back(taskPath);
    if (!j.only.empty()) {
        args.push_back("--only");
        args.push_back(j.only);
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, outPath.c_str(),
                                     O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_addopen(&fa, 2, errPath.c_str(),
                                     O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    // 独立进程组：取消时可对整组发信号（且不误伤服务自身）
    posix_spawnattr_setpgroup(&attr, 0);
    short flags = POSIX_SPAWN_SETPGROUP;
    posix_spawnattr_setflags(&attr, flags);

    pid_t pid = -1;
    const int rc = posix_spawn(&pid, workerExe_.c_str(), &fa, &attr,
                               argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        j.message = "worker 启动失败（posix_spawn rc=" + std::to_string(rc) + "）";
        setStateLocked(j, "failed", j.message);
        j.exitCode = -1;
        persist(j);
        return;
    }
    j.pid = static_cast<int>(pid);
    j.percent = 0;
    setStateLocked(j, "running", "");
    persist(j);
}

void JobManager::schedulerLoop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::milliseconds(400),
                         [this] { return shuttingDown_.load(); });
            if (shuttingDown_.load()) return;

            // 刷新运行中任务的进度（读日志尾部）
            for (auto& [id, j] : jobs_) {
                if (j.state == "running") {
                    const std::string t = readTail(
                        (fs::path(j.jobDir) / "stdout.log").string(), 8192);
                    const int p = parseProgress(t);
                    if (p >= 0 && p > j.percent) j.percent = p;
                }
            }

            // 派发：受并发上限 + 内存守卫约束（R4）
            const double avail = ip::MemGuard::availableGB();
            while (!queue_.empty() && runningCountLocked() < maxWorkers_) {
                if (avail >= 0 && avail < minMemGb_) {
                    break;  // 内存不足 → 暂缓派发（保持排队）
                }
                const std::string id = queue_.front();
                auto it = jobs_.find(id);
                if (it == jobs_.end() || it->second.state != "queued") {
                    queue_.erase(queue_.begin());
                    continue;
                }
                queue_.erase(queue_.begin());
                dispatchLocked(it->second);
            }
        }
    }
}

void JobManager::reaperLoop() {
    while (true) {
        int status = 0;
        const pid_t pid = ::waitpid(-1, &status, WNOHANG);
        if (pid > 0) {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& [id, j] : jobs_) {
                if (j.pid != static_cast<int>(pid)) continue;
                if (WIFEXITED(status)) {
                    j.exitCode = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    j.exitCode = 128 + WTERMSIG(status);
                }
                // 区分三种终止：用户取消 / 服务停机中断 / 正常失败
                if (j.cancelRequested) {
                    setStateLocked(j, "canceled", "已取消");
                } else if (j.exitCode == 0) {
                    j.percent = 100;
                    setStateLocked(j, "done", "");
                } else if (shuttingDown_) {
                    setStateLocked(j, "failed",
                                   j.message.empty()
                                       ? "服务重启中断（可重新提交；切片支持断点续跑）"
                                       : j.message);
                } else {
                    setStateLocked(j, "failed",
                                   "worker 退出码 " + std::to_string(j.exitCode));
                }
                j.pid = -1;
                persist(j);
                break;
            }
            continue;  // 立即回收下一个
        }
        if (shuttingDown_) {
            std::lock_guard<std::mutex> lk(mu_);
            if (runningCountLocked() == 0) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}

void JobManager::persist(const Job& j) const {
    ip::Json o;
    o["id"] = j.id;
    o["name"] = j.name;
    o["only"] = j.only;
    o["state"] = j.state;
    o["message"] = j.message;
    o["percent"] = j.percent;
    o["exit_code"] = j.exitCode;
    o["created_at"] = j.createdAt;
    o["started_at"] = j.startedAt;
    o["finished_at"] = j.finishedAt;
    writeAtomic((fs::path(j.jobDir) / "state.json").string(), o.dump(2));
}

void JobManager::loadFromDisk() {
    const std::string jobsRoot = (fs::path(dataDir_) / "jobs").string();
    std::error_code ec;
    if (!fs::is_directory(jobsRoot, ec)) return;
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& e : fs::directory_iterator(jobsRoot, ec)) {
        if (!e.is_directory()) continue;
        const std::string stateFile = (e.path() / "state.json").string();
        ip::Json o;
        std::string err;
        if (!ip::readJsonFile(stateFile, o, err)) continue;
        Job j;
        j.id = o.value("id", e.path().filename().string());
        j.name = o.value("name", std::string("job"));
        j.only = o.value("only", std::string(""));
        j.state = o.value("state", std::string("failed"));
        j.message = o.value("message", std::string(""));
        j.percent = o.value("percent", 0);
        j.exitCode = o.value("exit_code", -1);
        j.createdAt = o.value("created_at", 0LL);
        j.startedAt = o.value("started_at", 0LL);
        j.finishedAt = o.value("finished_at", 0LL);
        j.jobDir = e.path().string();
        // 上个进程生命周期内运行中的任务：如实标记为中断（可重提交；tiling 有 resume 幂等）
        if (j.state == "running" || j.state == "queued") {
            j.state = "failed";
            j.message = "服务重启中断（可重新提交；切片支持断点续跑）";
            j.pid = -1;
            persist(j);
        }
        jobs_[j.id] = std::move(j);
    }
}

void JobManager::start() {
    schedThread_ = std::thread([this] { schedulerLoop(); });
    reaperThread_ = std::thread([this] { reaperLoop(); });
}

void JobManager::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (shuttingDown_) return;
        shuttingDown_ = true;
        for (auto& [id, j] : jobs_) {
            if (j.state == "running" && j.pid > 0) {
                j.message = "服务重启中断（可重新提交；切片支持断点续跑）";
                persist(j);
                ::kill(j.pid, SIGTERM);
            }
        }
    }
    cv_.notify_all();
    // 限时等待 worker 退出；超时升级 SIGKILL（避免 worker 忽略 SIGTERM 时 join 永久挂起）
    for (int i = 0; i < 50; ++i) {  // 最多约 5s
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (runningCountLocked() == 0) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [id, j] : jobs_) {
            if (j.state == "running" && j.pid > 0) {
                ip::logWarn("worker 未响应 SIGTERM，升级 SIGKILL: pid " +
                            std::to_string(j.pid));
                ::kill(j.pid, SIGKILL);
            }
        }
    }
    if (schedThread_.joinable()) schedThread_.join();
    if (reaperThread_.joinable()) reaperThread_.join();
}

} // namespace ipsrv