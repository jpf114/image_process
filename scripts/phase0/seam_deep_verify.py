# -*- coding: utf-8 -*-
"""接缝问题深度验证：
1. 在已生成的拼接预览图上，量测四条内部接缝的【局部台阶】（逐行左右均值差）
2. 全分辨率下量测相邻景【重叠区】的真实辐射关系（Δμ_overlap）
3. 解释"全局平差为什么制造接缝"：接缝处局部亮度 L 的调节公式推演 + 实测
4. 实现【重叠区链式平差】（生产方案的正确形态）并重出拼接图对比
5. 算法正确性抽查（hist_match 的 CDF 一致性）
"""
import json
from pathlib import Path

import cv2
import numpy as np
from osgeo import gdal

gdal.UseExceptions()

DATA = Path("/home/tdt-dell/code/MyProject/image_process/文档资料/关于栅格切片和匀色/data")
OUT = Path("/home/tdt-dell/code/MyProject/image_process/output/phase0/color")
F = 8
SCENES = ["H48E018014C.tif", "H48E018015C.tif", "H48E019014C.tif", "H48E019015C.tif"]
P2024 = DATA / "2024"

# ---------- 几何：与 stitch_previews 相同的画布计算 ----------
meta, minx, miny, maxx, maxy = {}, 1e9, 1e9, -1e9, -1e9
for s in SCENES:
    ds = gdal.Open(str(P2024 / s)); gt = ds.GetGeoTransform()
    w, h = ds.RasterXSize, ds.RasterYSize
    meta[s] = dict(x0=gt[0], y0=gt[3], x1=gt[0]+gt[1]*w, y1=gt[3]+gt[5]*h, w=w//F, h=h//F)
    minx, miny = min(minx, meta[s]["x0"]), min(miny, meta[s]["y1"])
    maxx, maxy = max(maxx, meta[s]["x1"]), max(maxy, meta[s]["y0"])
    ds = None
res_pp = (meta[SCENES[0]]["x1"] - meta[SCENES[0]]["x0"]) / meta[SCENES[0]]["w"]
W, H = int((maxx-minx)/res_pp), int((maxy-miny)/res_pp)

def px_of(lon): return (lon - minx) / res_pp
def py_of(lat): return (maxy - lat) / res_pp

# 四条接缝：V(014|015) x≈105.5; H(014|019014) y≈29.000; H(015|019015) 同 y; V(019014|019015) 同 x
SEAMS = {
    "V_014_015":   ("v", px_of(105.4998)),
    "H_014_19014": ("h", py_of(29.000180)),
    "H_015_19015": ("h", py_of(29.000180)),
    "V_19014_19015": ("v", px_of(105.499795)),
}

def seam_local_step(img, kind, pos, half=60, inner=12):
    """接缝局部台阶：沿缝逐行/列取两侧 inner 像素均值差，返回 |Δ| 的中位数与均值（RGB亮度）"""
    gray = img.astype(np.float32).mean(axis=2)
    steps = []
    pos = int(round(pos))
    if kind == "v":
        for y in range(half, img.shape[0]-half, 4):
            row = gray[y]
            if pos-inner-2 < 0 or pos+inner+2 >= img.shape[1]: continue
            l = row[pos-inner-8:pos-2].mean()   # 左侧（避开粘贴重叠）
            r = row[pos+4:pos+inner+8].mean()
            steps.append(r - l)
    else:
        for x in range(half, img.shape[1]-half, 4):
            col = gray[:, x]
            if pos-inner-2 < 0 or pos+inner+2 >= img.shape[0]: continue
            t = col[pos-inner-8:pos-2].mean()
            b = col[pos+4:pos+inner+8].mean()
            steps.append(b - t)
    st = np.array(steps)
    return float(np.median(st)), float(np.abs(st).mean()), float(np.std(st))

# 重叠区取反方向偏移：014|015 重叠在缝左侧（015 从 105.4998 开始，014 到 105.5002）
SEAM_OFFSET = {"V_014_015": +6, "V_19014_19015": +6, "H_014_19014": +6, "H_015_19015": +6}

print("== 1) 拼接预览图上的局部接缝台阶（中位数 |Δ|，亮度 0-255）==")
mosaics = ["original", "global_adjust", "hist_match", "block_wallis", "reinhard"]
table = {}
for m in mosaics:
    img = cv2.cvtColor(cv2.imread(str(OUT / f"mosaic_2024_{m}.png")), cv2.COLOR_BGR2RGB)
    row = {}
    for name, (kind, pos) in SEAMS.items():
        med, mean_abs, std = seam_local_step(img, kind, pos + SEAM_OFFSET[name])
        row[name] = round(med, 2)
    table[m] = row
    print(f"  {m:15s}: " + "  ".join(f"{k.split('_')[0]}{v:+.2f}" for k, v in row.items()))

# ---------- 2) 全分辨率重叠区真实辐射关系 ----------
print("\n== 2) 相邻景重叠区实测（全分辨率，同地物双景差）==")
def overlap_stats(sa, sb):
    """返回两景在重叠区的逐波段 μ 与差"""
    da, db = gdal.Open(str(P2024/sa)), gdal.Open(str(P2024/sb))
    ga, gb = da.GetGeoTransform(), db.GetGeoTransform()
    # 重叠区（经纬度）
    ox0 = max(ga[0], gb[0]); oy1 = min(ga[3], gb[3])
    ox1 = min(ga[0]+ga[1]*da.RasterXSize, gb[0]+gb[1]*db.RasterXSize)
    oy0 = max(ga[3]+ga[5]*da.RasterYSize, gb[3]+gb[5]*db.RasterYSize)
    if ox1 <= ox0 or oy1 <= oy0:
        return None
    # 转 A 景像素窗口
    ax0 = int((ox0-ga[0])/ga[1]); ax1 = int((ox1-ga[0])/ga[1])
    ay0 = int((oy1-ga[3])/ga[5]); ay1 = int((oy0-ga[3])/ga[5])
    arrA = da.ReadAsArray(max(ax0,0), max(ay0,0), ax1-ax0, ay1-ay0)
    bx0 = int((ox0-gb[0])/gb[1]); bx1 = int((ox1-gb[0])/gb[1])
    by0 = int((oy1-gb[3])/gb[5]); by1 = int((oy0-gb[3])/gb[5])
    arrB = db.ReadAsArray(max(bx0,0), max(by0,0), bx1-bx0, by1-by0)
    n = min(arrA.shape[1], arrB.shape[1]); nn = min(arrA.shape[2], arrB.shape[2])
    A = arrA[:, :n, :nn].astype(np.float32); B = arrB[:, :n, :nn].astype(np.float32)
    ma = A.mean(axis=(1,2)); mb = B.mean(axis=(1,2))
    da = None; db = None
    return ma, mb, (ax1-ax0), (ay1-ay0)

pairs = [("H48E018014C.tif","H48E018015C.tif","V上排"),
         ("H48E019014C.tif","H48E019015C.tif","V下排"),
         ("H48E018014C.tif","H48E019014C.tif","H左列"),
         ("H48E018015C.tif","H48E019015C.tif","H右列")]
ov = {}
for sa, sb, tag in pairs:
    r = overlap_stats(sa, sb)
    if r:
        ma, mb, ow, oh = r
        ov[tag] = dict(mu_A=ma.tolist(), mu_B=mb.tolist())
        print(f"  {tag} ({sa[:-4]}|{sb[:-4]}) 重叠 {ow}x{oh}px: "
              f"μ_A={ma.round(1)} μ_B={mb.round(1)} Δμ={np.round(mb-ma,2)}")

# ---------- 3) 全局平差在接缝处制造台阶的定量推演 ----------
print("\n== 3) 推演：全局平差对接缝局部亮度的影响（R 波段）==")
# 用上排 V 接缝：014 左、015 右；重叠区局部亮度 L_A、L_B（重叠区均值，仅 RGB 前 3 波段）
muA = np.array(ov["V上排"]["mu_A"][:3]); muB = np.array(ov["V上排"]["mu_B"][:3])
target_mu = np.array([89.8, 92.2, 74.4]); target_std = np.array([23.1, 19.5, 20.6])
s14 = (np.array([81.3, 85.0, 67.0]), np.array([21.9, 18.9, 19.5]))
s15 = (np.array([95.4, 96.1, 75.7]), np.array([23.0, 18.6, 19.2]))
g14 = target_std/s14[1]; o14 = target_mu - g14*s14[0]
g15 = target_std/s15[1]; o15 = target_mu - g15*s15[0]
L = muA  # 接缝左侧局部亮度
print(f"  重叠区局部亮度 L_A={muA.round(1)} L_B={muB.round(1)} (原始局部台阶 Δ={np.round(muB-muA,2)})")
print(f"  014 调节: L'={np.round(g14*L+o14,1)}   015 调节: L'={np.round(g15*muB+o15,1)}")
print(f"  平差后接缝局部台阶 = {np.round(g15*muB+o15 - (g14*L+o14),2)}  (原始为 {np.round(muB-muA,2)})")

# ---------- 4) 重叠区链式平差（生产正确形态）+ 重出预览 ----------
print("\n== 4) 重叠区链式平差（以 014 为参考，链式对齐邻居）==")
def read_scene(path, decim=1):
    ds = gdal.Open(str(path)); w, h = ds.RasterXSize, ds.RasterYSize
    arr = ds.ReadAsArray(0, 0, w, h, buf_xsize=w//decim, buf_ysize=h//decim)
    ds = None
    return np.transpose(arr[:3], (1,2,0)).astype(np.float32), arr[3] > 0

def overlap_window(sa, sb, decim):
    """返回 A 景坐标下与 B 景重叠的像素窗口"""
    da = gdal.Open(str(P2024/sa)); db = gdal.Open(str(P2024/sb))
    ga, gb = da.GetGeoTransform(), db.GetGeoTransform()
    ox0 = max(ga[0], gb[0]); oy1 = min(ga[3], gb[3])
    ox1 = min(ga[0]+ga[1]*da.RasterXSize, gb[0]+gb[1]*db.RasterXSize)
    oy0 = max(ga[3]+ga[5]*da.RasterYSize, gb[3]+gb[5]*db.RasterYSize)
    da = None; db = None
    if ox1 <= ox0 or oy1 <= oy0: return None
    ax0 = max(int((ox0-ga[0])/ga[1]), 0); ax1 = int((ox1-ga[0])/ga[1])
    ay0 = max(int((oy1-ga[3])/ga[5]), 0); ay1 = int((oy0-ga[3])/ga[5])
    return [v//decim for v in (ax0, ay0, ax1-ax0, ay1-ay0)]

def scene_stats_window(path, win=None, decim=1):
    ds = gdal.Open(str(path)); w, h = ds.RasterXSize, ds.RasterYSize
    if win is None:
        arr = ds.ReadAsArray(0, 0, w, h, buf_xsize=w//decim, buf_ysize=h//decim)
    else:
        x, y, ww, hh = win
        arr = ds.ReadAsArray(x, y, ww, hh)
    ds = None
    m = arr[3] > 0
    out = []
    for b in range(3):
        v = arr[b][m].astype(np.float32)
        out.append((v.mean(), v.std()))
    return out  # [(mu, std) x3]

# 链式：参考 014；015 对 014(V)，019014 对 014(H)，019015 对 015(V 下排)（已调参链）
ref = SCENES[0]
params_chain = {ref: (np.ones(3), np.zeros(3))}
chain = [("H48E018015C.tif", ref), ("H48E019014C.tif", ref), ("H48E019015C.tif", "H48E018015C.tif")]
for s, anchor in chain:
    win = overlap_window(s, anchor, 1)
    st_s = scene_stats_window(P2024/s, win)          # s 在重叠区的 (μ,σ)
    win_a = overlap_window(anchor, s, 1)
    st_a = scene_stats_window(P2024/anchor, win_a)   # anchor 在重叠区的 (μ,σ)
    # anchor 已有的调节参数（链式累计）
    ga_, oa_ = params_chain[anchor]
    # 目标：调节后 s 的重叠区统计 == 调节后 anchor 的重叠区统计
    tgt_mu = np.array([ga_[b]*st_a[b][0] + oa_[b] for b in range(3)])
    tgt_sd = np.array([ga_[b]*st_a[b][1] for b in range(3)])
    g = np.array([tgt_sd[b]/(st_s[b][1]+1e-3) for b in range(3)])
    o = np.array([tgt_mu[b] - g[b]*st_s[b][0] for b in range(3)])
    params_chain[s] = (g, o)
    print(f"  {s[:-4]} <- {anchor[:-4]}: gain={g.round(3)} offset={o.round(2)} (仅用重叠区{'%dx%d' % (win[2], win[3])}统计)")

# 生成链式平差的四景预览 + 拼接
canvas = np.zeros((H, W, 3), dtype=np.uint8)
for s in SCENES:
    rgb, alpha = read_scene(P2024/s, decim=F)
    g, o = params_chain[s]
    out = np.clip(rgb*g[None,None,:] + o[None,None,:], 0, 255).astype(np.uint8)
    m = meta[s]
    px = int((m["x0"]-minx)/res_pp); py = int((maxy-m["y0"])/res_pp)
    canvas[py:py+out.shape[0], px:px+out.shape[1]] = out
cv2.imwrite(str(OUT / "mosaic_2024_overlap_chain.png"), cv2.cvtColor(canvas, cv2.COLOR_RGB2BGR))
img_chain = canvas
med, mabs, std = seam_local_step(img_chain, "v", px_of(105.4998)+6)
print(f"  链式平差后 V 上排接缝台阶: 中位数 {med:+.2f}")
row = {}
for name, (kind, pos) in SEAMS.items():
    med, mabs, _ = seam_local_step(img_chain, kind, pos + SEAM_OFFSET[name])
    row[name] = round(med, 2)
table["overlap_chain"] = row
print(f"  {'overlap_chain':15s}: " + "  ".join(f"{k.split('_')[0]}{v:+.2f}" for k, v in row.items()))

# ---------- 5) hist_match 算法正确性抽查 ----------
print("\n== 5) hist_match CDF 一致性抽查（015 处理后 vs 参考景）==")
def cdf_match_lut(src_hist, ref_hist):
    cdf_s = np.cumsum(src_hist)/max(src_hist.sum(),1)
    cdf_r = np.cumsum(ref_hist)/max(ref_hist.sum(),1)
    lut = np.zeros(256, dtype=np.uint8); j = 0
    for i in range(256):
        while j < 255 and cdf_r[j] < cdf_s[i]: j += 1
        lut[i] = j
    return lut
ds = None
# 简化：用降采样直方图
rgb0, _ = read_scene(P2024/SCENES[0], decim=8)
rgb1, a1 = read_scene(P2024/SCENES[1], decim=8)
for b, ch in enumerate("RGB"):
    h_ref = np.bincount(rgb0[:,:,b].astype(np.uint8).ravel(), minlength=256)
    h_src = np.bincount(rgb1[:,:,b][a1].astype(np.uint8), minlength=256)
    lut = cdf_match_lut(h_src, h_ref)
    balanced = lut[rgb1[:,:,b].astype(np.uint8)][a1]
    cdf_ref = np.cumsum(h_ref)/h_ref.sum()
    h_bal = np.bincount(balanced, minlength=256)
    cdf_bal = np.cumsum(h_bal)/max(h_bal.sum(),1)
    print(f"  {ch}: 处理后 CDF 与参考最大偏差 {np.abs(cdf_bal-cdf_ref).max()*100:.1f}%")

# 保存指标
table_ser = {k: {kk: float(vv) for kk, vv in v.items()} for k, v in table.items()}
(OUT / "seam_metrics.json").write_text(json.dumps(table_ser, indent=1, ensure_ascii=False), encoding="utf-8")
print("\n完成。seam_metrics.json 已保存")
