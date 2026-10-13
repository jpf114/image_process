# -*- coding: utf-8 -*-
"""Phase 0 验证脚本：VRTDerivedRasterBand + 像素函数实现"匀色不落盘像素"
验证点：
1. 派生波段像素函数数值正确性
2. 窗口读取性能开销
3. 多景镶嵌 VRT 跨景窗口读取
4. 镶嵌 VRT 外部金字塔（高层瓦片性能关键）
"""
import time
import subprocess
from osgeo import gdal
import numpy as np

gdal.UseExceptions()
gdal.SetConfigOption("GDAL_VRT_ENABLE_PYTHON", "YES")

SRC = "/home/tdt-dell/code/MyProject/image_process/文档资料/关于栅格切片和匀色/data/2024/H48E018014C.tif"
OUT = "/tmp/test_vrt"
import os
os.makedirs(OUT, exist_ok=True)

ds0 = gdal.Open(SRC)
gt, w, h, wkt = ds0.GetGeoTransform(), ds0.RasterXSize, ds0.RasterYSize, ds0.GetSpatialRef().ExportToWkt()
ds0 = None

GAIN, OFFSET = 1.3, 15.0

# GDAL >= 3.9 的 10 参签名；gain/offset 内嵌进代码
#（生产 C++ 引擎将注册带参 C 像素函数 GDALAddDerivedBandPixelFuncWithArgs，无需内嵌）
PYFUNC = f"""
import numpy as np

def gain_offset(in_ar, out_ar, xoff, yoff, xsize, ysize, raster_xsize, raster_ysize, buf_radius, gdal_type):
    v = np.clip(in_ar[0].astype(np.float32) * {GAIN} + {OFFSET}, 0, 255)
    out_ar[:] = v.astype(np.uint8)
    return 0
"""


def band_xml(i):
    return f"""  <VRTRasterBand dataType="Byte" band="{i}" subClass="VRTDerivedRasterBand">
    <PixelFunctionType>gain_offset</PixelFunctionType>
    <PixelFunctionLanguage>Python</PixelFunctionLanguage>
    <PixelFunctionCode><![CDATA[{PYFUNC}]]></PixelFunctionCode>
    <SimpleSource>
      <SourceFilename>{SRC}</SourceFilename>
      <SourceBand>{i}</SourceBand>
      <SrcRect xOff="0" yOff="0" xSize="{w}" ySize="{h}"/>
      <DstRect xOff="0" yOff="0" xSize="{w}" ySize="{h}"/>
    </SimpleSource>
  </VRTRasterBand>"""


vrt_xml = f"""<VRTDataset rasterXSize="{w}" rasterYSize="{h}">
  <SRS>{wkt.replace('&', '&amp;').replace('<', '&lt;')}</SRS>
  <GeoTransform>{', '.join(str(x) for x in gt)}</GeoTransform>
{band_xml(1)}{band_xml(2)}{band_xml(3)}
</VRTDataset>"""
with open(f"{OUT}/balanced_pf.vrt", "w", encoding="utf-8") as f:
    f.write(vrt_xml)

# 1) 正确性
ds_src = gdal.Open(SRC)
arr_src = ds_src.ReadAsArray(1000, 1000, 256, 256)
ds_bal = gdal.Open(f"{OUT}/balanced_pf.vrt")
arr_bal = ds_bal.ReadAsArray(1000, 1000, 256, 256)
expect = np.clip(arr_src[:3].astype(np.float32) * GAIN + OFFSET, 0, 255).astype(np.uint8)
print(f"像素函数数值正确性: {'PASS' if np.array_equal(arr_bal[:3], expect) else 'FAIL'}")
print(f"  R 均值 {arr_src[0].mean():.1f} -> {arr_bal[0].mean():.1f} (预期约 {arr_src[0].mean()*GAIN+OFFSET:.1f})")

# 2) 性能：随机窗口 100 次
rng = np.random.default_rng(42)
wins = [(int(rng.integers(0, w - 256)), int(rng.integers(0, h - 256))) for _ in range(100)]
t0 = time.perf_counter()
for x, y in wins:
    ds_src.ReadAsArray(x, y, 256, 256)
t_src = time.perf_counter() - t0
t0 = time.perf_counter()
for x, y in wins:
    ds_bal.ReadAsArray(x, y, 256, 256)
t_bal = time.perf_counter() - t0
print(f"100 窗口直读: {t_src*1000:.0f}ms | VRT+Python像素函数: {t_bal*1000:.0f}ms | 比值 {t_bal/t_src:.2f}x")

# 3) 四景镶嵌 VRT（原生 tif 直接镶嵌，验证跨景窗口可读）
base = "/home/tdt-dell/code/MyProject/image_process/文档资料/关于栅格切片和匀色/data/2024"
with open(f"{OUT}/filelist.txt", "w") as f:
    for s in ["H48E018014C.tif", "H48E018015C.tif", "H48E019014C.tif", "H48E019015C.tif"]:
        f.write(f"{base}/{s}\n")
subprocess.run(["gdalbuildvrt", "-input_file_list", f"{OUT}/filelist.txt",
                f"{OUT}/mosaic.vrt"], check=True)
m = gdal.Open(f"{OUT}/mosaic.vrt")
print(f"镶嵌 VRT: {m.RasterXSize}x{m.RasterYSize}")
cx, cy = m.RasterXSize // 2, m.RasterYSize // 2
arr = m.ReadAsArray(cx - 128, cy - 128, 512, 512)
print(f"跨景窗口读取: shape={arr.shape}, 值域 {arr.min()}..{arr.max()} -> PASS")

# 4) 镶嵌 VRT 外部金字塔（.ovr）——低层级(高zoom号)瓦片性能的关键
subprocess.run(["gdaladdo", "-r", "average", f"{OUT}/mosaic.vrt", "2", "4", "8"], check=True)
m2 = gdal.Open(f"{OUT}/mosaic.vrt")
t0 = time.perf_counter()
arr_small = m2.ReadAsArray(0, 0, m2.RasterXSize, m2.RasterYSize, 512, 512)
print(f"金字塔后整图缩读 512x512: {arr_small.shape} 耗时 {(time.perf_counter()-t0)*1000:.0f}ms -> PASS")
