# -*- coding: utf-8 -*-
"""Phase 0 补充验证（审计发现的两项证据缺口）

A. 2024 四图幅对重叠区 Δμ 持久化（seam_deep_verify.py 只打印未落盘，报告 §4.1 数据源）
B. GF2(2025) × 2024 重叠区跨批次匀色验证：
   seam_deep_verify 已证明"全局统计目标会在本就接边连续的数据上制造接缝"，
   但 GF2 实验当时仍用全局目标（对齐 2024 全局统计）且未做接缝验证——同类风险未排查。
   本脚本在两者 ~32km×27km 真实重叠区上对比：
     - global_adjust：GF2 对齐 2024 全局 (μ,σ) 目标（复现原实验参数来源）
     - overlap_adjust：GF2 由重叠区统计解参
   指标 = 重叠区残差 Δμ（整体 + 4×4 分块空间分布，后者反映接缝台阶的真实空间结构）

内存安全（本机 9.7GB 约束）：全部窗口降采样读（≤1600px）；统计参数用分块直方图（恒定内存）。
"""
import json
import subprocess
import sys
from pathlib import Path

import numpy as np
from osgeo import gdal

gdal.UseExceptions()

DATA = Path("/home/tdt-dell/code/MyProject/image_process/文档资料/关于栅格切片和匀色/data")
SCENES = ["H48E018014C.tif", "H48E018015C.tif", "H48E019014C.tif", "H48E019015C.tif"]
GF2 = DATA / "2025/GF2_PMS1_E105.6_N29.0_RGB_20250324C.tif"
OUT = Path("/home/tdt-dell/code/MyProject/image_process/output/phase0/color")
TMP = Path("/tmp/p0")


def free_gb():
    with open("/proc/meminfo") as f:
        for line in f:
            if line.startswith("MemAvailable"):
                return int(line.split()[1]) / 1048576
    return -1


def check_mem(tag):
    g = free_gb()
    print(f"  [mem] {tag}: 可用 {g:.1f} GB")
    if g < 1.5:
        sys.exit(f"内存护栏：可用 {g:.1f}GB < 1.5GB，终止")


def extent(ds):
    gt = ds.GetGeoTransform()
    minx, maxy = gt[0], gt[3]
    maxx = gt[0] + gt[1] * ds.RasterXSize
    miny = gt[3] + gt[5] * ds.RasterYSize
    return minx, miny, maxx, maxy


def exact_stats(path):
    """分块精确统计（直方图法），返回 per-band (mean, std)（仅 alpha>0 像素）"""
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
    return mean, np.sqrt(var)


def read_geo_window(path, bounds, W, H):
    """按地理范围读取窗口并重采样到 W×H，返回 rgb(H,W,3) float32 + alpha bool"""
    ds = gdal.Open(str(path))
    gt = ds.GetGeoTransform()
    minx, miny, maxx, maxy = bounds
    px0 = int(np.floor((minx - gt[0]) / gt[1]))
    px1 = int(np.ceil((maxx - gt[0]) / gt[1]))
    py0 = int(np.floor((gt[3] - maxy) / -gt[5]))
    py1 = int(np.ceil((gt[3] - miny) / -gt[5]))
    px0, py0 = max(px0, 0), max(py0, 0)
    px1, py1 = min(px1, ds.RasterXSize), min(py1, ds.RasterYSize)
    arr = ds.ReadAsArray(px0, py0, px1 - px0, py1 - py0, buf_xsize=W, buf_ysize=H)
    ds = None
    rgb = np.transpose(arr[:3], (1, 2, 0)).astype(np.float32)
    alpha = arr[3] > 0
    return rgb, alpha


def band_stats(rgb, mask):
    """返回 per-band (mean, std)，仅 mask 像素"""
    out_m, out_s = np.zeros(3), np.zeros(3)
    for b in range(3):
        v = rgb[:, :, b][mask]
        out_m[b] = v.mean() if v.size else np.nan
        out_s[b] = v.std() if v.size else np.nan
    return out_m, out_s


result = {}

# ============ A. 2024 图幅对重叠区 Δμ 持久化 ============
print("== A. 2024 四图幅对重叠区统计（复算并持久化报告 §4.1）==")
pairs = [
    ("H48E018014C.tif", "H48E018015C.tif", "V上排"),
    ("H48E019014C.tif", "H48E019015C.tif", "V下排"),
    ("H48E018014C.tif", "H48E019014C.tif", "H左列"),
    ("H48E018015C.tif", "H48E019015C.tif", "H右列"),
]
overlap_2024 = {}
for sa, sb, tag in pairs:
    da, db = gdal.Open(str(DATA / "2024" / sa)), gdal.Open(str(DATA / "2024" / sb))
    ea, eb = extent(da), extent(db)
    b = (max(ea[0], eb[0]), max(ea[1], eb[1]), min(ea[2], eb[2]), min(ea[3], eb[3]))
    da = db = None
    if b[0] >= b[2] or b[1] >= b[3]:
        overlap_2024[tag] = None
        continue
    # 重叠条带全分辨率读取（条带很小：~23×9279 或 13911×20），保持真实宽高比
    W = max(1, int(round((b[2] - b[0]) / 1.8e-5)))
    H = max(1, int(round((b[3] - b[1]) / 1.8e-5)))
    rgbA, aA = read_geo_window(DATA / "2024" / sa, b, W, H)
    rgbB, aB = read_geo_window(DATA / "2024" / sb, b, W, H)
    m = aA & aB
    muA, _ = band_stats(rgbA, m)
    muB, _ = band_stats(rgbB, m)
    overlap_2024[tag] = {
        "overlap_px": f"{W}x{H}",
        "mu_A": [round(float(x), 2) for x in muA],
        "mu_B": [round(float(x), 2) for x in muB],
        "delta_mu": [round(float(x), 2) for x in muB - muA],
        "valid_ratio": round(float(m.mean()), 3),
    }
    print(f"  {tag}: Δμ(R/G/B) = {np.round(muB - muA, 2)}  有效比例 {m.mean():.2f}")
result["overlap_2024_pairs"] = overlap_2024

# ============ B. GF2 × 2024 重叠区跨批次验证 ============
print("\n== B. GF2(2025) × 2024 重叠区跨批次匀色验证 ==")
check_mem("B 开始")

# B1. 全局精确统计（分块直方图，恒定内存）
print("  [1/4] 精确统计（分块直方图）…")
stats24 = {s: exact_stats(DATA / "2024" / s) for s in SCENES}
check_mem("2024 统计后")
stats_gf2 = exact_stats(GF2)
check_mem("GF2 统计后")
target_mean = np.mean([stats24[s][0] for s in SCENES], axis=0)
target_std = np.mean([stats24[s][1] for s in SCENES], axis=0)

# global_adjust 参数（复现原实验：GF2 对齐 2024 全局目标）
gain_g = target_std / stats_gf2[1]
offset_g = target_mean - gain_g * stats_gf2[0]
print(f"  global_adjust 参数: gain={np.round(gain_g,3)} offset={np.round(offset_g,2)}")

# B2. 镶嵌 VRT + 重叠区窗口读取
print("  [2/4] 构建 2024 镶嵌 VRT 并读取重叠区窗口…")
mosaic_vrt = TMP / "gf2check_mosaic.vrt"
flist = TMP / "gf2check_filelist.txt"
flist.write_text("\n".join(str(DATA / "2024" / s) for s in SCENES) + "\n")
subprocess.run(["gdalbuildvrt", "-input_file_list", str(flist), str(mosaic_vrt)],
               check=True, capture_output=True)
dm, dg = gdal.Open(str(mosaic_vrt)), gdal.Open(str(GF2))
em, eg = extent(dm), extent(dg)
dm = dg = None
inter = (max(em[0], eg[0]), max(em[1], eg[1]), min(em[2], eg[2]), min(em[3], eg[3]))
if inter[0] >= inter[2] or inter[1] >= inter[3]:
    sys.exit("GF2 与 2024 无重叠？请检查范围。")
dx_km = (inter[2] - inter[0]) * 111.32 * np.cos(np.radians(29))
dy_km = (inter[3] - inter[1]) * 110.57
print(f"  重叠区: [{inter[0]:.4f},{inter[1]:.4f},{inter[2]:.4f},{inter[3]:.4f}] "
      f"约 {dx_km:.1f}km × {dy_km:.1f}km")
W = 1536
H = max(64, int(round(W * (inter[3] - inter[1]) / (inter[2] - inter[0]))))
rgbM, aM = read_geo_window(mosaic_vrt, inter, W, H)
rgbG, aG = read_geo_window(GF2, inter, W, H)
check_mem("窗口读取后")
m = aM & aG
print(f"  重叠区窗口 {W}×{H}，双侧有效比例 {m.mean():.2f}")

# B3. 重叠区统计与两种方法残差
print("  [3/4] 解算两种方法参数并计算残差…")
muM, sM = band_stats(rgbM, m)
muG, sG = band_stats(rgbG, m)
print(f"  重叠区实测: 2024 μ={np.round(muM,1)} σ={np.round(sM,1)}")
print(f"              GF2  μ={np.round(muG,1)} σ={np.round(sG,1)}")
print(f"  原始重叠区 Δμ = {np.round(muM - muG, 1)}  <- 跨批次真实辐射差（需要匀色）")

gain_o = sM / sG
offset_o = muM - gain_o * muG

def resid(rgb, gain, offset):
    adj = rgb * gain[None, None, :] + offset[None, None, :]
    np.clip(adj, 0, 255, out=adj)
    mu_adj, _ = band_stats(adj, m)
    return mu_adj - muM  # 相对 2024 重叠区基准的残差

resid_global = resid(rgbG, gain_g, offset_g)
resid_overlap = resid(rgbG, gain_o, offset_o)
print(f"  global_adjust 后重叠区残差 Δμ = {np.round(resid_global, 2)}  <- 接缝台阶风险")
print(f"  overlap_adjust 后重叠区残差 Δμ = {np.round(resid_overlap, 2)}")

# B4. 4×4 分块残差空间分布（反映接缝台阶的真实结构）
print("  [4/4] 4×4 分块残差空间分布…")
cells = {"global_adjust": [], "overlap_adjust": []}
adj_g = np.clip(rgbG * gain_g[None, None, :] + offset_g[None, None, :], 0, 255)
adj_o = np.clip(rgbG * gain_o[None, None, :] + offset_o[None, None, :], 0, 255)
for iy in range(4):
    for ix in range(4):
        sy, sx = slice(iy * H // 4, (iy + 1) * H // 4), slice(ix * W // 4, (ix + 1) * W // 4)
        mc = m[sy, sx]
        if mc.mean() < 0.05:
            cells["global_adjust"].append(None)
            cells["overlap_adjust"].append(None)
            continue
        row = {}
        for name, adj in [("global_adjust", adj_g), ("overlap_adjust", adj_o)]:
            dmu = np.array([adj[sy, sx, b][mc].mean() - rgbM[sy, sx, b][mc].mean()
                            for b in range(3)])
            row[name] = round(float(np.abs(dmu).mean()), 2)
        cells["global_adjust"].append(row["global_adjust"])
        cells["overlap_adjust"].append(row["overlap_adjust"])
cg = [c for c in cells["global_adjust"] if c is not None]
co = [c for c in cells["overlap_adjust"] if c is not None]
print(f"  分块 |Δμ| 均值: global={np.mean(cg):.2f}, overlap={np.mean(co):.2f}")
print(f"  分块 |Δμ| 最大: global={np.max(cg):.2f}, overlap={np.max(co):.2f}")

result["gf2_overlap"] = {
    "intersection_bounds": [round(x, 6) for x in inter],
    "size_km": [round(float(dx_km), 1), round(float(dy_km), 1)],
    "window": [W, H],
    "valid_ratio_both": round(float(m.mean()), 3),
    "mu_2024_overlap": [round(float(x), 2) for x in muM],
    "std_2024_overlap": [round(float(x), 2) for x in sM],
    "mu_gf2_overlap": [round(float(x), 2) for x in muG],
    "std_gf2_overlap": [round(float(x), 2) for x in sG],
    "delta_mu_before": [round(float(x), 2) for x in (muM - muG)],
    "global_adjust": {
        "gain": [round(float(x), 4) for x in gain_g],
        "offset": [round(float(x), 2) for x in offset_g],
        "residual_delta_mu": [round(float(x), 2) for x in resid_global],
    },
    "overlap_adjust": {
        "gain": [round(float(x), 4) for x in gain_o],
        "offset": [round(float(x), 2) for x in offset_o],
        "residual_delta_mu": [round(float(x), 2) for x in resid_overlap],
    },
    "cells_4x4_abs_delta_mu": cells,
    "cell_mean": {"global": round(float(np.mean(cg)), 2), "overlap": round(float(np.mean(co)), 2)},
    "cell_max": {"global": round(float(np.max(cg)), 2), "overlap": round(float(np.max(co)), 2)},
}

out_path = OUT / "gf2_overlap_metrics.json"
out_path.write_text(json.dumps(result, indent=1, ensure_ascii=False), encoding="utf-8")
print(f"\n完成。结果已保存: {out_path}")
