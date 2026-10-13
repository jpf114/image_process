add_library(ip_color_balance SHARED
    src/color_balance.cpp
    src/apply_and_profile.cpp
    src/graph_ls.cpp
    src/vrt_writer.cpp
)
target_include_directories(ip_color_balance PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ip_color_balance PUBLIC ip_core)
