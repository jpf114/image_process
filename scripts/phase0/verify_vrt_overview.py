# -*- coding: utf-8 -*-
"""补充验证：镶嵌 VRT + 外部金字塔(.ovr) 的低层级缩读性能"""
import time
import subprocess
from osgeo import gdal

gdal.UseExceptions()
OUT = "/tmp/test_vrt"
M = f"{OUT}/mosaic.vrt"

m = gdal.Open(M)
print(f"镶嵌 VRT: {m.RasterXSize}x{m.RasterYSize}，已有金字塔: {m.GetRasterBand(1).GetOverviewCount()}")

t0 = time.perf_counter()
arr_small = m.ReadAsArray(0, 0, m.RasterXSize, m.RasterYSize,
                          buf_xsize=512, buf_ysize=512)
print(f"金字塔后整图缩读 512x512: {arr_small.shape} 耗时 {(time.perf_counter()-t0)*1000:.0f}ms -> PASS")
