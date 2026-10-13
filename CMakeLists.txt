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

find_package(GDAL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
# OpenCV 后续 Wallis/直方图匹配阶段引入（vcpkg.json 已声明）
find_package(OpenCV QUIET COMPONENTS core imgproc)

add_subdirectory(core)
add_subdirectory(operators)
add_subdirectory(apps)
# enable_testing 必须在顶层（CTestTestfile 自顶向下生成）
enable_testing()
add_subdirectory(tests)

# 安装规则：仅装交付物（operator 动态库 + exe），core 静态并入动态库不单独安装
install(TARGETS ip_color_balance ip_tiling color_balance tiling
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION bin)
