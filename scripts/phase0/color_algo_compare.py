# -*- coding: utf-8 -*-
"""Phase 0.4 匀色算法对比试验
方法：global_adjust(景级增益/偏置) / hist_match(直方图匹配) / block_wallis(分块Wallis) / reinhard(lαβ颜色传递)
数据：2024 四景 2m（景间匀色，目视接缝+定量台阶差）；2025 GF2 0.8m 极暗（对齐 2024 目标）
输出：output/phase0/color/ 下各方法拼接预览图 + 定量指标
"""
import json
import subprocess
import time
from pathlib import Path

import cv2
import numpy as np
from osgeo import gdal

gdal.UseExceptions()

def check_mem(tag=""):
    """内存护栏：本机 WSL 仅 9.7GB RAM，可用 <1.5GB 时终止"""
    r = subprocess.run(["free", "-m"], capture_output=True, text=True)
    avail = int(r.stdout.splitlines()[1].split()[6])
    print(f"  [mem] 可用 {avail}MB {tag}")
    if avail < 1500:
        raise MemoryError(f"可用内存不足({avail}MB)，终止以防 WSL 崩溃")

DATA = Path("/home/tdt-dell/code/MyProject/image_process/文档资料/关于栅格切片和匀色/data")
OUT = Path("/home/tdt-dell/code/MyProject/image_process/output/phase0/color")
OUT.mkdir(parents=True, exist_ok=True)

SCENES_2024 = ["H48E018014C.tif", "H48E018015C.tif", "H48E019014C.tif", "H48E019015C.tif"]
GF2 = DATA / "2025/GF2_PMS1_E105.6_N29.0_RGB_20250324C.tif"
BLOCK = 512
PREVIEW_FACTOR = 8

# ---------- 基础工具 ----------
def read_scene(path, decim=1):
    """读取 RGB(H,W,3 float32) + alpha(H,W bool)；decim>1 时降采样读取"""
    ds = gdal.Open(str(path))
    w, h = ds.RasterXSize, ds.RasterYSize
    sw, sh = w // decim, h // decim
    arr = ds.ReadAsArray(0, 0, w, h, buf_xsize=sw, buf_ysize=sh)
    ds = None
    rgb = np.transpose(arr[:3], (1, 2, 0)).astype(np.float32)
    alpha = arr[3] > 0
    return rgb, alpha

def exact_stats(path):
    """分块精确统计（直方图法），返回 per-band (mean, std, hist256)（仅 alpha>0 像素）"""
    ds = gdal.Open(str(path))
    w, h = ds.RasterXSize, ds.RasterYSize
    hist = np.zeros((3, 256), dtype=np.int64)
    bs = 2048
    for y in range(0, h, bs):
        hh = min(bs, h - y)
        arr = ds.ReadAsArray(0, y, w, hh)
        m = arr[3] > 0
        for b in range(3):
            hist[b] += np.bincount(arr[b][m].ravel(), minlength=256)
    ds = None
    n = hist.sum(axis=1)
    mean = (hist * np.arange(256)).sum(axis=1) / np.maximum(n, 1)
    var = (hist * (np.arange(256)[None, :] - mean[:, None]) ** 2).sum(axis=1) / np.maximum(n, 1)
    return mean, np.sqrt(var), hist

def apply_gain_offset(rgb, alpha, gain, offset):
    out = rgb * gain[None, None, :] + offset[None, None, :]
    np.clip(out, 0, 255, out=out)
    return out

def apply_lut(rgb, alpha, luts):
    """luts: list of 256-entry uint8 arrays per band"""
    out = np.empty_like(rgb, dtype=np.uint8)
    for b in range(3):
        out[:, :, b] = luts[b][rgb[:, :, b].astype(np.uint8)]
    return out.astype(np.float32)

def cdf_match_lut(src_hist, ref_hist):
    """返回 256 项 LUT：src 值 -> ref 分布"""
    cdf_s = np.cumsum(src_hist) / max(src_hist.sum(), 1)
    cdf_r = np.cumsum(ref_hist) / max(ref_hist.sum(), 1)
    lut = np.zeros(256, dtype=np.uint8)
    j = 0
    for i in range(256):
        while j < 255 and cdf_r[j] < cdf_s[i]:
            j += 1
        lut[i] = j
    return lut

def block_wallis_params(path, target_mean, target_std, block=BLOCK):
    """计算分块 (a, b) 参数网格（逐波段），返回 grids, 每格中心坐标；无效像素占比>80% 的块用邻域均值填充"""
    ds = gdal.Open(str(path))
    w, h = ds.RasterXSize, ds.RasterYSize
    nbx, nby = int(np.ceil(w / block)), int(np.ceil(h / block))
    a_grid = np.ones((3, nby, nbx), dtype=np.float32)
    b_grid = np.zeros((3, nby, nbx), dtype=np.float32)
    bs = block
    for iy in range(nby):
        for ix in range(nbx):
            x0, y0 = ix * bs, iy * bs
            ww, hh = min(bs, w - x0), min(bs, h - y0)
            arr = ds.ReadAsArray(x0, y0, ww, hh)
            m = arr[3] > 0
            if m.mean() < 0.2:
                a_grid[:, iy, ix] = np.nan; b_grid[:, iy, ix] = np.nan
                continue
            for b in range(3):
                v = arr[b][m].astype(np.float32)
                mu, sd = v.mean(), v.std() + 1e-3
                a = target_std[b] / sd
                a = float(np.clip(a, 0.4, 2.5))  # 稳定限幅
                a_grid[b, iy, ix] = a
                b_grid[b, iy, ix] = target_mean[b] - a * mu
    ds = None
    # 填充 nan 块（用行/列均值，兜底 1.0/0.0）
    for b in range(3):
        col_mean = np.nanmean(np.where(np.isnan(a_grid[b]), np.nan, a_grid[b]), axis=0)
        a_grid[b] = np.where(np.isnan(a_grid[b]), col_mean[None, :], a_grid[b])
        col_mean_b = np.nanmean(b_grid[b], axis=0)
        b_grid[b] = np.where(np.isnan(b_grid[b]), col_mean_b[None, :], b_grid[b])
        a_grid[b] = np.nan_to_num(a_grid[b], nan=1.0)
        b_grid[b] = np.nan_to_num(b_grid[b], nan=0.0)
    centers_x = (np.arange(nbx) + 0.5) * block
    centers_y = (np.arange(nby) + 0.5) * block
    return a_grid, b_grid, centers_x, centers_y

def apply_wallis(rgb, alpha, params, shape_hw):
    a_grid, b_grid, cx, cy = params
    H, W = shape_hw
    out = np.empty_like(rgb)
    for b in range(3):
        # 双线性上采样参数场到全图
        a_full = cv2.resize(a_grid[b], (W, H), interpolation=cv2.INTER_LINEAR)
        b_full = cv2.resize(b_grid[b], (W, H), interpolation=cv2.INTER_LINEAR)
        v = rgb[:, :, b] * a_full + b_full
        np.clip(v, 0, 255, out=v)
        out[:, :, b] = v
    return out

def rgb_to_lab_reinhard(rgb, alpha, target_stats_lab):
    """Reinhard: 线性RGB -> LMS(log) -> lαβ，按目标 (μ,σ) 平移缩放，转回"""
    # 简化实现：在 lαβ (log LMS) 空间传递
    rgb01 = np.clip(rgb / 255.0, 1e-4, 1)
    L = 0.3811*rgb01[:,:,0] + 0.5783*rgb01[:,:,1] + 0.0406*rgb01[:,:,2]
    M = 0.1967*rgb01[:,:,0] + 0.7244*rgb01[:,:,1] + 0.0782*rgb01[:,:,2]
    S = 0.0241*rgb01[:,:,0] + 0.1288*rgb01[:,:,1] + 0.8444*rgb01[:,:,2]
    l = (np.log(L) + np.log(M) + np.log(S)) / np.sqrt(3)
    a = (np.log(L) + np.log(M) - 2*np.log(S)) / np.sqrt(6)
    b = (np.log(L) - np.log(M)) / np.sqrt(2)
    out = np.empty_like(rgb)
    # 目标统计 (mean, std) per channel in lαβ，从参考影像计算
    (m_l, m_a, m_b), (s_l, s_a, s_b) = target_stats_lab
    l2 = (l - l[alpha].mean()) / (l[alpha].std() + 1e-6) * s_l + m_l
    a2 = (a - a[alpha].mean()) / (a[alpha].std() + 1e-6) * s_a + m_a
    b2 = (b - b[alpha].mean()) / (b[alpha].std() + 1e-6) * s_b + m_b
    L2 = np.exp(l2/np.sqrt(3) + a2/np.sqrt(6) + b2/np.sqrt(2))
    M2 = np.exp(l2/np.sqrt(3) + a2/np.sqrt(6) - b2/np.sqrt(2))
    S2 = np.exp(l2/np.sqrt(3) - a2/np.sqrt(3))
    out[:,:,0] = 4.4679*L2 - 3.5873*M2 + 0.1193*S2
    out[:,:,1] = -1.2186*L2 + 2.3809*M2 - 0.1624*S2
    out[:,:,2] = 0.0497*L2 - 0.2439*M2 + 1.2045*S2
    out = np.clip(out * 255.0, 0, 255)
    return out

def lab_stats(path, decim=4):
    rgb, alpha = read_scene(path, decim=decim)
    rgb01 = np.clip(rgb / 255.0, 1e-4, 1)
    L = 0.3811*rgb01[:,:,0] + 0.5783*rgb01[:,:,1] + 0.0406*rgb01[:,:,2]
    M = 0.1967*rgb01[:,:,0] + 0.7244*rgb01[:,:,1] + 0.0782*rgb01[:,:,2]
    S = 0.0241*rgb01[:,:,0] + 0.1288*rgb01[:,:,1] + 0.8444*rgb01[:,:,2]
    l = (np.log(L) + np.log(M) + np.log(S)) / np.sqrt(3)
    a = (np.log(L) + np.log(M) - 2*np.log(S)) / np.sqrt(6)
    b = (np.log(L) - np.log(M)) / np.sqrt(2)
    return ([l[alpha].mean(), a[alpha].mean(), b[alpha].mean()],
            [l[alpha].std(), a[alpha].std(), b[alpha].std()])

# ---------- 开始试验 ----------
print("== 统计基准（分块精确直方图）==")
t0 = time.time()
stats24 = {}
for s in SCENES_2024:
    stats24[s] = exact_stats(DATA / "2024" / s)
    print(f"  2024/{s}: mean={stats24[s][0].round(1)}, std={stats24[s][1].round(1)} ({time.time()-t0:.0f}s)")
stats_gf2 = exact_stats(GF2)
print(f"  2025 GF2: mean={stats_gf2[0].round(1)}, std={stats_gf2[1].round(1)}")

# 目标：2024 四景加权均值（自动均衡目标）
n_valid = None
target_mean = np.mean([stats24[s][0] for s in SCENES_2024], axis=0)
target_std = np.mean([stats24[s][1] for s in SCENES_2024], axis=0)
ref_hist = stats24[SCENES_2024[0]][2]  # 直方图匹配参考：第一景
print(f"目标(自动): mean={target_mean.round(1)}, std={target_std.round(1)}")

methods = ["global_adjust", "hist_match", "block_wallis", "reinhard"]
metrics = {"target_mean": target_mean.tolist(), "target_std": target_std.tolist(), "scenes": {}}

# Reinhard 目标：四景 lαβ 统计的均值
lab_list = [lab_stats(DATA / "2024" / s) for s in SCENES_2024]
target_lab = (np.mean([x[0] for x in lab_list], axis=0),
              np.mean([x[1] for x in lab_list], axis=0))
print(f"Reinhard 目标(lαβ): mean={np.round(target_lab[0],3)}, std={np.round(target_lab[1],3)}")

# ---------- 2024 四景：各方法处理 + 接缝台阶指标 ----------
# 内存安全：统计用精确直方图（分块、恒定内存）；方法应用与预览在 decim=8 降采样数据上做
seam_x_px = None  # 014/015 的接缝在影像内 ~105.5°
for s in SCENES_2024:
    check_mem(f"处理 {s} 前")
    p = DATA / "2024" / s
    rgb, alpha = read_scene(p, decim=PREVIEW_FACTOR)
    rec = {}
    # before
    rec["before"] = {"mean": [rgb[:,:,b][alpha].mean() for b in range(3)],
                     "std": [rgb[:,:,b][alpha].std() for b in range(3)]}
    for m in methods:
        t0 = time.time()
        rec[m] = {}
        if m == "global_adjust":
            gain = target_std / (stats24[s][1] + 1e-3)
            offset = target_mean - gain * stats24[s][0]
            out = apply_gain_offset(rgb, alpha, gain, offset)
            rec[m]["gain"] = gain.tolist(); rec[m]["offset"] = offset.tolist()
        elif m == "hist_match":
            luts = [cdf_match_lut(stats24[s][2][b], ref_hist[b]) for b in range(3)]
            out = apply_lut(rgb, alpha, luts)
        elif m == "block_wallis":
            params = block_wallis_params(p, target_mean, target_std)
            out = apply_wallis(rgb, alpha, params, rgb.shape[:2])
        elif m == "reinhard":
            out = rgb_to_lab_reinhard(rgb, alpha, target_lab)
        dt = time.time() - t0
        rec[m]["mean"] = [out[:,:,b][alpha].mean() for b in range(3)]
        rec[m]["std"] = [out[:,:,b][alpha].std() for b in range(3)]
        rec[m]["time_s"] = round(dt, 1)
        # 保存预览（数据已降采样，直接保存）
        cv2.imwrite(str(OUT / f"2024_{s[:-4]}_{m}.png"),
                    cv2.cvtColor(out.astype(np.uint8), cv2.COLOR_RGB2BGR))
        del out
    # 原始预览
    cv2.imwrite(str(OUT / f"2024_{s[:-4]}_original.png"),
                cv2.cvtColor(rgb.astype(np.uint8), cv2.COLOR_RGB2BGR))
    del rgb, alpha
    metrics["scenes"][s] = rec
    print(f"  {s}: 处理完成")

# 接缝台阶差：左景右缘 vs 右景左缘（018014 | 018015）
def seam_step(rec_l, rec_r, m):
    ml = np.array(rec_l[m if m in rec_l else "before"]["mean"])
    mr = np.array(rec_r[m if m in rec_r else "before"]["mean"])
    return float(np.abs(ml - mr).mean())

m_l, m_r = metrics["scenes"][SCENES_2024[0]], metrics["scenes"][SCENES_2024[1]]
print("\n== 2024 景间接缝台阶差（|ΔRGB均值|，越小越好）==")
for m in ["before"] + methods:
    key = m if m != "before" else "before"
    step = seam_step(m_l, m_r, key)
    print(f"  {m:15s}: {step:.2f}")
metrics["seam_step_vertical_pair"] = {m: seam_step(m_l, m_r, m if m != "before" else "before")
                                       for m in ["before"] + methods}

# ---------- GF2：对齐 2024 目标 ----------
print("\n== GF2 处理（对齐 2024 目标）==")
check_mem("GF2 处理前")
rgb, alpha = read_scene(GF2, decim=8)  # 5006x4303，float32 约 260MB，安全
rec = {"before": {"mean": [rgb[:,:,b][alpha].mean() for b in range(3)],
                  "std": [rgb[:,:,b][alpha].std() for b in range(3)]}}
for m in methods:
    t0 = time.time()
    if m == "global_adjust":
        gain = target_std / (stats_gf2[1] + 1e-3)
        offset = target_mean - gain * stats_gf2[0]
        out = apply_gain_offset(rgb, alpha, gain, offset)
    elif m == "hist_match":
        luts = [cdf_match_lut(stats_gf2[2][b], ref_hist[b]) for b in range(3)]
        out = apply_lut(rgb, alpha, luts)
    elif m == "block_wallis":
        # 对降采样数据做分块 Wallis（目标 = 2024 目标均值/std）
        a_grid = np.full((3, rgb.shape[0] // BLOCK + 2, rgb.shape[1] // BLOCK + 2), np.nan, np.float32)
        b_grid = np.full_like(a_grid, np.nan)
        nbx = rgb.shape[1] // BLOCK + 1
        nby = rgb.shape[0] // BLOCK + 1
        for iy in range(nby):
            for ix in range(nbx):
                y0, x0 = iy * BLOCK, ix * BLOCK
                blk = rgb[y0:y0+BLOCK, x0:x0+BLOCK]
                ma = alpha[y0:y0+BLOCK, x0:x0+BLOCK]
                if ma.mean() < 0.2:
                    continue
                for b in range(3):
                    v = blk[:, :, b][ma]
                    mu, sd = v.mean(), v.std() + 1e-3
                    a = float(np.clip(target_std[b] / sd, 0.4, 2.5))
                    a_grid[b, iy, ix] = a
                    b_grid[b, iy, ix] = target_mean[b] - a * mu
        for b in range(3):
            a_grid[b] = np.where(np.isnan(a_grid[b]), np.nanmean(a_grid[b], axis=0)[None, :], a_grid[b])
            b_grid[b] = np.where(np.isnan(b_grid[b]), np.nanmean(b_grid[b], axis=0)[None, :], b_grid[b])
            a_grid[b] = np.nan_to_num(a_grid[b], nan=1.0)
            b_grid[b] = np.nan_to_num(b_grid[b], nan=0.0)
        out = apply_wallis(rgb, alpha, (a_grid, b_grid,
                                        (np.arange(nbx) + 0.5) * BLOCK,
                                        (np.arange(nby) + 0.5) * BLOCK),
                           rgb.shape[:2])
    elif m == "reinhard":
        out = rgb_to_lab_reinhard(rgb, alpha, target_lab)
    dt = time.time() - t0
    rec[m] = {"mean": [out[:,:,b][alpha].mean() for b in range(3)],
              "std": [out[:,:,b][alpha].std() for b in range(3)],
              "time_s": round(dt, 1)}
    prev = out.astype(np.uint8)[::2, ::2]
    cv2.imwrite(str(OUT / f"gf2_{m}.png"), cv2.cvtColor(prev, cv2.COLOR_RGB2BGR))
    del out
cv2.imwrite(str(OUT / "gf2_original.png"), cv2.cvtColor(rgb.astype(np.uint8)[::2, ::2], cv2.COLOR_RGB2BGR))
metrics["gf2"] = rec
print("GF2 各方法 mean/std:")
for m in ["before"] + methods:
    r = rec[m]
    print(f"  {m:15s}: mean={np.round(r['mean'],1)}, std={np.round(r['std'],1)}")

(OUT / "metrics.json").write_text(
    json.dumps(metrics, indent=1, ensure_ascii=False, default=float), encoding="utf-8")
print(f"\n完成。预览图与指标输出: {OUT}")
