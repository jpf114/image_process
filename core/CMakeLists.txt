add_library(ip_core STATIC
    src/log.cpp
    src/mem_guard.cpp
    src/gdal_raii.cpp
    src/block_stats.cpp
    src/thread_pool.cpp
    src/json_io.cpp
)
target_include_directories(ip_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ip_core PUBLIC GDAL::GDAL nlohmann_json::nlohmann_json)
find_package(Threads REQUIRED)
target_link_libraries(ip_core PUBLIC Threads::Threads)
