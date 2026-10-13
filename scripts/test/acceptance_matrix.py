#!/usr/bin/env python3
# 综合验收测试矩阵：CLI 参数校验与异常、功能矩阵（坐标系/格式/nodata/续跑/方案模式）、
# 匀色方法耗时；输出 output/test_matrix/report.json 作为《测试文档》的一手数据。
#
# 运行：conda run --no-capture-output -n geo_env python scripts/test/acceptance_matrix.py
import json
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BIN = os.path.join(ROOT, "bin/Release")
OUT = os.path.join(ROOT, "output/test_matrix")
DATA24 = "文档资料/关于栅格切片和匀色/data/2024"
DATA25 = "文档资料/关于栅格切片和匀色/data/2025"
GF2 = f"{DATA25}/GF2_PMS1_E105.6_N29.0_RGB_20250324C.tif"
TMS_JSON = "operators/tiling/data/tms_CGCS2000.json"
VCPKG = os.environ.get("VCPKG_ROOT") or os.path.expanduser("~/vcpkg")
INST = os.path.join(VCPKG, "installed/x64-linux")


def env():
    e = dict(os.environ)
    e["LD_LIBRARY_PATH"] = os.path.join(INST, "lib") + ":" + e.get("LD_LIBRARY_PATH", "")
    e["GDAL_DATA"] = os.path.join(INST, "share/gdal")
    e["PROJ_DATA"] = os.path.join(INST, "share/proj")
    e["PROJ_LIB"] = os.path.join(INST, "share/proj")
    e.pop("GDAL_DRIVER_PATH", None)
    return e


def run(args, timeout=600):
    t0 = time.time()
    p = subprocess.run(args, cwd=ROOT, env=env(), stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, timeout=timeout)
    out = p.stdout.decode("utf-8", "replace")
    return p.returncode, out, round(time.time() - t0, 2)


def count_ext(d, ext):
    n = 0
    for r, _, fs in os.walk(d):
        n += sum(1 for f in fs if f.endswith(ext))
    return n


RESULTS = []
SKIP_SLOW = os.environ.get("IP_TEST_SKIP_SLOW") == "1"


def case(name, args, expect_rc, check=None, timeout=600):
    """expect_rc: 期望退出码集合；check: (rc, out, tag) -> (ok, detail)"""
    # 大耗时用例可按需跳过（IP_TEST_SKIP_SLOW=1），其既有结果从旧报告合并保留
    if SKIP_SLOW and (name.startswith("C ") or any(
            k in name for k in ("hist_match", "block_wallis", "cog_mosaic", "scene_format=cog"))):
        print("SKIP " + name + "（大耗时，保留旧结果）", flush=True)
        return True
    rc, out, sec = run(args, timeout)
    ok = rc in expect_rc
    detail = f"rc={rc} 期望∈{sorted(expect_rc)} 耗时{sec}s"
    if ok and check:
        cok, cdetail = check(rc, out, name)
        ok = cok
        detail += " | " + cdetail
    if not ok:
        detail += " | 输出尾: " + out.strip().splitlines()[-1][:160] if out.strip() else ""
    RESULTS.append({"case": name, "ok": ok, "rc": rc, "sec": sec, "detail": detail,
                    "cmd": " ".join(args[1:])})
    print(("PASS " if ok else "FAIL ") + name + " :: " + detail, flush=True)
    return ok


def tails_has(needle):
    def f(rc, out, tag):
        return (needle in out), f"含'{needle}'={'是' if needle in out else '否'}"
    return f


# ---------- 打包（S4）校验辅助 ----------

def sql_rows(path, sql):
    import sqlite3
    c = sqlite3.connect(path)
    try:
        return list(c.execute(sql))
    finally:
        c.close()


def pkg_tile_diff(pkg_path, ref_dir, table, zoom_shift, tms_row):
    """包内瓦片与散文件参照逐像素比对（PNG 无损，期望完全相同）。

    table: 瓦片表名；zoom_shift: SQL zoom_level → 目录层级号的偏移
    tms_row: True 则 SQL tile_row 为 TMS（自下而上），否则为 XYZ（自上而下）
    返回 (全部一致, 说明)
    """
    import numpy as np
    from osgeo import gdal
    rows = sql_rows(pkg_path, f"SELECT zoom_level,tile_column,tile_row FROM {table}")
    if not rows:
        return False, "包内无瓦片"
    worst = 0
    checked = 0
    for z, col, row in rows:
        y = (1 << z) - 1 - row if tms_row else row
        ref = gdal.Open(f"{ref_dir}/{z + zoom_shift}/{col}/{y}.png")
        if ref is None:
            return False, f"缺散文件参照 z{z + zoom_shift}/{col}/{y}"
        a = ref.ReadAsArray()
        ref = None
        ds = gdal.OpenEx(pkg_path, gdal.OF_RASTER,
                         open_options=[f"ZOOM_LEVEL={z}"]
                         + ([] if tms_row else [f"TABLE={table}"]))
        if ds is None:
            return False, f"包打不开 z{z}"
        b = ds.ReadAsArray(col * 256, y * 256, 256, 256)
        ds = None
        if b is None:
            return False, f"包内读取失败 z{z}/{col}/{y}"
        d = int(np.abs(a[:3].astype(int) - b[:3].astype(int)).max())
        worst = max(worst, d)
        checked += 1
    return worst == 0, f"逐像素比对 {checked} 张，最大差 {worst}"


def make_fixtures():
    """测试夹具：3 波段 VRT、1 波段影像、workflow 正常/缺段/损坏配置"""
    from osgeo import gdal  # geo_env
    src = f"{DATA24}/H48E018014C.tif"
    gdal.Translate(f"{OUT}/rgb3.tif", src, bandList=[1, 2, 3])
    gdal.Translate(f"{OUT}/rgb1band.tif", src, bandList=[1])
    ds = gdal.Open(f"{OUT}/rgb3.tif")
    gdal.Translate(f"{OUT}/mosaic_like.vrt", ds, format="VRT")
    ds = None
    with open(f"{OUT}/wf_no_tiling.json", "w") as f:
        f.write('{"color_balance": {"input": ["' + DATA24 + '"], "output_dir": "' + OUT + '/x"}}')
    with open(f"{OUT}/wf_bad.json", "w") as f:
        f.write('{ broken json ')
    with open(f"{OUT}/wf_ok.json", "w") as f:
        json.dump({"color_balance": {"input": [DATA24], "output_dir": f"{OUT}/wf_bal",
                                     "method": "overlap_adjust", "output_mode": "vrt",
                                     "ref_image": "H48E018014C"},
                   "tiling": {"output_dir": f"{OUT}/wf_tiles", "tiling_scheme": "CGCS2000",
                              "min_zoom": 13, "max_zoom": 13, "bbox": "105.30,29.02,105.32,29.04",
                              "overview_policy": "no", "num_threads": 4}}, f, indent=2)


def main():
    # 先保留既有报告（用于合并在本次被跳过的用例结果），再清理输出目录
    rp = f"{OUT}/report.json"
    old_results = {}
    if os.path.exists(rp):
        try:
            with open(rp) as f:
                old_results = {r["case"]: r for r in json.load(f).get("results", [])}
        except Exception:
            old_results = {}
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT, exist_ok=True)
    make_fixtures()  # 必须在清理之后生成（否则会被 rmtree 删掉）
    T = os.path.join(BIN, "tiling")
    C = os.path.join(BIN, "color_balance")
    W = os.path.join(BIN, "workflow")
    S = os.path.join(BIN, "ip_server")
    bbox = "105.30,29.02,105.32,29.04"
    d24 = f"{OUT}/j"

    # ---------- A. CLI 参数校验与异常 ----------
    case("A01 tiling 缺 --input", [T, "--output-dir", d24], {2}, tails_has("缺少输入"))
    case("A02 tiling 缺 --output-dir", [T, "--input", GF2], {2}, tails_has("缺少输出目录"))
    case("A03 tiling 未知参数", [T, "--input", GF2, "--output-dir", d24, "--nope"], {2}, tails_has("未知参数"))
    case("A04 tile_size 非 2 的幂", [T, "--input", GF2, "--output-dir", d24, "--tile-size", "100"], {2}, tails_has("2 的幂"))
    case("A05 format 非法", [T, "--input", GF2, "--output-dir", d24, "--format", "gif"], {2}, tails_has("png / jpeg / webp"))
    case("A06 nodata_mode=sample_fill 显式拒绝", [T, "--input", GF2, "--output-dir", d24, "--nodata-mode", "sample_fill"], {2}, tails_has("sample_fill"))
    case("A07 overview_policy 非法", [T, "--input", GF2, "--output-dir", d24, "--overview-policy", "bad"], {2}, tails_has("auto / yes / no"))
    case("A08 bbox 格式错", [T, "--input", GF2, "--output-dir", d24, "--bbox", "1,2,3"], {2}, tails_has("bbox 格式"))
    case("A09 tiling 输入不存在", [T, "--input", "no_such.tif", "--output-dir", d24], {1}, tails_has("无法打开输入"))
    case("A10 tiling --help", [T, "--help"], {0}, tails_has("用法"))
    case("A11 color 缺 --input", [C, "--output-dir", d24], {2}, tails_has("缺少输入"))
    case("A12 color method 非法", [C, "--input", DATA24, "--output-dir", d24, "--method", "bad"], {2}, tails_has("method 仅支持"))
    case("A13 color output_mode 非法", [C, "--input", DATA24, "--output-dir", d24, "--output-mode", "bad"], {2}, tails_has("output_mode 仅支持"))
    case("A14 color profile_mode 非法", [C, "--input", DATA24, "--output-dir", d24, "--profile-mode", "bad"], {2}, tails_has("profile_mode 仅支持"))
    case("A15 target-mean 值个数非法", [C, "--input", DATA24, "--output-dir", d24, "--target-mean", "1,2"], {2}, tails_has("1 个值或 3 个"))
    case("A16 target-std<=0 非法", [C, "--input", DATA24, "--output-dir", d24, "--target-std", "0"], {2}, tails_has("必须 > 0"))
    case("A17 apply 但方案不存在", [C, "--input", DATA24, "--output-dir", d24, "--profile-mode", "apply",
                                "--profile", f"{OUT}/no_profile.json"], {1}, tails_has("读取既有方案失败"))
    case("A18 append 仅支持 vrt", [C, "--input", GF2, "--output-dir", d24, "--profile-mode", "append",
                               "--output-mode", "tif"], {1}, tails_has("仅支持 output_mode=vrt"))
    case("A19 append 仅支持 overlap_adjust", [C, "--input", GF2, "--output-dir", d24, "--profile-mode", "append",
                                           "--method", "block_wallis"], {1}, tails_has("仅支持 method=overlap_adjust"))
    case("A20 波段数<3 拒绝", [C, "--input", f"{OUT}/rgb1band.tif", "--output-dir", d24], {1}, tails_has("少于 3 波段"))
    case("A21 workflow 缺 --config", [W], {2}, tails_has("用法"))
    case("A22 workflow 配置缺 tiling 段", [W, "--config", f"{OUT}/wf_no_tiling.json"], {2}, tails_has("缺少 tiling 段"))
    case("A23 workflow JSON 语法错", [W, "--config", f"{OUT}/wf_bad.json"], {1}, tails_has("读取配置失败"))
    case("A24 ip_server 缺 --data-dir", [S], {2}, tails_has("缺少 --data-dir"))
    case("A25 ip_server --help", [S, "--help"], {0}, tails_has("用法"))

    # ---------- B. 功能矩阵 ----------
    case("B01 CGCS2000 z13 bbox 切片", [T, "--input", f"{OUT}/mosaic_like.vrt", "--output-dir", f"{OUT}/t_cgcs",
                                    "--tiling-scheme", "CGCS2000", "--min-zoom", "13", "--max-zoom", "13", "--bbox", bbox],
         {0}, lambda rc, o, t: (count_ext(f"{OUT}/t_cgcs", ".png") > 0, f"瓦片 {count_ext(f'{OUT}/t_cgcs', '.png')} 张"))
    case("B02 WebMercatorQuad 切片", [T, "--input", f"{OUT}/mosaic_like.vrt", "--output-dir", f"{OUT}/t_merc",
                                    "--tiling-scheme", "WebMercatorQuad", "--max-zoom", "9"],
         {0}, lambda rc, o, t: (count_ext(f"{OUT}/t_merc", ".png") > 0, f"瓦片 {count_ext(f'{OUT}/t_merc', '.png')} 张"))
    case("B03 WorldCRS84Quad 切片", [T, "--input", f"{OUT}/mosaic_like.vrt", "--output-dir", f"{OUT}/t_crs84",
                                    "--tiling-scheme", "WorldCRS84Quad", "--max-zoom", "8"],
         {0}, lambda rc, o, t: (count_ext(f"{OUT}/t_crs84", ".png") > 0, f"瓦片 {count_ext(f'{OUT}/t_crs84', '.png')} 张"))
    case("B04 自定义 TMS JSON 切片", [T, "--input", f"{OUT}/mosaic_like.vrt", "--output-dir", f"{OUT}/t_tmsjson",
                                    "--tiling-scheme", TMS_JSON, "--min-zoom", "13", "--max-zoom", "13", "--bbox", bbox],
         {0}, lambda rc, o, t: (count_ext(f"{OUT}/t_tmsjson", ".png") > 0, f"瓦片 {count_ext(f'{OUT}/t_tmsjson', '.png')} 张"))
    case("B05 convention=tms（y 翻转）", [T, "--input", f"{OUT}/mosaic_like.vrt", "--output-dir", f"{OUT}/t_tmsconv",
                                       "--tiling-scheme", "CGCS2000", "--min-zoom", "13", "--max-zoom", "13",
                                       "--bbox", bbox, "--convention", "tms", "--no-resume"],
         {0}, lambda rc, o, t: (count_ext(f"{OUT}/t_tmsconv", ".png") > 0, f"瓦片 {count_ext(f'{OUT}/t_tmsconv', '.png')} 张"))
    case("B06 format=jpeg", [T, "--input", f"{OUT}/mosaic_like.vrt", "--output-dir", f"{OUT}/t_jpg",
                             "--tiling-scheme", "CGCS2000", "--min-zoom", "13", "--max-zoom", "13", "--bbox", bbox,
                             "--format", "jpeg"],
         {0}, lambda rc, o, t: (count_ext(f"{OUT}/t_jpg", ".jpg") > 0, f"jpg {count_ext(f'{OUT}/t_jpg', '.jpg')} 张"))
    case("B07 format=webp", [T, "--input", f"{OUT}/mosaic_like.vrt", "--output-dir", f"{OUT}/t_webp",
                             "--tiling-scheme", "CGCS2000", "--min-zoom", "13", "--max-zoom", "13", "--bbox", bbox,
                             "--format", "webp"],
         {0}, lambda rc, o, t: (count_ext(f"{OUT}/t_webp", ".webp") > 0, f"webp {count_ext(f'{OUT}/t_webp', '.webp')} 张"))
    case("B08 tile_size=512", [T, "--input", f"{OUT}/mosaic_like.vrt", "--output-dir", f"{OUT}/t_512",
                               "--tiling-scheme", "CGCS2000", "--min-zoom", "13", "--max-zoom", "13", "--bbox", bbox,
                               "--tile-size", "512"],
         {0}, lambda rc, o, t: (count_ext(f"{OUT}/t_512", ".png") > 0, f"瓦片 {count_ext(f'{OUT}/t_512', '.png')} 张"))
    case("B09 nodata_pct_threshold=50（阈值语义）", [T, "--input", GF2, "--output-dir", f"{OUT}/t_thr",
                                                 "--tiling-scheme", "CGCS2000", "--min-zoom", "12", "--max-zoom", "12",
                                                 "--nodata-pct-threshold", "50"],
         {0}, lambda rc, o, t: (("跳过(阈值)" in o), "日志含'跳过(阈值)'=" + ("是" if "跳过(阈值)" in o else "否")))
    case("B10 skip_empty_tiles=false", [T, "--input", GF2, "--output-dir", f"{OUT}/t_keep",
                                        "--tiling-scheme", "CGCS2000", "--min-zoom", "12", "--max-zoom", "12",
                                        "--skip-empty-tiles", "false"],
         {0}, lambda rc, o, t: (count_ext(f"{OUT}/t_keep", ".png") > 0, f"瓦片 {count_ext(f'{OUT}/t_keep', '.png')} 张"))
    case("B11 断点续跑（第二次全跳过）", [T, "--input", f"{OUT}/mosaic_like.vrt", "--output-dir", f"{OUT}/t_cgcs",
                                     "--tiling-scheme", "CGCS2000", "--min-zoom", "13", "--max-zoom", "13", "--bbox", bbox],
         {0}, lambda rc, o, t: ((f"写出 0" in o) and ("续跑跳过" in o), "写出 0 且含续跑跳过"))
    case("B12 color global_adjust + target", [C, "--input", DATA24, "--output-dir", f"{OUT}/c_ga",
                                             "--method", "global_adjust", "--target-mean", "100,110,90",
                                             "--target-std", "40,35,30", "--output-mode", "vrt"],
         {0}, lambda rc, o, t: (os.path.exists(f"{OUT}/c_ga/_profile.json"), "方案 JSON 已生成"))
    case("B13 color hist_match（物理输出）", [C, "--input", DATA24, "--output-dir", f"{OUT}/c_hm", "--method", "hist_match"],
         {0}, lambda rc, o, t: (count_ext(f"{OUT}/c_hm", ".tif") >= 4, f"tif {count_ext(f'{OUT}/c_hm', '.tif')} 个"))
    case("B14 color block_wallis", [C, "--input", DATA24, "--output-dir", f"{OUT}/c_wallis", "--method", "block_wallis"],
         {0}, lambda rc, o, t: (count_ext(f"{OUT}/c_wallis", ".tif") >= 4, f"tif {count_ext(f'{OUT}/c_wallis', '.tif')} 个"))
    case("B15 color output_mode=cog_mosaic", [C, "--input", DATA24, "--output-dir", f"{OUT}/c_mosaic",
                                             "--output-mode", "cog_mosaic"],
         {0}, lambda rc, o, t: (os.path.exists(f"{OUT}/c_mosaic/mosaic_cog.tif"), "mosaic_cog.tif 存在"))
    case("B16 color scene_format=cog", [C, "--input", DATA24, "--output-dir", f"{OUT}/c_cog",
                                        "--output-mode", "tif", "--scene-format", "cog"],
         {0}, lambda rc, o, t: (count_ext(f"{OUT}/c_cog", ".tif") >= 4, f"cog-tif {count_ext(f'{OUT}/c_cog', '.tif')} 个"))
    case("B17 color profile_mode=recreate", [C, "--input", DATA24, "--output-dir", f"{OUT}/c_recreate",
                                             "--profile-mode", "recreate", "--output-mode", "vrt"],
         {0}, lambda rc, o, t: (("recreate" in o) and os.path.exists(f"{OUT}/c_recreate/mosaic.vrt"), "recreate 日志 + mosaic.vrt"))
    case("B18 workflow 全流程（vrt→切片）", [W, "--config", f"{OUT}/wf_ok.json"], {0},
         lambda rc, o, t: (count_ext(f"{OUT}/wf_tiles", ".png") > 0, f"瓦片 {count_ext(f'{OUT}/wf_tiles', '.png')} 张"))

    # ---------- E. 打包输出（S4：MBTiles / GeoPackage） ----------
    # 先生成散文件参照（供逐像素比对）
    run([T, "--input", f"{OUT}/mosaic_like.vrt", "--output-dir", f"{OUT}/pkg_ref_merc",
         "--tiling-scheme", "WebMercatorQuad", "--max-zoom", "9"])
    run([T, "--input", f"{OUT}/mosaic_like.vrt", "--output-dir", f"{OUT}/pkg_ref_cgcs",
         "--tiling-scheme", "CGCS2000", "--min-zoom", "13", "--max-zoom", "13", "--bbox", bbox])

    def chk_mbtiles(rc, out, tag):
        pkg = f"{OUT}/pkg_mb/tiles.mbtiles"
        if not os.path.exists(pkg):
            return False, "包文件不存在"
        n = sql_rows(pkg, "SELECT COUNT(*) FROM tiles")[0][0]
        meta = dict(sql_rows(pkg, "SELECT name,value FROM metadata"))
        if n <= 0:
            return False, "tiles 表为空"
        if meta.get("minzoom") != "0" or meta.get("maxzoom") != "9":
            return False, f"minzoom/maxzoom 不符: {meta.get('minzoom')}/{meta.get('maxzoom')}"
        ok, d = pkg_tile_diff(pkg, f"{OUT}/pkg_ref_merc", "tiles", 0, True)
        return ok, f"tiles {n}，元数据 minzoom/maxzoom 正确；{d}"

    case("E01 package=mbtiles（含逐像素比对与元数据）",
         [T, "--input", f"{OUT}/mosaic_like.vrt", "--output-dir", f"{OUT}/pkg_mb",
          "--tiling-scheme", "WebMercatorQuad", "--max-zoom", "9", "--package", "mbtiles"],
         {0}, chk_mbtiles)

    def chk_gpkg(rc, out, tag):
        pkg = f"{OUT}/pkg_gp/tiles.gpkg"
        if not os.path.exists(pkg):
            return False, "包文件不存在"
        n = sql_rows(pkg, "SELECT COUNT(*) FROM tiles")[0][0]
        tm = sql_rows(pkg, "SELECT COUNT(*) FROM gpkg_tile_matrix")[0][0]
        if n <= 0:
            return False, "tiles 表为空"
        if tm <= 0:
            return False, "gpkg_tile_matrix 未登记层级"
        # GPKG tile_row 为 XYZ（自上而下）；SQL zoom_level = 目录层级 - 1（CGCS2000 从 1 起）
        ok, d = pkg_tile_diff(pkg, f"{OUT}/pkg_ref_cgcs", "tiles", 1, False)
        return ok, f"tiles {n}，tile_matrix {tm} 层；{d}"

    case("E02 package=gpkg（CGCS2000，含逐像素比对）",
         [T, "--input", f"{OUT}/mosaic_like.vrt", "--output-dir", f"{OUT}/pkg_gp",
          "--tiling-scheme", "CGCS2000", "--min-zoom", "13", "--max-zoom", "13",
          "--bbox", bbox, "--package", "gpkg"],
         {0}, chk_gpkg)

    case("E03 包模式不落散文件",
         [T, "--input", f"{OUT}/mosaic_like.vrt", "--output-dir", f"{OUT}/pkg_mb",
          "--tiling-scheme", "WebMercatorQuad", "--max-zoom", "9", "--package", "mbtiles"],
         {0}, lambda rc, o, t: (count_ext(f"{OUT}/pkg_mb", ".png") == 0,
                                f"散文件 {count_ext(f'{OUT}/pkg_mb', '.png')} 张（期望 0）"))

    case("E04 增量追加（相邻 bbox 新增瓦片）",
         [T, "--input", f"{OUT}/mosaic_like.vrt", "--output-dir", f"{OUT}/pkg_gp",
          "--tiling-scheme", "CGCS2000", "--min-zoom", "13", "--max-zoom", "13",
          "--bbox", "105.32,29.02,105.34,29.04", "--package", "gpkg"],
         {0}, lambda rc, o, t: (("追加到既有包" in o) and
                                sql_rows(f"{OUT}/pkg_gp/tiles.gpkg",
                                         "SELECT COUNT(*) FROM tiles")[0][0] == 2,
                                "追加且瓦片数 1→2"))

    case("E05 package=mbtiles 仅限 WebMercatorQuad", [T, "--input", f"{OUT}/mosaic_like.vrt",
                                                 "--output-dir", f"{OUT}/pkg_e5",
                                                 "--tiling-scheme", "CGCS2000", "--package", "mbtiles"],
         {2}, tails_has("仅支持 tiling_scheme=WebMercatorQuad"))
    case("E06 打包模式 tile_size 须 256", [T, "--input", f"{OUT}/mosaic_like.vrt",
                                          "--output-dir", f"{OUT}/pkg_e6", "--tile-size", "512",
                                          "--package", "gpkg"],
         {2}, tails_has("tile_size 须为 256"))
    case("E07 package 非法值", [T, "--input", f"{OUT}/mosaic_like.vrt", "--output-dir", f"{OUT}/pkg_e7",
                             "--package", "zzz"],
         {2}, tails_has("package 仅支持 mbtiles / gpkg"))

    # ---------- C. 性能：匀色方法耗时（2024 四景，vrt 模式） ----------
    for m in ["overlap_adjust", "global_adjust", "block_wallis", "hist_match"]:
        nm = f"C 匀色耗时 {m}"
        if SKIP_SLOW and m in ("block_wallis", "hist_match"):
            print("SKIP " + nm + "（大耗时，保留旧结果）", flush=True)
            continue
        rc, out, sec = run([C, "--input", DATA24, "--output-dir", f"{OUT}/p_{m}", "--method", m,
                            "--output-mode", "vrt" if m in ("overlap_adjust", "global_adjust") else "tif"],
                           timeout=900)
        RESULTS.append({"case": f"C 匀色耗时 {m}", "ok": rc == 0, "rc": rc, "sec": sec,
                        "detail": f"rc={rc} 耗时 {sec}s", "cmd": f"color_balance --method {m}"})
        print(("PASS " if rc == 0 else "FAIL ") + f"C 匀色耗时 {m} :: {sec}s", flush=True)

    # 汇总（与既有报告按 case 名合并，支持分批/跳过大耗时用例）
    by_case = dict(old_results)
    for r in RESULTS:
        by_case[r["case"]] = r
    RESULTS[:] = list(by_case.values())
    passed = sum(1 for r in RESULTS if r["ok"])
    report = {"total": len(RESULTS), "passed": passed, "failed": len(RESULTS) - passed,
              "env": {"GDAL_DATA": env()["GDAL_DATA"]}, "results": RESULTS}
    with open(f"{OUT}/report.json", "w") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    print(f"\n=== 合计 {passed}/{len(RESULTS)} 通过 ===", flush=True)
    return 0 if passed == len(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())