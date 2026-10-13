# -*- coding: utf-8 -*-
"""Phase 0.2 白瓦片/无效值复现与对策验证（GF2 影像，33% 无效区）

试验矩阵：
  A. JPEG 切片（无 alpha 通道）        -> 预期：无效区瓦片变黑（RGB=0 显形）
  B. JPEG + --dst-nodata 255          -> 预期：无效区瓦片变白（= ArcGIS 常见白瓦片机制）
  C. PNG（默认带 alpha）               -> 预期：无效区透明，不显黑/白
  D. PNG + --skip-blank              -> 预期：全无效瓦片不落盘
  E. PNG + --nodata-values-pct-threshold 50 -> 预期：边缘瓦片整体透明（无黑晕/白晕）
结论输出：各方案下"无效区瓦片/边界瓦片"的实际像素统计。
"""
import json
import math
import os
import shutil
import subprocess
import sys
from pathlib import Path

from osgeo import gdal
import numpy as np

gdal.UseExceptions()

SRC = "/home/tdt-dell/code/MyProject/image_process/文档资料/关于栅格切片和匀色/data/2025/GF2_PMS1_E105.6_N29.0_RGB_20250324C.tif"
OUT = Path("/tmp/p0/wt")
OUT.mkdir(parents=True, exist_ok=True)
GDAL_DATA = "/tmp/p0/gdal_data"  # 复用 0.3 的临时数据目录

# --- 计算影像在 WebMercator z16..18 覆盖的瓦片范围（用于定位无效区瓦片）---
ds = gdal.Open(SRC)
gt = ds.GetGeoTransform()
minx, maxy = gt[0], gt[3]
maxx, miny = gt[0] + gt[1]*ds.RasterXSize, gt[3] + gt[5]*ds.RasterYSize
ds = None

srs4490 = osr4490 = None
from osgeo import osr
s4490 = osr.SpatialReference(); s4490.ImportFromEPSG(4490); s4490.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
s3857 = osr.SpatialReference(); s3857.ImportFromEPSG(3857); s3857.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
ct = osr.CoordinateTransformation(s4490, s3857)
p1 = ct.TransformPoint(minx, miny); p2 = ct.TransformPoint(maxx, maxy)
mmx0, mmy0 = min(p1[0], p2[0]), min(p1[1], p2[1])
mmx1, mmy1 = max(p1[0], p2[0]), max(p1[1], p2[1])

WORLD = 40075016.68557849  # WebMercator 世界宽度（米）
def tile_range(z):
    n = 2 ** z
    span = WORLD / n  # 单瓦片跨度（米）
    x0 = int(math.floor((mmx0 + WORLD/2) / span)); x1 = int(math.floor((mmx1 + WORLD/2) / span))
    y0 = int(math.floor((WORLD/2 - mmy1) / span)); y1 = int(math.floor((WORLD/2 - mmy0) / span))
    return x0, x1, y0, y1

# 找一个"中心点为无效像素"的 z18 瓦片与一个"跨有效/无效边界"的 z18 瓦片
ds = gdal.Open(SRC)
def lonlat_to_px(lon, lat):
    gx, gy = ct.TransformPoint(lon, lat)[0:2]  # 4490->3857 不对，需要像素：直接用源 GT
    return None

# 直接在源影像像素坐标找无效/边界位置
rng = np.random.default_rng(7)
found_nodata = found_edge = None
for _ in range(400):
    x = int(rng.integers(0, ds.RasterXSize - 64)); y = int(rng.integers(0, ds.RasterYSize - 64))
    a = ds.GetRasterBand(4).ReadAsArray(x, y, 64, 64)
    inv = (a == 0).mean()
    if found_nodata is None and inv > 0.999:
        found_nodata = (x + 32, y + 32)
    if found_edge is None and 0.35 < inv < 0.65:
        found_edge = (x + 32, y + 32)
    if found_nodata and found_edge:
        break
ds = None
print(f"无效区中心像素: {found_nodata}, 边界像素: {found_edge}")

def px_to_z18_tile(px, py):
    # 源像素 -> 4490 经纬度 -> 3857 米 -> z18 瓦片
    lon = gt[0] + gt[1] * px
    lat = gt[3] + gt[5] * py
    mx, my = ct.TransformPoint(lon, lat)[0:2]
    span = WORLD / (2 ** 18)
    tx = int((mx + WORLD/2) // span); ty = int((WORLD/2 - my) // span)
    return tx, ty

tile_nodata = px_to_z18_tile(*found_nodata)
tile_edge = px_to_z18_tile(*found_edge)
print(f"z18 无效区瓦片: {tile_nodata}, 边界瓦片: {tile_edge}")

# --- 试验切片（z17-z18 两级即可说明问题，控制总量）---
x0, x1, y0, y1 = tile_range(18)
print(f"GF2 覆盖 z18 瓦片范围: {x1-x0+1} x {y1-y0+1}")

cases = {
    "A_jpeg":        ["-f", "JPEG"],
    "B_jpeg_255":    ["-f", "JPEG", "--dst-nodata", "255"],
    "C_png":         ["-f", "PNG"],
    "D_png_skip":    ["-f", "PNG", "--skip-blank"],
    "E_png_th50":    ["-f", "PNG", "--resampling", "average", "--nodata-values-pct-threshold", "50"],
}
results = {}
for name, extra in cases.items():
    odir = OUT / name
    done = odir.exists() and any(odir.rglob("*.png")) or any(odir.rglob("*.jpg")) if odir.exists() else False
    if not done:
        shutil.rmtree(odir, ignore_errors=True)
        cmd = ["gdal", "raster", "tile", "-i", SRC, "-o", str(odir),
               "--tiling-scheme", "WebMercatorQuad", "--convention", "xyz",
               "--min-zoom", "17", "--max-zoom", "18", "-j", "8", *extra]
        r = subprocess.run(cmd, env=dict(os.environ, GDAL_DATA=GDAL_DATA),
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(f"[{name}] FAIL: {r.stderr[-300:]}"); sys.exit(1)
    results[name] = odir
    print(f"[{name}] 就绪")

# --- 检查各方案的无效区瓦片与边界瓦片 ---
def tile_stats(odir, tx, ty):
    z = 18
    p = Path(odir) / str(z) / str(tx) / f"{ty}.png"
    pj = Path(odir) / str(z) / str(tx) / f"{ty}.jpg"
    f = p if p.exists() else pj
    if not f.exists():
        return "不存在(被跳过)"
    im = gdal.Open(str(f))
    arr = im.ReadAsArray()
    im = None
    if arr.shape[0] == 4:  # PNG RGBA
        a = arr[3]
        return f"RGBA, alpha=0 比例 {(a==0).mean()*100:.0f}%, RGB均值(有效) " + \
               ",".join(f"{arr[i][a>0].mean():.0f}" for i in range(3))
    else:
        rgb = arr[:3]
        mean = rgb.mean(axis=(1, 2))
        white = ((rgb >= 250).all(axis=0)).mean() * 100
        black = ((rgb <= 5).all(axis=0)).mean() * 100
        return f"RGB, 均值 {mean[0]:.0f}/{mean[1]:.0f}/{mean[2]:.0f}, 纯白比例 {white:.0f}%, 纯黑比例 {black:.0f}%"

print("\n===== 无效区中心瓦片（z18）=====")
for name, odir in results.items():
    print(f"[{name}] {tile_stats(odir, *tile_nodata)}")
print("\n===== 边界瓦片（z18）=====")
for name, odir in results.items():
    print(f"[{name}] {tile_stats(odir, *tile_edge)}")

# 边界瓦片单独导出大图供目视检查
for name in ["A_jpeg", "B_jpeg_255", "C_png", "E_png_th50"]:
    p = results[name] / "18" / str(tile_edge[0])
    f = list(p.glob("*.png")) + list(p.glob("*.jpg"))
    if f:
        shutil.copy(f[0], OUT / f"edge_{name}{'.'+f[0].suffix.lstrip('.')}")
print(f"\n边界瓦片样例已导出到 {OUT}/edge_*")
