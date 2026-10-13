# 单元测试（gtest；数值锚点见 AGENT.md §4）。enable_testing 在顶层 CMakeLists。
find_package(GTest CONFIG REQUIRED)

add_executable(ip_tests
    test_block_stats.cpp
    test_tms.cpp
    test_solver.cpp
    test_render.cpp
    test_params_json.cpp
)
target_include_directories(ip_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/operators/color_balance/src
    ${CMAKE_SOURCE_DIR}/operators/tiling/src)
target_link_libraries(ip_tests PRIVATE
    ip_core ip_color_balance ip_tiling GTest::gtest_main)
target_compile_options(ip_tests PRIVATE -Wno-unused-result)

include(GoogleTest)
gtest_discover_tests(ip_tests DISCOVERY_TIMEOUT 60)
