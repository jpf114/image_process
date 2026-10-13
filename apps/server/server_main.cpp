// ip_server：服务化控制面（cpp-httplib 薄层）
//
// 定位（见《Phase4服务化论证报告.md》）：只做接收/排队/查询/静态瓦片，
// 重任务一律 fork+exec 到 worker 进程执行——长任务绝不占用 HTTP 工作线程（R1）。
//
// 端点（REST v1）：
//   GET    /api/v1/health                健康检查（免鉴权）
//   POST   /api/v1/jobs                  提交任务（body = workflow 同构 JSON；可带 name/only）
//   GET    /api/v1/jobs                  任务列表
//   GET    /api/v1/jobs/{id}             状态/进度
//   GET    /api/v1/jobs/{id}/logs        运行日志尾部
//   DELETE /api/v1/jobs/{id}             取消
//   GET    /tiles/...                    静态瓦片（映射 --tiles-dir）
//
// 用法: ip_server --data-dir <目录> --port <8080> [--tiles-dir <目录>] [--workers N]
//                 [--token <令牌>] [--min-mem-gb 1.5] [--worker <workflow exe 路径>]
#define CPPHTTPLIB_THREAD_POOL_COUNT 8  // 控制面线程池上限设小（R1），与执行面进程池分离

#include "job_manager.h"

#include "ip/json_io.h"
#include "ip/log.h"
#include "ip/mem_guard.h"

#include <httplib.h>

#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include <unistd.h>

namespace fs = std::filesystem;

namespace {

void sendJson(httplib::Response& res, int status, const ip::Json& j) {
    res.status = status;
    res.set_content(j.dump(2), "application/json; charset=utf-8");
}

// worker exe：默认取自身同目录下的 workflow（bin/<cfg>/workflow）
std::string defaultWorkerPath() {
    std::error_code ec;
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (ec) return "workflow";
    return (self.parent_path() / "workflow").string();
}

int defaultWorkers() {
    const unsigned hw = std::thread::hardware_concurrency();
    const int cores = hw > 0 ? static_cast<int>(hw) : 2;
    const double avail = ip::MemGuard::availableGB();
    int byMem = avail > 0 ? static_cast<int>(avail / 3.0) : 1;  // 单任务峰值按 ~3GB 估
    return std::max(1, std::min(cores, std::max(1, byMem)));
}

void usage() {
    std::cout
        << "用法: ip_server --data-dir <目录> [选项]\n"
           "  --host <0.0.0.0>          监听地址\n"
           "  --port <8080>             监听端口\n"
           "  --tiles-dir <目录>        静态瓦片根（映射 /tiles/，可省略）\n"
           "  --workers <N>             worker 并发上限（默认 min(核数, 内存/3GB)）\n"
           "  --min-mem-gb <1.5>        派发前内存守卫阈值\n"
           "  --token <令牌>            可选 Bearer 鉴权\n"
           "  --worker <路径>           执行体（默认同目录 workflow）\n"
           "  -h | --help\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string host = "0.0.0.0";
    int port = 8080;
    std::string dataDir, tilesDir, token, workerExe;
    int workers = -1;
    double minMemGb = 1.5;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "缺少参数值: " << name << std::endl;
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a == "--host") {
            host = need("--host");
        } else if (a == "--port") {
            port = std::atoi(need("--port"));
        } else if (a == "--data-dir") {
            dataDir = need("--data-dir");
        } else if (a == "--tiles-dir") {
            tilesDir = need("--tiles-dir");
        } else if (a == "--workers") {
            workers = std::atoi(need("--workers"));
        } else if (a == "--min-mem-gb") {
            minMemGb = std::atof(need("--min-mem-gb"));
        } else if (a == "--token") {
            token = need("--token");
        } else if (a == "--worker") {
            workerExe = need("--worker");
        } else {
            std::cerr << "未知参数: " << a << std::endl;
            usage();
            return 2;
        }
    }
    if (dataDir.empty()) {
        std::cerr << "缺少 --data-dir（状态与日志根目录）" << std::endl;
        usage();
        return 2;
    }
    if (workerExe.empty()) workerExe = defaultWorkerPath();
    if (workers <= 0) workers = defaultWorkers();

    std::error_code ec;
    fs::create_directories(dataDir, ec);

    ip::logInfo("==== ip_server 启动 ====");
    ip::logInfo("  监听 " + host + ":" + std::to_string(port));
    ip::logInfo("  数据目录 " + dataDir);
    ip::logInfo("  执行体 " + workerExe);
    ip::logInfo("  worker 并发上限 " + std::to_string(workers) +
                "（核数 " + std::to_string(std::thread::hardware_concurrency()) +
                "，内存守卫 " + std::to_string(minMemGb) + "GB）");
    if (!token.empty()) ip::logInfo("  已启用 Bearer 鉴权");
    if (!tilesDir.empty()) ip::logInfo("  静态瓦片 " + tilesDir + " → /tiles/");

    ipsrv::JobManager mgr(dataDir, workerExe, workers, minMemGb);
    mgr.loadFromDisk();
    mgr.start();

    httplib::Server svr;
    svr.set_payload_max_length(16 * 1024 * 1024);  // 任务 JSON 上限（R5）
    svr.set_read_timeout(30, 0);
    svr.set_write_timeout(30, 0);
    svr.set_keep_alive_timeout(5);

    // 鉴权（可选）——/api/v1/health 免鉴权（容器探针）
    svr.set_pre_routing_handler([&](const httplib::Request& req,
                                    httplib::Response& res) {
        if (token.empty() || req.path == "/api/v1/health") {
            return httplib::Server::HandlerResponse::Unhandled;
        }
        if (req.get_header_value("Authorization") != "Bearer " + token) {
            sendJson(res, 401, {{"error", "unauthorized"}});
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    svr.Get("/api/v1/health", [](const httplib::Request&, httplib::Response& res) {
        sendJson(res, 200, {{"status", "ok"}});
    });

    // 提交任务
    svr.Post("/api/v1/jobs", [&mgr](const httplib::Request& req,
                                    httplib::Response& res) {
        ip::Json body;
        try {
            body = ip::Json::parse(req.body);
        } catch (const std::exception& e) {
            sendJson(res, 400, {{"error", std::string("JSON 解析失败: ") + e.what()}});
            return;
        }
        std::string err;
        const std::string id = mgr.submit(body, err);
        if (id.empty()) {
            sendJson(res, 400, {{"error", err}});
            return;
        }
        ip::logInfo("提交任务 " + id);
        sendJson(res, 202, {{"job_id", id}, {"state", "queued"}});
    });

    svr.Get("/api/v1/jobs", [&mgr](const httplib::Request&, httplib::Response& res) {
        sendJson(res, 200, mgr.list());
    });

    svr.Get(R"(/api/v1/jobs/([^/]+)/logs)",
            [&mgr](const httplib::Request& req, httplib::Response& res) {
                bool found = false;
                const std::string txt = mgr.logs(req.matches[1].str(), found);
                if (!found) {
                    sendJson(res, 404, {{"error", "任务不存在"}});
                    return;
                }
                res.status = 200;
                res.set_content(txt, "text/plain; charset=utf-8");
            });

    svr.Get(R"(/api/v1/jobs/([^/]+))",
            [&mgr](const httplib::Request& req, httplib::Response& res) {
                bool found = false;
                const ip::Json j = mgr.get(req.matches[1].str(), found);
                if (!found) {
                    sendJson(res, 404, {{"error", "任务不存在"}});
                    return;
                }
                sendJson(res, 200, j);
            });

    svr.Delete(R"(/api/v1/jobs/([^/]+))",
               [&mgr](const httplib::Request& req, httplib::Response& res) {
                   std::string err;
                   if (!mgr.cancel(req.matches[1].str(), err)) {
                       sendJson(res, 409, {{"error", err}});
                       return;
                   }
                   sendJson(res, 200, {{"canceled", req.matches[1].str()}});
               });

    // 静态瓦片（长文件传输不经任务线程池排队逻辑，交由 httplib 文件服务）
    if (!tilesDir.empty()) {
        fs::create_directories(tilesDir, ec);
        if (!svr.set_mount_point("/tiles", tilesDir)) {
            ip::logWarn("静态瓦片挂载失败: " + tilesDir);
        }
    }

    // 优雅退出：主线程 sigwait，收到信号后 svr.stop()（R8）
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    std::signal(SIGPIPE, SIG_IGN);  // cpp-httplib SIGPIPE 注意事项

    std::thread listenThread([&svr, &host, port, &mgr] {
        if (!svr.listen(host, port)) {
            ip::logError("监听失败（端口被占用？）");
            std::exit(1);
        }
        (void)mgr;
    });

    int sig = 0;
    sigwait(&set, &sig);
    ip::logInfo("收到信号 " + std::to_string(sig) + "，开始优雅退出");
    svr.stop();
    listenThread.join();
    mgr.stop();  // SIGTERM 全部 worker，等待回收
    ip::logInfo("==== ip_server 已退出 ====");
    return 0;
}