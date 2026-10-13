add_executable(color_balance color_balance_main.cpp)
target_link_libraries(color_balance PRIVATE ip_color_balance)

add_executable(tiling tiling_main.cpp)
target_link_libraries(tiling PRIVATE ip_tiling)

add_executable(workflow workflow_main.cpp)
target_link_libraries(workflow PRIVATE ip_color_balance ip_tiling)
