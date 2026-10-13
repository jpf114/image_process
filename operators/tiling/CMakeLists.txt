add_library(ip_tiling SHARED
    src/tiling.cpp
    src/tile_ctx.cpp
    src/package_sink.cpp
    src/tms.cpp
)
target_include_directories(ip_tiling PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ip_tiling PUBLIC ip_core)
