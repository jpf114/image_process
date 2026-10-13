#pragma once
// 服务化：任务队列 + 状态机 + 磁盘持久化 + worker 进程池（fork/exec）
//
// 设计依据《Phase4服务化论证报告.md》：
//  - 控制面/执行面分离：本类只排队与监管，重任务一律 fork+exec 到 worker 进程
//  - 严禁裸 fork 复用 GDAL 状态：子进程立即 exec 全新镜像（exec 重置地址空间）
//  - 并发上限 = min(物理核数, 内存允许) ；派发前用 MemGuard 检查可用内存
//  - 状态落盘原子写（临时文件 + rename），重启可恢复（运行中任务标记中断）
//  - 进度回传：读 worker stdout 日志尾部的 "[NN%]"（operator 已按此格式输出）
#include "ip/json_io.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ipsrv {

struct Job {
    std::string id;
    std::string name;
    std::string only;        // ""（全流程）| color | tiling
    ip::Json task;           // 传给 workflow --config 的完整配置
    std::string state;       // queued | running | done | failed | canceled
    std::string message;     // 附加说明（错误/中断原因）
    int percent = 0;
    int exitCode = -1;
    long long createdAt = 0;
    long long startedAt = 0;
    long long finishedAt = 0;
    int pid = -1;
    std::string jobDir;      // <dataDir>/jobs/<id>
    bool cancelRequested = false;
};

class JobManager {
public:
    // dataDir: 状态与日志根目录；workerExe: 被 fork+exec 的执行体（workflow exe 绝对路径）
    JobManager(std::string dataDir, std::string workerExe, int maxWorkers,
               double minMemGb);
    ~JobManager();

    // 提交任务：body 为 workflow 同构配置（可选 name/only 字段）；返回 job id
    std::string submit(const ip::Json& body, std::string& err);

    // 查询（found=false 表示无此任务）
    ip::Json get(const std::string& id, bool& found) const;
    ip::Json list() const;
    std::string logs(const std::string& id, bool& found) const;

    // 取消：queued → 直接置 canceled；running → SIGTERM（由回收线程确认）
    bool cancel(const std::string& id, std::string& err);

    void start();
    void stop();  // SIGTERM 全部运行中 worker，等待退出

    // 启动时加载磁盘状态（运行中任务标记为中断）
    void loadFromDisk();

private:
    void schedulerLoop();
    void reaperLoop();
    void dispatchLocked(Job& j);           // 需持有 mutex_
    void refreshProgress(Job& j);          // 无锁调用（仅读日志文件）
    void persist(const Job& j) const;      // 原子写 <jobDir>/state.json
    void setStateLocked(Job& j, const std::string& st, const std::string& msg);
    int runningCountLocked() const;
    std::string workerPathForLog() const { return workerExe_; }

    std::string dataDir_;
    std::string workerExe_;
    int maxWorkers_ = 1;
    double minMemGb_ = 1.5;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::string, Job> jobs_;
    std::vector<std::string> queue_;   // queued 的任务 id（FIFO）
    std::atomic<bool> shuttingDown_{false};  // 停机中（供 reaper 判定中断语义，需跨线程可见）
    std::thread schedThread_;
    std::thread reaperThread_;

    std::atomic<long long> seq_{0};
};

} // namespace ipsrv