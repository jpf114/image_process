# Phase 0 验证报告（栅格匀色与切片工具）

> 执行时间：2026-10-12 ~ 2026-10-13　环境：WSL Ubuntu（4 核 / 9.7GB RAM）
> 脚本：[scripts/phase0/](file:///home/tdt-dell/code/MyProject/image_process/scripts/phase0)　
> 产出：[output/phase0/](file:///home/tdt-dell/code/MyProject/image_process/output/phase0)

## 结论总表

| # | 验证项 | 结果 | 关键结论 |
|---|---|---|---|
| 0.1 | 环境体检 | **PASS** | geo_env 的 proj.db **无问题**；此前报错是 GDAL3 权威轴序所致，显式 `SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER)` 即可。4490↔3857/4326 双向转换精确 |
| 0.3 | EPSG:4490 切片方案 | **PASS** | 自研 `tms_CGCS2000.json`（OGC 标准）可用；格网与天地图公式完全一致，几何 == WorldCRS84Quad、层级编号 +1。两个关键实现细节见 §2 |
| 0.2 | 白瓦片复现与对策 | **PASS** | 无效值显形问题成功复现；对策（透明 PNG）完全消除；z18"白色"根因 = 无效像素值/平台填充色被当作有效数据 + JPEG 无透明通道，**与分辨率无关** |
| 0.4 | 匀色算法对比 | **PASS** | 初版用"全局均值差"当接缝指标是**错的**，已修正（§4）：本批数据四条接边本就辐射连续（重叠区 Δμ<0.35），全局统计对齐反而**制造接缝**（台阶 -6.2，目视可见）。**主方法=重叠区辐射平差 overlap_chain（台阶 -0.26/-0.44，恢复无缝）+ 前置接边一致性检查**；block_wallis 局部统计保形可作景内精化。**补验（§4.6）：跨批次 GF2×2024 场景同样成立——全局目标留 -4.1 重叠区残差，重叠区解参为 0**；仅完全无重叠才允许全局目标 |
| 0.5 | 切片性能基准 | **PASS** | `gdal raster tile` 多线程有效但 4 核机器 4 线程后收益趋平；JPEG 编码最快，PNG 最慢（约 2-4 倍差距），自研引擎需重点优化 PNG 编码并行 |
| — | VRT 像素函数（前次完成） | **PASS** | 匀色不落盘可行，读取零开销(0.96x)；GDAL VRT 无 LUT 元素，用 VRTDerivedRasterBand + C++ 注册带参像素函数 |

**工程约束（重要）**：本机 WSL 仅 9.7GB 内存。期间一次并行两个全分辨率重负载曾导致 WSL 崩溃（已复盘）。**C++ 引擎必须遵守：块式处理（恒定内存）、限制并发影像数、预留内存护栏、基准测试串行执行。**

---

## 1. 0.1 环境体检（PASS）

- geo_env：GDAL 3.11.4 + PROJ 9.3.1 + OpenCV 4.13 + numpy 1.26，全部可用
- vcpkg：GDAL 3.12.4（库，无 CLI 工具，生产链接用）；conda CLI 用于验证
- **"proj.db 有问题"的真相**：`osr.CoordinateTransformation(4490→3857)` 直接使用会报
  `PROJ: webmerc: Invalid latitude`——因为 EPSG:4490 权威轴序为（纬度,经度），传入的 (105.5, 29.0) 被当作 (lat=105.5)。**proj.db 本身完好**。修复：对所有 SRS 调用
  `SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER)`（脚本 `check_env.py` 已验证全部 PASS）
- z18 分辨率量化：WebMercator z18 在北纬 29° 为 0.522m/px。2024 数据 2.0m/px（过采样 3.8 倍）、2025 GF2 0.8m/px（过采样 1.5 倍）→ z18 只会糊不会白

## 2. 0.3 EPSG:4490（天地图）切片方案（PASS）

`tms_CGCS2000.json`（OGC TwoD TileMatrixSet 标准，identifier 1~24，z1=全球 2×1，分辨率 0.703125°/px÷2^(n-1)）：
- GDAL 3.11.4 自动发现（放入 GDAL_DATA 目录，无需改环境——验证用符号链接副本）
- 瓦片索引与天地图公式逐级核对**全部 PASS**（z10~z14 实测 x/y 索引与理论值完全一致）
- 与 WorldCRS84Quad 同级索引瓦片集合**完全一致** → 4490 与 4326 在瓦片格网上无差别，仅 CRS 声明与层级起点不同

**两个关键实现细节（C++ 引擎与部署必须遵守）**：
1. **topLeftCorner 必须按 CRS 轴序书写**：EPSG:4490 为 lat-first，JSON 中须写 `[90, -180]`；写 `[-180, 90]` 会报
   `Extent of source dataset is not compatible with extent of tile matrix`（GDAL 内部检测 lat-long CRS 后交换轴）。网上资料普遍写 `[-180, 90]`（那是 CRS84/lon-lat 的写法），照抄必错。
2. **GDAL CLI 按矩阵数组索引（0 起）命名输出目录**，忽略 identifier 字符串：天地图 z10 出现在输出目录 `9/`。自研引擎按 identifier（=天地图层级）命名即可规避；若用 GDAL CLI 需注意层级换算 z_out = z_tianditu − 1。

## 3. 0.2 白瓦片/无效值复现与对策（PASS）

数据：GF2（33% 无效区，无效像素 RGB=0 黑色，alpha=0）。z17~z18 WebMercator 切片试验矩阵：

| 方案 | 无效区中心瓦片 | 边界瓦片（75% 无效） | 结论 |
|---|---|---|---|
| A. JPEG | **100% 纯黑**（RGB 0/0/0 显形） | 75% 纯黑硬边界，均值 6/7/6 | 复现"无效值显形"：黑块/黑边 |
| B. JPEG + --dst-nodata 255 | 仍 100% 纯黑 | 同 A | JPEG 无透明通道，dst-nodata 不产生填充效果 |
| C. PNG（默认带 alpha） | **100% 透明** | 75% 透明，有效区正常 | **对策有效** |
| D. PNG + --skip-blank | 全无效瓦片不落盘 | 同 C | 额外节省存储 |
| E. PNG + 阈值50%+average | 100% 透明 | 75% 透明 | 边缘瓦片自动整体透明，无黑晕 |

样例图：[output/phase0/white_tiles/](computer:///home/tdt-dell/code/MyProject/image_process/output/phase0/white_tiles)

**z18"白色"根因结论**（结合量化分析）：
1. 本数据无效区像素为黑色 → JPEG 输出显形为**黑块**；若源数据无效区填了 255（白色背景，交付数据常见）或切片平台把透明渲染为白底（ArcGIS 缓存 JPEG 即如此），则显形为**白块**——机制相同，都是"无效像素未按透明处理"。
2. **分辨率不足（over-zoom）不产生白色**，只会模糊——z18 过采样已量化排除。
3. 生产对策（写入参数默认值）：`format=png` + `nodata_mode=transparent`；需 JPEG 时提示用户无效区将以 `fill_value`（默认 255）显形；`nodata_pct_threshold`（默认 100，边缘瓦片可调 50）+ `skip_empty_tiles`（默认开）。

## 4. 0.4 匀色算法对比（**修订版：指标修正后的结论**）

> **重要更正（2026-10-13）**：初版报告用"景间全局均值差"当接缝指标（11.30→0.02），得出"全局平差最优"的结论。
> 经需求方目视质疑并重新验证：**该指标是错的**——全局均值一致 ≠ 接缝无台阶，全局平差反而**制造了肉眼可见的接缝**。
> 以下为修正后的完整证据链（脚本 `seam_deep_verify.py`，指标见 `output/phase0/color/seam_metrics.json`）。

### 4.1 重叠区实测真相（全分辨率，同地物双景差）

| 接缝对 | 重叠区 | Δμ (R/G/B) |
|---|---|---|
| 014\|015（V上排） | 23×9279px | **-0.07 / +0.12 / -0.18** |
| 19014\|19015（V下排） | 23×9279px | -0.07 / +0.08 / -0.33 |
| 014\|19014（H左列） | 13911×20px | +0.15 / -0.02 / -0.10 |
| 015\|19015（H右列） | 13911×20px | +0.04 / +0.03 / -0.13 |

**这批标准图幅在四条接边处本来就辐射连续（Δμ<0.35）**。全局均值差（81.3 vs 95.4）来自**景内内容/光照差异**（不同图幅地物构成不同），不是接边问题——对这种数据做"全局统计对齐"本身就是错误操作。
（本节数据已持久化：`gf2_overlap_verify.py` Part A 复算与原值一致，R/G 波段完全吻合、B 波段 ±0.2 窗口容差，见 `output/phase0/color/gf2_overlap_metrics.json`）

### 4.2 各方法在四条接缝上的【局部台阶】实测（拼接图上逐行量测，亮度 0-255）

| 方法 | V上排 | H左列 | 结论 |
|---|---|---|---|
| 原始 | **+0.04** | -0.24 | 本来就无缝 |
| global_adjust（全局目标） | **-6.23** | -3.15 | **制造接缝**（目视可见，见 mosaic_2024_global_adjust.png） |
| hist_match（全局参考） | -6.28 | -2.71 | 同样制造接缝 |
| reinhard（全局目标） | -7.00 | -3.29 | 最差 |
| block_wallis（局部块统计） | **+0.56** | -1.01 | 四方法中最好（局部统计天然保形） |
| **overlap_chain（重叠区链式平差）** | **-0.26** | **-0.44** | **最优，恢复原始无缝状态** |

### 4.3 机理推演（为何全局平差制造接缝）

接缝处原始局部亮度 L_A=89.1、L_B=89.0（台阶≈0）。全局平差把 014 拉亮（L'→98.0）、015 压暗（L'→83.4），**接缝台阶被制造为 -14.6（R 波段，公式预测）**，实测 -6.2（降采样+羽化混合后衰减）。即：**两景接边本来连续、只是景内统计不同时，按全局统计调节必然在接缝处造成跳变。**

### 4.4 算法实现正确性（排除实现 bug）

- global_adjust：处理后全局 μ 与目标差 0.02 ✓（按构造精确）
- hist_match：处理后 CDF 与参考景最大偏差 2.1~2.7% ✓（LUT 实现正确）
- **问题不在实现，在"目标统计的选取"——用全局统计做目标对本来接边连续的数据是错误目标**

### 4.5 修正后的选型结论（替换初版）

1. **主方法 = 重叠区辐射平差（overlap_chain / 图平差）**：仅用重叠区统计解算各景参数（链式或最小二乘图平差），重叠区一致性是唯一硬约束；**无缝时自动解出 ≈ 恒等变换（本数据实测 gain 0.975~1.007, offset ±2.3），不破坏原始连续性**。
2. **前置"接边一致性检查"**：先量测所有重叠对 Δμ/Δσ，低于阈值（建议 |Δμ|<2）的景对**跳过调节**——本批 2024 数据应直接判定"无需匀色"。
3. block_wallis 用于**景内**不均（局部统计保形，接缝最稳），仍需物理输出。
4. 全局统计（μ/σ 对齐）**仅适用于"与基准批次完全无重叠且确需视觉统一"的场景**，且必须提示其接缝风险；**有重叠时一律以重叠区为准**（含跨批次，见 §4.6 实测）。
5. 拼接环节增加**羽化（feathering）**作为兜底（重叠区线性过渡），属于镶嵌 operator 职责。

目视对比：`mosaic_2024_original.png` / `mosaic_2024_global_adjust.png`（可见缝）/ `mosaic_2024_overlap_chain.png`（无缝）——
[output/phase0/color/](computer:///home/tdt-dell/code/MyProject/image_process/output/phase0/color)

### 4.6 补验：GF2(2025)×2024 跨批次重叠区（2026-10-12 审计新增）

> 审计发现：初版 GF2 实验用"对齐 2024 全局统计目标"拉亮，**未做重叠区接缝验证**——与 §4 同类的指标盲区。
> 本节用两者真实重叠区（28.1km×26.6km，双侧有效 69%）直接验证（脚本 `gf2_overlap_verify.py`，数据 `gf2_overlap_metrics.json`）。

| 项 | 数值 | 说明 |
|---|---|---|
| 重叠区原始辐射差 Δμ | **70.6 / 70.4 / 57.9** | 跨批次差异巨大，确需匀色 |
| global_adjust 后重叠区残差 | **-4.12 / -3.46 / -2.14** | 对齐全局目标 → 批次交界处留 ~4 级台阶（根因：2024 重叠区局部 μ=93.9 高于其全局 μ=89.8，全局目标瞄错了地方） |
| overlap_adjust 后重叠区残差 | **0 / 0 / 0** | 重叠区解参 → 批次交界无缝（同窗口自洽语义） |
| 4×4 分块 \|Δμ\| 均值/最大 | global 3.81/6.55；overlap 3.03/7.45 | 两种方法都存在 ~3 级**空间变化残差**（景内光照趋势差异，单景线性变换不可解）——由 Wallis 精化/羽化兜底 |

> **勘误（2026-10-12 Phase 1 复核）**：本节数据取自窗口**降采样读取**（W=1536）。全分辨率复核表明：
> ① GF2 全图精确 std 应为 **8.20/7.23/6.96**（metrics.json "gf2.before" 的 6.66/5.85/5.94 为降采样统计，σ 被平滑压小）；
> ② 窗口降采样同样压小重叠区 σ → 本节 overlap_adjust gain 3.20 偏大，**原始分辨率采样下应为 ~2.43**（Phase 1 C++ 实测并经 Python bincount 双重复核）。
> 教训：凡统计必须明确采样语义；gain 解算一律用原始分辨率采样（C++ `windowStatsRGB` 已改为 tile 采样）。详见 AGENT.md §4/§8。

**结论**：§4.5 修正后的方法论同样覆盖跨批次场景——**只要与基准影像有重叠，一律用重叠区统计解参**；"全局目标"仅是完全无重叠时的回退。分块残差数据同时表明：景级线性变换之后仍需 Wallis/羽化处理空间变化分量（与 §4.5 第 3/5 点互证）。

## 5. 0.5 切片性能基准（PASS）

数据：2024 四景镶嵌 VRT（27801×18538，4490）→ WebMercator z12~z15（2232 瓦片 / PNG 共 239MB 每轮）。

**线程扩展性**（PNG, z12~z15，2232 瓦片/239MB 每次）：

| 线程数 | 1 | 2 | 4 | 8 | 16 |
|---|---|---|---|---|---|
| 耗时(s) | 89.2 | 52.2 | **33.1** | 33.3 | 33.5 |

→ 随核数线性扩展至 4 线程（=物理核数）后饱和；GDAL 内建多线程调度有效（默认 ALL_CPUS 合理）。

**编码格式对比**（8 线程，同数据）：

| 格式 | 耗时(s) | 体积 | 评注 |
|---|---|---|---|
| PNG | 32.9 | 239MB | 最慢、最大，但唯一无损+透明默认 |
| JPEG | 23.6 | 28MB | 最快，无 alpha（无效区显形） |
| WEBP | 26.3 | **22MB** | **体积仅 PNG 的 9.2%**，速度接近 JPEG，支持 alpha |

**结论**：
1. 线程数默认 = 物理核数即可，>核数无收益（与本机 4 核实测一致）；
2. **WEBP 是存储/传输最优解**（体积 1/11、速度接近 JPEG、支持透明）——建议对外默认仍为 PNG（兼容性），提供 `--format webp` 选项并在文档中说明收益；JPEG 仅在明确不需要透明时使用；
3. 自研引擎的性能优化优先级：瓦片级并行调度（已验证有效模式）> WEBP/PNG 编码并行（编码占比较大）> IO（COG/WEBP 读取已足够快）。


## 6. 对技术方案的修订项

1. **实现规范新增**：所有 CRS 相关代码必须 `SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER)`（OSR/warp 一致），否则 4490 数据全流程报错。
2. **TMS 交付物**：`tms_CGCS2000.json`（topLeftCorner=[90,-180]）随 tiling operator 的 data 目录分发；引擎按 identifier 输出层级目录（与天地图一致，从 1 起）。
3. **默认参数确认**：`format=png`+`nodata_mode=transparent`（0.2 验证）；`skip_empty_tiles=true`；`nodata_pct_threshold=100`（边缘带 50）。
4. **匀色算法优先级（依 §4 修订）**：接边一致性检查（前置门控，重叠对 |Δμ|<2 自动跳过调节）→ **overlap_adjust 重叠区辐射平差（主）** → block_wallis（景内精化，可选）→ hist_match（回退）；global_adjust（全局统计目标）仅限**完全无重叠**的场景（含跨批次无重叠情形，见 §4.6）且需提示接缝风险；Reinhard 降级为实验选项。
5. **内存工程约束**：块式处理 + 并发上限 + 内存护栏（本机 9.7GB RAM 实测教训）；服务化 worker 进程按可用内存配置并发数。
6. GDAL CLI 的 `--nodata-values-pct-threshold` 等参数要求 `--resampling average`——自研引擎无此限制，实现任意重采样下的 nodata 判定。

## 7. 脚本清单

| 脚本 | 用途 |
|---|---|
| `check_env.py` | 环境与 4490 转换体检 |
| `verify_vrt_pixelfunc.py` | VRT 像素函数（匀色不落盘）验证 |
| `verify_tms_4490.py` | 4490 TMS 定义生成 + 切片格网核对 |
| `repro_white_tiles.py` | 白瓦片复现试验矩阵（A~E） |
| `color_algo_compare.py` | 四种匀色算法对比（内存安全版） |
| `seam_deep_verify.py` | 接缝深度验证：重叠区真值统计、接缝局部台阶指标、机理推演、overlap_chain 平差 |
| `gf2_overlap_verify.py` | 补验：2024 图幅对重叠统计持久化 + GF2×2024 跨批次重叠区两方法残差对比 |
| `stitch_previews.py` | 2×2 拼接预览图生成 |
| `bench_tiling.sh` | 线程/编码格式性能基准 |

运行方式：`conda activate geo_env && python scripts/phase0/<脚本>`（bench 为 bash）。

**证据归档清单**（2026-10-12 审计补全，此前部分证据仅在 /tmp 未持久化）：
- `output/phase0/bench/`：线程/格式基准原始 .time 数据 + mosaic4490.vrt + filelist.txt
- `output/phase0/tms/`：tms_CGCS2000.json（交付物副本）、verify_stats.txt（格网核对 PASS 输出）、tms_compare_manifest.txt（与 WorldCRS84Quad 逐字节一致 manifest）
- `output/phase0/white_tiles/`：repro_stats.txt（复现统计输出）+ 中心/边界瓦片样例（含 D_skip 与 center_A/C）
- `output/phase0/color/`：metrics.json、seam_metrics.json、gf2_overlap_metrics.json + 全部预览图
