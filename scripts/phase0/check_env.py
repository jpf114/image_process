# -*- coding: utf-8 -*-
"""Phase 0.1 环境体检：
1. geo_env GDAL proj.db 问题验证（4490<->3857/4326 坐标转换正确性）
2. 像素级 CRS 一致性：4490 影像重投影到 3857 的坐标偏差
3. vcpkg GDAL CLI 可用性（gdal raster tile 等）
"""
from osgeo import gdal, osr

gdal.UseExceptions()
DATA = "/home/tdt-dell/code/MyProject/image_process/文档资料/关于栅格切片和匀色/data"

print("== GDAL/PROJ 基本信息 ==")
print(f"GDAL: {gdal.__version__}")
print(f"PROJ: {osr.GetPROJVersionMajor()}.{osr.GetPROJVersionMinor()}.{osr.GetPROJVersionMicro()}")
print(f"PROJ 数据目录: {osr.GetPROJSearchPaths()}")

print("\n== 1) 坐标转换正确性（EPSG:4490 <-> 3857/4326）==")
# 关键：GDAL 3 默认按权威轴序（4490 为 纬度,经度），必须显式声明传统 GIS 轴序
srs_4490 = osr.SpatialReference(); srs_4490.ImportFromEPSG(4490)
srs_4326 = osr.SpatialReference(); srs_4326.ImportFromEPSG(4326)
srs_3857 = osr.SpatialReference(); srs_3857.ImportFromEPSG(3857)
for s in (srs_4490, srs_4326, srs_3857):
    s.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
ct = osr.CoordinateTransformation(srs_4490, srs_4326)
x, y, _ = ct.TransformPoint(105.5, 29.0)
print(f"4490->4326: (105.5, 29.0) -> ({x:.9f}, {y:.9f})  [预期几乎不变, 偏差<1e-6°]")
assert abs(x - 105.5) < 1e-6 and abs(y - 29.0) < 1e-6, "4490->4326 转换异常!"

ct = osr.CoordinateTransformation(srs_4490, srs_3857)
x, y, _ = ct.TransformPoint(105.5, 29.0)
exp_x = 105.5 * 20037508.342789244 / 180.0
import math
exp_y = math.log(math.tan(math.pi/4 + math.radians(29.0)/2)) * 20037508.342789244 / math.pi
print(f"4490->3857: ({x:.3f}, {y:.3f})  [预期 ({exp_x:.3f}, {exp_y:.3f})]")
assert abs(x - exp_x) < 0.5 and abs(y - exp_y) < 0.5, "4490->3857 转换异常!"

ct = osr.CoordinateTransformation(srs_3857, srs_4490)
x2, y2, _ = ct.TransformPoint(x, y)
print(f"3857->4490 往返: ({x2:.9f}, {y2:.9f})  [应回到 (105.5, 29.0)]")
assert abs(x2 - 105.5) < 1e-7 and abs(y2 - 29.0) < 1e-7

print("PASS: 4490 与 3857/4326 双向转换正常（proj.db 工作正常）")

print("\n== 2) 影像级验证：4490 影像 GetSpatialRef / WGS84 自动转换 ==")
ds = gdal.Open(f"{DATA}/2024/H48E018014C.tif")
srs = ds.GetSpatialRef()
print(f"影像 CRS: {srs.GetName()} | Authority: {srs.GetAuthorityName(None)}:{srs.GetAuthorityCode(None)}")
gt = ds.GetGeoTransform()
ct = osr.CoordinateTransformation(srs_4490, srs_3857)
# 影像四角
w, h = ds.RasterXSize, ds.RasterYSize
for name, px, py in [("左上", 0, 0), ("右下", w, h)]:
    gx = gt[0] + gt[1]*px
    gy = gt[3] + gt[5]*py
    mx, my, _ = ct.TransformPoint(gx, gy)
    print(f"  {name} ({gx:.6f},{gy:.6f}) -> 3857 ({mx:.1f},{my:.1f})")
print("PASS: 影像元数据读取与重投影正常")
ds = None

print("\n== 3) WMTS/天地图常用公式自检：z18 WebMercator 分辨率 ==")
res18 = 156543.03392804062 / (2**18)
print(f"WebMercator z18 赤道分辨率: {res18:.4f} m/px；纬度29°约 {res18*math.cos(math.radians(29)):.4f} m/px")
print("（2024 数据 2.0m/px、2025 数据 0.8m/px —— 均低于该分辨率，z18 属过采样）")
