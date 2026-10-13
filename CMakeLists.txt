cmake_minimum_required(VERSION 3.20)

# vcpkg 依赖策略：使用全局 classic 安装（/home/tdt-dell/vcpkg），禁用 manifest 模式。
# 根目录 vcpkg.json 仅作依赖清单/版本说明，不触发安装。
# 必须在 project() 之前设置（toolchain 于 project() 时读取）。
set(VCPKG_MANIFEST_MODE OFF CACHE BOOL "vcpkg manifest mode (OFF: use global classic install)" FORCE)

project(image_process LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# core 静态库需并入 operator 动态库，必须 PIC
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 输出目录：bin/<CONFIG>（Debug/Release 分离），build 目录建议 build/{debug,release}
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/$<CONFIG>)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/$<CONFIG>)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/$<CONFIG>)

# 安装 RPATH：可执行与 so 均可在 bin/ 同级找到 lib/ 依赖（打包带走可用）
set(CMAKE_INSTALL_RPATH "$ORIGIN:$ORIGIN/../lib")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain")
endif()

# GPU 编译开关（占位）：本期纯 CPU（决策 4）。开启仅暴露 IP_ENABLE_GPU 宏，
# 运行参数 use_gpu 在此宏下才走 GPU 分支（当前无实现，仍按 CPU 执行）。
option(IP_ENABLE_GPU "启用 GPU 编译开关（本期占位，默认纯 CPU）" OFF)
if(IP_ENABLE_GPU)
    add_compile_definitions(IP_ENABLE_GPU=1)
endif()

# 依赖说明：GDAL + nlohmann-json 为全部所需依赖。
# OpenCV 经完整评估（2026-10-13）确认当前无使用必要——其优势（优化 imgproc 滤波、
# 稠密线代、CUDA）分别对应本项目未实现的融合羽化、过小的解算规模、无 GPU 环境；
# 唯一理论受益点 cv::LUT 逐像素映射处本项目非瓶颈（纯带宽型，-O3 循环已饱和）。
# 结论：不引入 OpenCV 依赖；再引入条件见 技术方案.md §3.6。
find_package(GDAL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
# 服务化控制面（header-only HTTP，Phase 4）
find_package(httplib CONFIG REQUIRED)

add_subdirectory(core)
add_subdirectory(operators)
add_subdirectory(apps)
# enable_testing 必须在顶层（CTestTestfile 自顶向下生成）
enable_testing()
add_subdirectory(tests)

# 安装规则：仅装交付物（operator 动态库 + 全部 exe），core 静态并入动态库不单独安装
install(TARGETS ip_color_balance ip_tiling color_balance tiling workflow ip_server
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION bin)
# 随库分发 CGCS2000 TMS 定义（供 GDAL CLI / 外部工具使用；自研引擎内置该方案）
install(FILES operators/tiling/data/tms_CGCS2000.json
        DESTINATION share/gdal)
