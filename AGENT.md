# AGENT.md — image_process 项目工作指引

> 面向 AI Agent 的项目指引。接手任何任务前先通读本文；修改关键决策须同步更新本文与技术方案文档。

## 1. 项目定位

一套**栅格影像匀色 + 切片**的 C++17 工具集（image_process），最终形态：

- `operators/`：功能动态库（color_balance、tiling，后续扩展），每个功能一个 so
- `apps/`：独立 exe（薄壳，调 operator），含工作流 exe（匀色→切片一键）
- `core/`：公共基础（GDAL 封装、线程池、参数、TMS 计算）
- 后期：cpp-httplib + 多进程 worker 服务化（不用 Go；纯 CPU C++ 对 Docker 友好）

## 2. 当前状态（2026-10-13）

- **Phase 0（Python 验证）6/6 PASS**；**Phase 1（core+color_balance）→ Phase 2（tiling）→ Phase 3（apps+workflow）全部完成并数据级验证**；S1 差距补齐、S2 单元测试（gtest）完成。
- **本轮（2026-10-13）独立完整复核并逐项修复**：交付包漏 workflow、workflow 未接 profile_mode/输出/目标等参数、profile_mode 无校验且 apply/recreate 缺失、target_mean/std 缺失、参数命名偏离方案、断点续跑计数重复、OpenCV 冗余依赖——详见 §6 计划表末行、§8 教训 11/12、§9 验证表。
- 当前测试用例 **24/24 通过**（`cd build/release && ctest --output-on-failure`）。
- **S3 Phase 4 服务化已完成（2026-10-13）**：`ip_server`（cpp-httplib 控制面 + 任务队列/状态机 + worker 进程池 fork/exec）+ REST 5 端点 + 静态瓦片 + Dockerfile；已验证提交/进度/取消/崩溃隔离/优雅退出/重启恢复。用法见 [服务化使用说明.md](文档资料/关于栅格切片和匀色/服务化使用说明.md)，设计论证见 [Phase4服务化论证报告.md](文档资料/关于栅格切片和匀色/Phase4服务化论证报告.md)。
- 待办：S4 MBTiles/GPKG（可选）；`num_processes` 真多进程（服务化已提供任务级并行的替代路径）。
- 测试数据：`文档资料/关于栅格切片和匀色/data/{2024,2025}`（2024 四景 2m EPSG:4490 标准图幅；2025 GF2 0.8m 极暗、33% 无效区）

## 3. 硬性规范（违反即返工）

### 3.1 匀色指标（Phase 0 血泪教训，最重要的教训）
- 评价接缝质量**只能用**：①接缝处局部台阶（逐行量测缝两侧亮度差）②重叠区 Δμ 真值
- **禁止**用"景间全局均值差"当接缝指标——全局均值一致 ≠ 接缝无缝；全局统计对齐会把本就接边连续的数据制造出 -6~-14 的可见台阶
- **算法优先级**：接边一致性检查（重叠对 |Δμ|<2 自动跳过）→ **overlap_adjust 重叠区辐射平差（主）** → block_wallis（景内精化）→ hist_match（回退）；global_adjust（全局统计目标）**仅完全无重叠时**回退并提示接缝风险
- 景级线性变换后仍有 ~3 级空间变化残差（实测），由 Wallis/羽化兜底

### 3.2 内存（本机 WSL 仅 9.7GB RAM / 4 核）
- 一切处理**块式**（恒定内存），禁止整景 float32 装载（2024 一景全分辨率 float32 ≈1.5GB，曾致 WSL OOM 崩溃）
- 严格串行或限并发；重负载任务跑前检查 `MemAvailable`（<1.5GB 终止）
- 基准测试串行执行

### 3.3 GDAL/坐标
- EPSG:4490 权威轴序**纬度在前**：所有 OSR 代码必须 `SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER)`，否则全流程报错
- 自研 `tms_CGCS2000.json` 的 topLeftCorner 必须写 `[90,-180]`（按 CRS 轴序；网上资料 `[-180,90]` 照抄必错）；副本在 `output/phase0/tms/`
- 天地图层级从 1 起；CGCS2000 z_n ≡ WorldCRS84Quad level n-1（格网相同）；GDAL CLI 按数组索引命名输出目录（z10 → 目录 9），**自研引擎按 identifier 命名**
- GDAL VRT 无内建 LUT 元素；匀色参数走 `VRTDerivedRasterBand` + `GDALAddDerivedBandPixelFuncWithArgs()` 注册带参像素函数（实测读取开销 0.96x）

### 3.4 切片默认参数（Phase 0 已验证）
- `format=png` + `nodata_mode=transparent`（无效区透明，白/黑瓦片的唯一根治方案；JPEG 无透明必显形，显黑或白取决于无效像素值）
- `skip_empty_tiles=true`；`nodata_pct_threshold=100`（边缘带可 50）
- z18"白色"与分辨率无关（over-zoom 只糊不白）
- WEBP 体积仅 PNG 的 9.2% 且带透明（存储最优）；对外默认仍 PNG
- 线程默认 = 物理核数（>核数无收益，实测 4 核饱和）

### 3.5 构建与环境（2026-10-12 需求方确认）
- C++17；**vcpkg 使用全局 classic 安装**（`/home/tdt-dell/vcpkg`，x64-linux 已装 gdal 3.12.4 + nlohmann-json + proj，无需更新；OpenCV 虽在 vcpkg 中存在但本项目不再依赖）。**禁用 manifest 模式**（CMakeLists 已默认 OFF）；根目录 `vcpkg.json` 仅作依赖清单/版本说明，不触发任何下载安装
- 构建目录区分 **build/debug 与 build/release**；二进制输出 **bin/Debug 与 bin/Release**（顶层 CMakeLists 已配置 `$<CONFIG>` 输出）
- **交付形态默认动态库**：operator 为 .so（如 libip_color_balance.so），core 为内部静态库并入动态库（不单独交付）；后续按需支持静态库
- **安装默认 Release**：`cmake --install build/release --prefix <dir>`；打包用 `scripts/package_release.sh`——自动构建 Release → install → **递归收集全部动态依赖 so**（排除系统库）→ 拷贝 GDAL_DATA/PROJ 数据 → 生成 bin/ipenv.sh 启动脚本 → tar.gz，可直接带走解压使用（假设目标机 x86_64 且 glibc 不旧于构建机）
- 所有文本文件 **UTF-8（无 BOM）+ LF**，禁止 CRLF
- **第三方依赖仅 GDAL + nlohmann-json + cpp-httplib**（vcpkg.json 同）。cpp-httplib（header-only，0.51.0）仅用于服务化控制面（`ip_server`）。**OpenCV 经完整评估不引入**：其优势（imgproc 滤波/稠密线代/CUDA）在本项目未被使用（融合羽化未实现、解算规模过小、无 GPU 环境），唯一理论受益点 `cv::LUT` 处非瓶颈；完整论证与再引入条件见《技术方案》§3.6
- GPU：`IP_ENABLE_GPU` CMake option（默认 OFF，仅暴露编译宏）+ `use_gpu` 运行参数（占位，GPU 路径未实现）
- Python 一律用 conda `geo_env`（GDAL 3.11.4 + OpenCV 4.13），禁止 base 环境、禁止先 pip
- 不私自下载/安装任何库，需先征得用户同意

## 4. 数值验证锚点（C++ 实现必须对齐实测结果）

**统计真值（2026-10-12 修正，Python bincount 全分辨率复核）**：
- GF2 全图精确统计（仅 alpha>0 掩码）：mean 23.34/25.24/18.69，**std 8.20/7.23/6.96**
- ⚠️ Phase 0 metrics.json 中 GF2 std=6.66/5.85/5.94 是**降采样(decim=8)统计**（平滑压小了 σ），已被真值取代——引用统计值前必须确认采样语义（教训见 §8）
- 2024 四景全图精确统计：mean 81.3/85.1/67.0 等（metrics.json 场景值有效，2024 无效区占比 0，降采样影响可忽略）

**overlap_adjust 恒等路径（2024 四景，已验证 PASS）**：
- 6 条重叠边 Δμ(max) 全部 < 0.71 < 阈值 2.0 → 全部门控跳过
- 四景参数恒等，输出与输入逐像素一致（maxdiff=0），CRS/geotransform 保留
- 重叠边 Δμ 与 Phase 0 §4.1 吻合（R 波段一致，G/B ±0.2 窗口容差）

**overlap_adjust 解算路径（GF2×2024 跨批次，已验证）**：
- 一致性等价类：2024 四景互跳过 → 恒等保持；GF2 经 (014,GF2) 边解出 gain=2.43/2.41/2.41
- 解算语义：先沿 skip 边合并等价类，再沿未 skip 边 BFS（首达即定）——**防止跨批次景反向拉动已一致景**（曾出现 015 被 GF2 拉偏的 bug，已修复）
- 014|GF2 边残差=0（构造保证）；28km 全区残差 6.2/6.1/7.5 = 空间变化分量 + 单边锚定差 → **下一迭代：最小二乘图平差**均衡各边（Phase 0 §4.6：空间分量 ~3 级为线性变换不可消除的下限）
- GF2 全区残差评估须用同窗口自洽（Phase 0 §4.6 的 residual 0 即同窗口语义）

## 5. 目录结构

```
image_process/
├── AGENT.md            # 本文件
├── CMakeLists.txt      # 顶层 C++17 + vcpkg toolchain
├── vcpkg.json          # 依赖清单（gdal、nlohmann-json、cpp-httplib）
├── core/               # 公共基础（静态库）
├── operators/          # 功能动态库：color_balance、tiling（含 data/tms_CGCS2000.json 分发件）
├── apps/               # exe 薄壳（+ server/：ip_server 服务化控制面）
├── docker/             # Dockerfile（Phase 4 容器化）
├── scripts/phase0/     # Python 验证脚本（geo_env）
├── output/phase0/      # Phase 0 证据（bench/tms/white_tiles/color）
└── 文档资料/           # 需求、方案、报告、测试数据
```

## 6. 工作计划

| 阶段 | 内容 | 状态 |
|---|---|---|
| Phase 0 | Python 验证 6 项 | ✅ 完成（证据落盘 output/phase0） |
| Phase 1 | core + color_balance：overlap_adjust（统计/门控/图平差/块式应用/方案 JSON）+ CLI + 构建打包体系 | ✅ 完成，2024+GF2 双路径验证通过 |
| Phase 1 迭代 2 | 最小二乘图平差 + block_wallis + hist_match + VRT 零落盘输出（含组合模式） | ✅ 全部完成并验证 |
| Phase 2 | tiling operator：TMS 三内置+JSON、WarpedVRT+线程池并行、nodata 模式、png/jpeg/webp | ✅ 核心完成并验证（CGCS2000 格网与 GDAL CLI 逐级一致；GF2 透明对策一致） |
| Phase 2 收尾 | 性能优化（overview 利用）、MBTiles/GPKG 打包、tiles_scope 增量切片 | ✅ auto overview/bbox/tile_buffer 已实现；MBTiles/GPKG 见《后续执行方案》S4 |
| Phase 3 | apps + workflow（三 exe、JSON 配置、断点续跑、端到端验收） | ✅ 完成 |
| Phase 4 | 服务化（cpp-httplib + worker 进程池、Dockerfile） | ✅ 完成（2026-10-13；见《服务化使用说明.md》） |
| S1 差距补齐 | 方案承诺项闭环（每景 COG/镶嵌 COG/append 增量/参数补齐） | ✅ 完成（2026-10-13） |
| S2 单元测试 | gtest 用例（block_stats/tms/solver/render 四套件）+ 顶层 enable_testing | ✅ 完成（2026-10-13，23/23 通过 + 真实影像冒烟） |
| 复核修复 | 独立完整复核（算法/需求/测试/方案）发现缺陷逐项修复：安装漏件、workflow 参数、profile_mode 校验+apply/recreate、target_mean/std、参数别名、续跑计数、OpenCV 冗余 | ✅ 完成（2026-10-13，24/24 通过 + CLI/workflow 冒烟） |

## 7. 常用命令

```bash
# 构建（CMakePresets，GCC+Ninja；linux-release→build/release，linux-debug→build/debug）
export VCPKG_ROOT=$HOME/vcpkg
cmake --preset linux-release && cmake --build --preset linux-release -j4
# debug: cmake --preset linux-debug && cmake --build --preset linux-debug -j4
# GPU 占位开关（默认 OFF，仅暴露 IP_ENABLE_GPU 宏）：cmake --preset linux-release -DIP_ENABLE_GPU=ON
# 输出 bin/{Release,Debug}；manifest 已禁用（vcpkg.json 仅清单说明）

# 打包（自动用 presets 构建 Release + 收集依赖 + tar.gz）
scripts/package_release.sh

# 运行（LD_LIBRARY_PATH/GDAL_DATA/PROJ_DATA 指向 vcpkg 安装）
export LD_LIBRARY_PATH=$HOME/vcpkg/installed/x64-linux/lib:$LD_LIBRARY_PATH
export GDAL_DATA=$HOME/vcpkg/installed/x64-linux/share/gdal
export PROJ_DATA=$HOME/vcpkg/installed/x64-linux/share/proj

# 单元测试（24 用例；enable_testing 在顶层 CMakeLists，ctest 须在 build/release 下执行）
cd build/release && ctest --output-on-failure
bin/Release/color_balance --input <影像|目录> --output-dir <目录> --ref-image <参考景>
bin/Release/workflow --config <workflow.json> [--only color|tiling]

# 服务化（Phase 4）：控制面 + worker 进程池；用法见《服务化使用说明.md》
bin/Release/ip_server --data-dir ipdata --tiles-dir out/tiles --port 8080 --workers 2
curl -X POST -H 'Content-Type: application/json' --data-binary @job.json \
  http://127.0.0.1:8080/api/v1/jobs        # job.json 与 workflow --config 同构
curl http://127.0.0.1:8080/api/v1/jobs/<id>          # 状态/进度
curl http://127.0.0.1:8080/api/v1/jobs/<id>/logs     # 日志
curl -X DELETE http://127.0.0.1:8080/api/v1/jobs/<id>  # 取消

# 运行日志：自动落盘 <output_dir>/logs/run_<operator>_<时间戳>.log（每次处理一个文件）

# git（origin=github.com/jpf114/image_process，本地开发按阶段提交，推送需用户确认）
git add -A && git commit -m "..."

# 运行 Python 验证/对比脚本
conda activate geo_env && python scripts/phase0/<脚本>.py
```

## 8. Phase 1 实施教训（编码前必读）

1. **GDAL 驱动注册**：进程内不调用 `GDALAllRegister()` 时 `GDALDataset::Open` 会**静默失败且无错误消息**——core 的 openReadOnly/openUpdate 已用 `std::call_once` 懒注册，新代码一律走 core 封装
2. **统计采样语义**：buf 降采样读取会平滑高频、系统性压小 σ → gain 解算偏大（GF2 σ 实际 8.2 vs 降采样 6.7；gain 3.20 → 修正 2.43）。窗口统计一律用**原始分辨率 tile 采样**（core windowStatsRGB 已实现），引用任何统计值先确认采样方式
3. **图平差传播规则**：必须先沿"门控跳过"边合并一致性等价类（恒等），再沿未跳过边最小二乘解算——否则跨批次景会反向拉动已一致景（实测 bug）
4. **静态库并入动态库必须 PIC**（CMAKE_POSITION_INDEPENDENT_CODE ON 已全局配置）
5. 打包排除系统库（/lib、/usr/lib），自家 libip_* 不重复收集
6. **VRT 派生波段像素函数（GDAL 3.12 实测，踩坑三次才定位）**：
   - XML 属性是 **`subClass`**（驼峰），小写 `subclass` 不被识别（读失败无报错）
   - 源数据在 `papoSources[0]`，输出在 `pData`——**分离缓冲**，不是 in-place
   - `nBufferRadius` 传 64 但**源缓冲并未扩边**：布局 = 原始请求窗（nXSize×nYSize，行宽 nXSize，无偏移）；radius 仅信息参数（除非 XML 声明 `<BufferRadius>`）。按"行宽含 radius"写会造成堆越界（表现为 close 时 FlushCache 段错误）+ 读回全 0/错位
   - 像素函数为进程内注册，VRT 仅可在同进程打开（切片 operator 同进程使用）；复现证据脚本 `scripts/phase0/test_vrt_min.cpp`
7. **验证脚本陷阱**：Python 掩码抽样 `arr[b][m][:7]` 只取前 7 个像素毫无代表性——hist_match 首验 CDF 偏差 0.5 的假象即由此来，全量 bincount 后为 0.016~0.026（真值）
8. **WarpedVRT 编程（tiling 踩坑四次，S2 定稿改用 AutoCreateWarpedVRT）**：
   - **定稿路径：`GDALAutoCreateWarpedVRT(src, srcWkt, dstWkt, eResampleAlg, dfMaxError, nullptr)`**——自动推算目标网格/变换/**DstGeoTransform**，支持指定重采样算法；同 CRS 恒等变换无重采样损失，跨 CRS 由瓦片层 RasterIOExtraArg 兜底。手工 `GDALWarpOptions`+`GDALCreateWarpedVRT` 的下列陷阱仅作历史参考（手写网格：OSR 采样源网格→bbox→尺寸/gt；`wo->hSrcDS` 必设（Validate）；**DstGeoTransform 必须显式设置**否则全 0 无报错；bbox 采样须含边界角点）
   - **VRT CreateCopy 的 XML 回读校验**：VRT 驱动 CreateCopy 会把序列化 XML 重新 Open 一次——`<SourceDataset>` 取源 dataset description，**MEM 空描述必挂**（`ERROR 4: : No such file or directory` + `Validate(): hSrcDS is not set`，报错点在写出而非 warp）。**VRT 源必须是可重开文件路径**——测试合成源也要先落盘 GTiff 再打开（生产天然满足）
   - **`threadLocalWarpDs` 语义**：每线程至多缓存一个 VRT，**路径变更即重开**（原实现 thread_local 首次初始化后忽略 path 参数，同线程换路径会错拿旧句柄）
   - **轴序双轨**：warp 内部 SRS 用 `OAMS_AUTHORITY_COMPLIANT`（与 gdal CLI 行为一致）；自研 bbox 数值变换用 TRADITIONAL——两套 SRS 实例分开，混用会全 0 或 bbox 塌缩
   - 瓦片渲染：数据交集在瓦片内的**像素偏移**必须计算（交集≠瓦片原点时直读会拉伸错位）；用 `GDALRasterIOExtraArg.eResampleAlg` 在读取时降采样（窗口→瓦片子区 1:1 放置），内存恒定
9. **多源镶嵌 VRT 的 decim 缺陷（workflow 端到端踩坑）**：自研"SimpleSource 引用匀色 VRT（派生波段）"的镶嵌 VRT 在**降采样读取**时产生横向黑条+内容错位（1:1 读取正常！诊断必须做 decim 场景）；**嵌套派生波段不能用于需要 decim/overview 的下游**。修复：镶嵌 VRT 用 **ComplexSource 引用原始影像 + `<ScaleRatio>/<ScaleOffset>` 内联线性匀色参数**（GDAL 原生支持，结构对齐 gdalbuildvrt：UseMaskBand + SourceProperties + 浮点 rect）；**VrtSource.gain 数组仅 3 波段，band≥3（alpha）循环严禁越界访问**（曾致全瓦片无效）
10. **Phase 2 验证锚点（2026-10-13）**：CGCS2000 z10~z14 候选瓦片 502 张与 gdal raster tile 逐级一致（2/8/28/96/368）；z14 全瓦片公共有效区 RGB diff 3.54（bilinear vs CLI near）；GF2 边缘瓦片 alpha 25% + 有效区 RGB [25,27,23] 与 Phase 0 C 方案一致；skip-empty 15898 张正确跳过。性能：2024 mosaic z10-14 PNG 502 张 9s（4 线程）；GF2 无 overview 源 z17-18 需建 .ovr 提速（方案 4.3 已定，tiling 已实现 build_overviews=auto）。
11. **瓦片"写出/续跑"计数必须用独立返回码**：断点续跑跳过若返回 0（与"写出"同码），会被同时计入 written → 统计重复（实测 4+4+12 = 20 > 候选 16，且掩盖续跑是否真的生效）。定稿 `renderTile` 返回 **4 = 续跑跳过**（0=写出/1=空/2=阈值/3=数据外）；gtest `RenderTile.ResumeSkipsExisting` 锁定。复核验证：第二次运行「写出 0，续跑跳过 4，数据外 12」。
12. **依赖最小化：OpenCV 不引入**（2026-10-13 完整评估）：本项目计算负载为 ①`++hist[v]` 直方图统计 ②`clip(g·x+o)`/`lut[x]` 逐像素映射 ③重采样/编码（全交 GDAL）④小规模稠密 LS（自研高斯消元，N=景数）。OpenCV 三大优势——优化 imgproc 滤波、稠密线代、CUDA——分别对应本项目未实现的融合羽化、过小的解算规模、无 GPU 环境；唯一理论受益点 `cv::LUT` 处非瓶颈（带宽型，-O3 已饱和）。且实测 `--as-needed` 已将其从二进制剔除（`DT_NEEDED` 无 opencv），故仅为声明冗余。定稿：`vcpkg.json` 与 CMake 均移除；再引入条件见《技术方案》§3.6。

## 9. 迭代 2 与 Phase 2 验证结果（2026-10-13）

| 项 | 结果 |
|---|---|
| 最小二乘图平差 | GF2 gain=2.71/2.54/2.74（4 条约束边均衡），各边残差 2.0~7.0（平方和最优，替代单边锚定链式） |
| block_wallis | 输出 mean=81.4/85.1/67.1 ≈ 参考景统计（按构造正确；target=ref 全局统计语义） |
| hist_match | 输出 vs 参考 CDF 最大偏差 1.6~2.6%（与 Phase 0 锚点 2.1~2.7% 一致） |
| VRT 零落盘 | 2024 四景匀色 VRT ×4 ≈10.8KB + mosaic.vrt 9.1KB ≈ **20KB**（对比物理输出 240MB），逐景自检 PASS（早前"36KB"表述有误，已按落盘实测更正） |
| 组合模式 | overlap_adjust+wallis GF2×2024 全流程通过（5 景块式处理，内存安全） |
| **tiling 格网** | CGCS2000 z10~14 共 502 瓦片与 gdal raster tile **逐级一致**（2/8/28/96/368） |
| **tiling 内容** | z14 全瓦片公共有效区 RGB diff=3.54（bilinear vs CLI near，合理）；边缘瓦片 alpha 覆盖两侧一致 |
| **tiling nodata** | GF2 z17-18：skip-empty 正确跳过 15898 张全无效瓦片；边缘瓦片 alpha 25% + 有效区 RGB [25,27,23] 与 Phase 0 透明对策一致 |
| **tiling 性能** | 2024 mosaic z10-14 PNG 502 张 9s（4 线程）；GF2 无 ovr 源 13min（auto overview 已实现，见 workflow） |
| **workflow 端到端** | 2024 恒等：匀色 VRT 6s → 切片 z12-14 492 张 22s，**零像素落盘**；断点续跑 492 全跳过（2.7s）；vs 纯 tiling z14 diff=2.55（残差=镶嵌源 near decim vs ComplexSource UseMaskBand 的边缘差异，量级合理） |
| **S2 单元测试** | gtest 23/23 通过（block_stats 5 / tms 6 / solver 7 / render 4）；真实 4490 影像→CGCS2000 z13 冒烟 24 瓦片正常（AutoCreateWarpedVRT 定稿后回归） |
| **本轮复核修复（24/24）** | 新增 `RenderTile.ResumeSkipsExisting`：续跑返回码 4 与"写出"区分——第二次运行「写出 0，续跑跳过 4，数据外 12」=候选 16，计数守恒 |
| **apply 模式** | 按既有方案冻结参数生成匀色 VRT（方案 JSON 不被修改，mtime/大小不变）；4 景 gain=1.00 恒等保持；CLI 与 workflow（`profile_mode=apply`）双通道验证通过 |
| **target_mean/std** | global_adjust 显式目标 μ=100/110/90、σ=40/35/30：3 景 gain 与 σ_t/σ 逐波段一致（误差 <1e-6，参考景按设计恒等）；workflow JSON 数字/数组两种写法均解析通过 |
| **workflow 参数与交付** | workflow 现解析 profile_mode/scene_format/mosaic_cog/target/wallis_target_std/cache_mb/tile_buffer/num_processes/use_gpu/keep_tmp_vrt/tiles_scope/overview_policy；`cmake --install` 现含 tiling/workflow/libip_tiling.so + share/gdal/tms_CGCS2000.json |
| **S3 服务化（Phase 4）** | 端到端：POST 提交 → 进度 10→85→100 → done → 经 `GET /tiles/...` 取瓦片（HTTP 200/30837B）；并发：workers=1 时 A=running / B=queued；**崩溃隔离**：kill -9 worker → 任务 failed(137)、服务 health 200、队列自动续跑下一任务；取消：DELETE running → canceled；优雅退出：SIGTERM → "开始优雅退出"；重启恢复：运行中任务重启后标 `failed/服务重启中断`（可重提交），历史任务从磁盘恢复 |

## 9.1 workflow 管线结构（2026-10-13 定稿）

```
workflow exe（同进程）
├─ color_balance（overlap_adjust 图平差 + 接边门控）→ 输出：
│   ① 每景匀色 VRT（VRTDerivedRasterBand + ip_gain_offset，进程内自检）
│   ② mosaic.vrt（ComplexSource 直接引用【原始影像】+ ScaleRatio/ScaleOffset 内联匀色参数
│      + UseMaskBand + SourceProperties + 浮点 rect —— 完全对齐 gdalbuildvrt 成熟结构）
│   ③ 方案 JSON（含每景 gain/offset 与每边均衡残差）
└─ tiling（读 mosaic.vrt → WarpedVRT → 瓦片并行渲染）
    ⚠ 不读 ①（嵌套派生波段的 decim 读取有缺陷——横向黑条+内容错位，实测坑）；
    mosaic.vrt ② 无派生层，decim/overview/跨进程全部走 GDAL 成熟路径。
    像素函数注册（ensureColorBalancePixelFunc）仅 ① 的自检需要。
```
