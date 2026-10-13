# -*- coding: utf-8 -*-
"""Phase 0.4 补充：将四景方法预览按地理位置拼成 2x2 拼接图（目视接缝评估）"""
from pathlib import Path

import cv2
import numpy as np
from osgeo import gdal

gdal.UseExceptions()

DATA = Path("/home/tdt-dell/code/MyProject/image_process/文档资料/关于栅格切片和匀色/data/2024")
OUT = Path("/home/tdt-dell/code/MyProject/image_process/output/phase0/color")
F = 8  # 与 color_algo_compare.py 一致的降采样系数

SCENES = ["H48E018014C.tif", "H48E018015C.tif", "H48E019014C.tif", "H48E019015C.tif"]

# 计算画布范围（经纬度）
meta = {}
minx = miny = 1e9; maxx = maxy = -1e9
for s in SCENES:
    ds = gdal.Open(str(DATA / s))
    gt = ds.GetGeoTransform()
    w, h = ds.RasterXSize, ds.RasterYSize
    x0, y0 = gt[0], gt[3]
    x1, y1 = gt[0] + gt[1] * w, gt[3] + gt[5] * h
    meta[s] = (x0, y0, x1, y1, w // F, h // F)
    minx, miny = min(minx, x0), min(miny, y1)
    maxx, maxy = max(maxx, x1), max(maxy, y0)
    ds = None

# 统一画布像素尺度：用第一景的预览每像素度数（四景同分辨率）
res_pp = (meta[SCENES[0]][2] - meta[SCENES[0]][0]) / meta[SCENES[0]][4]  # 经度°/预览像素
W = int((maxx - minx) / res_pp)
H = int((maxy - miny) / res_pp)
print(f"画布 {W}x{H}")

def paste(canvas, img, x0, y0, x1, y1, w, h):
    px = int((x0 - minx) / res_pp)
    py = int((maxy - y0) / res_pp)
    h_img, w_img = img.shape[:2]
    canvas[py:py + h_img, px:px + w_img] = img

suffixes = ["original"] + ["global_adjust", "hist_match", "block_wallis", "reinhard"]
for suf in suffixes:
    canvas = np.zeros((H, W, 3), dtype=np.uint8)
    for s in SCENES:
        p = OUT / f"2024_{s[:-4]}_{suf}.png"
        img = cv2.imread(str(p))
        if img is None:
            print(f"缺少 {p}，跳过"); continue
        x0, y0, x1, y1, w, h = meta[s]
        paste(canvas, img, x0, y0, x1, y1, w, h)
    out = OUT / f"mosaic_2024_{suf}.png"
    cv2.imwrite(str(out), canvas)
    print(f"-> {out.name} {canvas.shape}")
print("完成")
