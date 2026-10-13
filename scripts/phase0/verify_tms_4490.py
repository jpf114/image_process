# -*- coding: utf-8 -*-
"""Phase 0.3 EPSG:4490（CGCS2000/天地图）切片方案验证
1. 生成 OGC TwoD TMS 标准的 tms_CGCS2000.json（层级从 1 起，z1=2x1 瓦片）
2. 用临时 GDAL_DATA（不改本地环境）让 gdal raster tile 识别该方案并对 2024 四景切片
3. 数学验证：瓦片边界与天地图公式一致；与 WorldCRS84Quad 层级恰好偏移 1
"""
import json
import math
import os
import shutil
import subprocess
import sys
from pathlib import Path

from osgeo import gdal

gdal.UseExceptions()

DATA = Path("/home/tdt-dell/code/MyProject/image_process/文档资料/关于栅格切片和匀色/data/2024")
OUT = Path("/tmp/p0")
GDAL_DATA_SRC = "/home/tdt-dell/miniconda3/envs/geo_env/share/gdal"
M_PER_DEG = 2 * math.pi * 6378137 / 360.0  # 111319.49079327358
OGC_PIXEL_M = 0.28e-3

OUT.mkdir(parents=True, exist_ok=True)

# ---------- 1) 生成 tms_CGCS2000.json ----------
def build_tms(max_level=24):
    # 关键：EPSG:4490 为 lat-lon 轴序，OGC TMS JSON 的 topLeftCorner 必须按 CRS 轴序写 [90,-180]
    # （GDAL 解析后内部会 swap 回 lon-lat；写 [-180,90] 会报"Extent ... not compatible"）
    mats = []
    for n in range(1, max_level + 1):
        res_deg = 0.703125 / 2 ** (n - 1)
        scale = res_deg * M_PER_DEG / OGC_PIXEL_M
        mats.append({
            "type": "TileMatrixType",
            "identifier": str(n),
            "scaleDenominator": scale,
            "topLeftCorner": [90.0, -180.0],
            "tileWidth": 256,
            "tileHeight": 256,
            "matrixWidth": 2 ** n,
            "matrixHeight": 2 ** (n - 1),
        })
    return {
        "type": "TileMatrixSetType",
        "identifier": "CGCS2000",
        "title": "CGCS2000 Geographic 2D Tile Matrix Set (TianDiTu convention, level starts at 1)",
        "supportedCRS": "http://www.opengis.net/def/crs/EPSG/0/4490",
        "wellKnownScaleSet": "urn:ogc:def:wkss:OGC:1.0:GoogleCRS84Quad",
        "tileMatrix": mats,
    }

gdal_data_tmp = OUT / "gdal_data"
if not gdal_data_tmp.exists():
    # 用符号链接复制，避免拷贝大文件；再叠加我们的 tms
    gdal_data_tmp.mkdir()
    for f in Path(GDAL_DATA_SRC).iterdir():
        try:
            os.symlink(f, gdal_data_tmp / f.name)
        except FileExistsError:
            pass
tms_path = gdal_data_tmp / "tms_CGCS2000.json"
tms_path.write_text(json.dumps(build_tms(), indent=1), encoding="utf-8")
print(f"tms_CGCS2000.json 写入: {tms_path}")

# ---------- 2) 镶嵌 VRT ----------
mosaic = OUT / "mosaic4490.vrt"
scenes = [str(DATA / s) for s in
          ["H48E018014C.tif", "H48E018015C.tif", "H48E019014C.tif", "H48E019015C.tif"]]
flist = OUT / "filelist.txt"
flist.write_text("\n".join(scenes) + "\n", encoding="utf-8")
subprocess.run(["gdalbuildvrt", "-input_file_list", str(flist), str(mosaic)],
               check=True, capture_output=True)
ds = gdal.Open(str(mosaic))
gt = ds.GetGeoTransform()
minx, maxy = gt[0], gt[3]
maxx = gt[0] + gt[1] * ds.RasterXSize
miny = gt[3] + gt[5] * ds.RasterYSize
ds = None
print(f"镶嵌范围: [{minx:.6f},{miny:.6f},{maxx:.6f},{maxy:.6f}]")

# ---------- 3) 数学验证函数 ----------
def expected_tiles(level, minx, miny, maxx, maxy):
    """天地图 level n: 瓦片经度宽 = 180/2^(n-1)，返回覆盖的 x,y 索引范围"""
    span = 180.0 / 2 ** (level - 1)
    x0 = int(math.floor((minx + 180.0) / span)); x1 = int(math.floor((maxx - 1e-12 + 180.0) / span))
    y0 = int(math.floor((90.0 - maxy) / span));  y1 = int(math.floor((90.0 - miny - 1e-12) / span))
    return x0, x1, y0, y1

# ---------- 4) gdal raster tile 实际切片（z10-z14, xyz 命名）----------
env = dict(os.environ, GDAL_DATA=str(gdal_data_tmp))
tiles_root = OUT / "tiles_cgcs2000"
shutil.rmtree(tiles_root, ignore_errors=True)
cmd = ["gdal", "raster", "tile", "-i", str(mosaic), "-o", str(tiles_root),
       "--tiling-scheme", "CGCS2000", "--convention", "xyz",
       "--min-zoom", "9", "--max-zoom", "13", "-f", "PNG", "-j", "4"]  # CLI zoom=索引, 目录9..13 = identifier 10..14
r = subprocess.run(cmd, env=env, capture_output=True, text=True)
if r.returncode != 0:
    print("STDERR:", r.stderr[-2000:]); sys.exit(1)

# 统计各层实际生成的瓦片索引范围
# 注意：GDAL CLI 按矩阵数组索引(0起)命名输出目录、忽略 identifier 字符串，
# 故 identifier=n 的几何出现在输出目录 z=n-1；自研引擎将按 identifier（天地图层级）命名。
ok = True
for z in range(10, 15):
    zdir = tiles_root / str(z - 1)  # 输出目录 z-1 == identifier z
    xs = sorted(int(d) for d in os.listdir(zdir)) if zdir.exists() else []
    ys = set()
    for xd in xs:
        ys |= {int(Path(f).stem) for f in os.listdir(zdir / str(xd))}
    ex0, ex1, ey0, ey1 = expected_tiles(z, minx, miny, maxx, maxy)
    match = bool(xs) and xs[0] == ex0 and xs[-1] <= ex1 and min(ys) == ey0 and max(ys) <= ey1
    n_real = sum(len(os.listdir(zdir / str(x))) for x in xs) if xs else 0
    n_expect = (ex1 - ex0 + 1) * (ey1 - ey0 + 1)
    print(f"天地图 z{z}(目录{z-1}): 实际 x[{xs[0] if xs else '-'}..{xs[-1] if xs else '-'}] y[{min(ys) if ys else '-'}..{max(ys) if ys else '-'}] "
          f"数量{n_real}(剔空) | 预期 x[{ex0}..{ex1}] y[{ey0}..{ey1}] 数量{n_expect} -> {'PASS' if match else 'FAIL'}")
    ok &= match

# ---------- 5) 与 WorldCRS84Quad 对比：同级索引几何应一致 ----------
tiles_root2 = OUT / "tiles_crs84"
shutil.rmtree(tiles_root2, ignore_errors=True)
cmd2 = ["gdal", "raster", "tile", "-i", str(mosaic), "-o", str(tiles_root2),
        "--tiling-scheme", "WorldCRS84Quad", "--convention", "xyz",
        "--min-zoom", "9", "--max-zoom", "13", "-f", "PNG", "-j", "4"]
r2 = subprocess.run(cmd2, env=env, capture_output=True, text=True)
if r2.returncode != 0:
    print("STDERR:", r2.stderr[-2000:]); sys.exit(1)
equiv = True
for z in range(10, 14):
    d1 = tiles_root / str(z - 1)   # CGCS2000 identifier z
    d2 = tiles_root2 / str(z - 1)  # WorldCRS84Quad level z-1（同为索引 z-1）
    set1 = {(x, Path(f).stem) for x in os.listdir(d1) for f in os.listdir(d1 / x)}
    set2 = {(x, Path(f).stem) for x in os.listdir(d2) for f in os.listdir(d2 / x)}
    same = set1 == set2
    print(f"CGCS2000 天地图z{z} vs WorldCRS84Quad z{z-1}: 瓦片集合 {'一致' if same else '不一致'} -> {'PASS' if same else 'FAIL'}")
    equiv &= same

print("\n结论: 天地图 CGCS2000 方案 =", "PASS" if (ok and equiv) else "FAIL",
      "(几何格网 == WorldCRS84Quad, identifier 层级从 1 起, z1 全球 2x1)")
