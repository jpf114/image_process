#!/usr/bin/env python3
# Phase 4 服务化压测（4.5）：并发任务 / 大影像内存峰值 / 每任务线程数超订对照 / 控制面可用性
#
# 运行：conda run -n geo_env python scripts/stress/phase4_stress.py
# 产出：output/phase4_stress/report.json（一手数据，供 Phase4压测报告 引用）
#
# 采样方式（一手数据，不模拟）：
#  - worker 峰值内存：读 /proc/<pid>/status 的 VmHWM（峰值常驻集）
#  - 服务可用性/延迟：每 1s 轮询 GET /api/v1/health，记录延迟与失败数
#  - 任务耗时：提交→终态 的墙钟时间
import json
import os
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BIN = os.path.join(ROOT, "bin/Release/ip_server")
OUT = os.path.join(ROOT, "output/phase4_stress")
DATA2024 = "文档资料/关于栅格切片和匀色/data/2024"
GF2 = "文档资料/关于栅格切片和匀色/data/2025/GF2_PMS1_E105.6_N29.0_RGB_20250324C.tif"

VCPKG = os.environ.get("VCPKG_ROOT") or os.path.expanduser("~/vcpkg")
INST = os.path.join(VCPKG, "installed/x64-linux")


def worker_env():
    """显式钉住 GDAL/PROJ 数据路径（vcpkg 版）。

    必须显式覆盖：若在 conda 环境（如 geo_env）中启动，激活脚本会注入
    PROJ_DATA/GDAL_DATA/GDAL_DRIVER_PATH 指向该环境的另一套 PROJ 数据库
    （LAYOUT.VERSION 与 vcpkg GDAL 3.12 不兼容）→ EPSG 查询失败
    （"无法识别 EPSG:4490"）。生产由 ipenv.sh / 容器 ENV 承担同样职责。
    """
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = os.path.join(INST, "lib") + ":" + env.get("LD_LIBRARY_PATH", "")
    env["GDAL_DATA"] = os.path.join(INST, "share/gdal")
    env["PROJ_DATA"] = os.path.join(INST, "share/proj")
    env["PROJ_LIB"] = os.path.join(INST, "share/proj")
    env.pop("GDAL_DRIVER_PATH", None)
    return env


def http(method, url, body=None, timeout=10):
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        raw = r.read()
        return r.status, (json.loads(raw) if raw else None)


def job_cfg(tag, i, num_threads):
    """2024 匀色(create) + CGCS2000 z13 小范围切片；每任务独立输出目录避免续跑短路"""
    return {
        "name": f"{tag}-{i}",
        "color_balance": {
            "input": [DATA2024],
            "output_dir": f"output/phase4_stress/{tag}/j{i}/balanced",
            "method": "overlap_adjust",
            "output_mode": "vrt",
            "ref_image": "H48E018014C",
        },
        "tiling": {
            "output_dir": f"output/phase4_stress/{tag}/j{i}/tiles",
            "tiling_scheme": "CGCS2000",
            "min_zoom": 13,
            "max_zoom": 13,
            "format": "png",
            "overview_policy": "no",
            "tiles_scope": "footprint",
            "bbox": "105.30,29.02,105.32,29.04",
            "tile_buffer": 1,
            "num_threads": num_threads,
        },
    }


def gf2_cfg(tag):
    """大影像（GF2 40053x34426）统计内存峰值：块式处理恒定内存验证"""
    return {
        "name": f"{tag}-gf2-stats",
        "only": "color",  # 仅匀色（无 tiling 段）
        "color_balance": {
            "input": [GF2],
            "output_dir": f"output/phase4_stress/{tag}/balanced",
            "method": "overlap_adjust",
            "output_mode": "vrt",
        },
    }


MOSAIC_WITH_OVR = "output/phase3/e2e_2024/balanced/mosaic.vrt"  # 已带 .ovr 金字塔（Phase 3 产出，702MB）


def job_cfg_full(tag, i, num_threads):
    """切片密集（**tiling-only**，492 瓦片/任务）——用于"每任务线程数 × worker 并发"超订对照：
       2 worker × nt=4 → 8 线程/4 核（超订）；2 worker × nt=2 → 4 线程（匹配核数）。

    注：输入复用 Phase 3 已带 .ovr 金字塔的 mosaic.vrt。若在无金字塔的 VRT 上跑同一规模，
    实测单任务需 ≈68 分钟（≈8.8s/瓦片）——见报告"关于 overview 的观测"一节。
    """
    return {
        "name": f"{tag}-{i}",
        "only": "tiling",
        "tiling": {
            "input": MOSAIC_WITH_OVR,
            "output_dir": f"output/phase4_stress/{tag}/j{i}/tiles",
            "tiling_scheme": "CGCS2000",
            "min_zoom": 12,
            "max_zoom": 14,
            "format": "png",
            "overview_policy": "auto",  # 已存在 .ovr → 直接使用
            "num_threads": num_threads,
        },
    }


def peak_rss_kb(pid):
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmHWM:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return 0


def peak_gb():
    with open("/proc/meminfo") as f:
        for line in f:
            if line.startswith("MemAvailable:"):
                return int(line.split()[1]) / (1024 * 1024)
    return -1.0


def run_scenario(tag, workers, num_threads, jobs, port):
    base = f"http://127.0.0.1:{port}"
    datadir = os.path.join(OUT, tag, "ipdata")
    os.makedirs(datadir, exist_ok=True)
    log = open(os.path.join(OUT, tag, "server.log"), "w")
    srv = subprocess.Popen(
        [BIN, "--data-dir", datadir, "--port", str(port), "--workers", str(workers),
         "--min-mem-gb", "1.5"],
        cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, env=worker_env())
    try:
        for _ in range(60):
            try:
                http("GET", base + "/api/v1/health", timeout=2)
                break
            except Exception:
                time.sleep(0.5)

        t0 = time.time()
        ids, cfgs = [], []
        for i, cfg in enumerate(jobs):
            cfgs.append(cfg)
            st, res = http("POST", base + "/api/v1/jobs", cfg)
            ids.append(res["job_id"])
        submit_done = time.time()

        per_job = {j: {"submit": t, "start": None, "end": None, "peak_kb": 0}
                   for j, t in zip(ids, [submit_done] * len(ids))}
        health_lat, health_fail = [], 0
        mem_min = 99.0
        last_health = 0.0
        while True:
            done = 0
            for j in ids:
                st, d = http("GET", base + f"/api/v1/jobs/{j}")
                if per_job[j]["start"] is None and d["state"] == "running":
                    per_job[j]["start"] = time.time()
                if d["pid"] and d["pid"] > 0:
                    pk = peak_rss_kb(d["pid"])
                    if pk > per_job[j]["peak_kb"]:
                        per_job[j]["peak_kb"] = pk
                if d["state"] in ("done", "failed", "canceled"):
                    if per_job[j]["end"] is None:
                        per_job[j]["end"] = time.time()
                        per_job[j]["state"] = d["state"]
                    done += 1
            now = time.time()
            if now - last_health >= 1.0:
                last_health = now
                try:
                    h0 = time.time()
                    http("GET", base + "/api/v1/health", timeout=5)
                    health_lat.append((time.time() - h0) * 1000)
                except Exception:
                    health_fail += 1
                m = peak_gb()
                if m >= 0:
                    mem_min = min(mem_min, m)
            if done == len(ids):
                break
            time.sleep(0.3)
        t_all_done = time.time()

        res = {
            "tag": tag, "workers": workers, "num_threads": num_threads,
            "jobs": len(ids),
            "wall_sec": round(t_all_done - t0, 2),
            "submit_sec": round(submit_done - t0, 2),
            "per_job": [
                {"id": j, "state": per_job[j].get("state"),
                 "wall_sec": round((per_job[j]["end"] or t_all_done) -
                                   (per_job[j]["start"] or t0), 1),
                 "peak_rss_mb": round(per_job[j]["peak_kb"] / 1024.0, 1)}
                for j in ids],
            "peak_rss_mb_max": round(max(v["peak_kb"] for v in per_job.values()) / 1024.0, 1),
            "health_samples": len(health_lat),
            "health_fail": health_fail,
            "health_p95_ms": round(sorted(health_lat)[int(len(health_lat) * 0.95)] if health_lat else 0, 1),
            "health_max_ms": round(max(health_lat) if health_lat else 0, 1),
            "mem_available_min_gb": round(mem_min, 2),
        }
        return res, srv, log
    except Exception as e:
        srv.send_signal(signal.SIGTERM)
        srv.wait(timeout=30)
        log.close()
        raise e


def shutdown(srv, log):
    srv.send_signal(signal.SIGTERM)
    rc = srv.wait(timeout=60)
    log.close()
    return rc


def main():
    os.makedirs(OUT, exist_ok=True)
    report = {"scenarios": [], "note": "一手采样：/proc VmHWM 峰值 + /health 轮询",
              "env": {"GDAL_DATA": worker_env()["GDAL_DATA"],
                      "PROJ_DATA": worker_env()["PROJ_DATA"],
                      "LD_LIBRARY_PATH_head": worker_env()["LD_LIBRARY_PATH"].split(":")[0]}}
    port = 18095
    plan_all = {
        # 小切片（1 瓦片/任务）：并发度与内存基线
        "s1_w1_t4": (1, 4, [job_cfg("s1_w1_t4", i, 4) for i in range(4)]),
        "s2_w2_t4": (2, 4, [job_cfg("s2_w2_t4", i, 4) for i in range(4)]),
        "s3_w2_t2": (2, 2, [job_cfg("s3_w2_t2", i, 2) for i in range(4)]),
        # 大影像统计内存峰值
        "s4_gf2": (1, 4, [gf2_cfg("s4_gf2")]),
        # 切片密集（464 瓦片/任务）：线程超订对照
        "s5_w1_t4_full": (1, 4, [job_cfg_full("s5_w1_t4_full", i, 4) for i in range(2)]),
        "s6_w2_t4_full": (2, 4, [job_cfg_full("s6_w2_t4_full", i, 4) for i in range(2)]),
        "s7_w2_t2_full": (2, 2, [job_cfg_full("s7_w2_t2_full", i, 2) for i in range(2)]),
    }
    only = sys.argv[1:]
    plan = [(t, *plan_all[t]) for t in (only if only else plan_all)]
    for tag, workers, nt, jobs in plan:
        os.makedirs(os.path.join(OUT, tag), exist_ok=True)
        print(f"== 场景 {tag}: workers={workers} num_threads={nt} jobs={len(jobs)}", flush=True)
        res, srv, log = run_scenario(tag, workers, nt, jobs, port)
        rc = shutdown(srv, log)
        res["server_exit"] = rc
        report["scenarios"].append(res)
        print(json.dumps(res, ensure_ascii=False, indent=2), flush=True)
    # 与既有报告按 tag 合并（支持分批跑场景）
    rp = os.path.join(OUT, "report.json")
    if os.path.exists(rp):
        try:
            with open(rp) as f:
                old = json.load(f)
            by_tag = {s["tag"]: s for s in old.get("scenarios", [])}
            for s in report["scenarios"]:
                by_tag[s["tag"]] = s
            report["scenarios"] = list(by_tag.values())
        except Exception:
            pass
    with open(rp, "w") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    print("报告: output/phase4_stress/report.json", flush=True)


if __name__ == "__main__":
    sys.exit(main())