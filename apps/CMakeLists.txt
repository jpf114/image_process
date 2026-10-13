add_executable(color_balance color_balance_main.cpp)
target_link_libraries(color_balance PRIVATE ip_color_balance)

# tiling 也链匀色库：读每景匀色 VRT（VRTDerivedRasterBand + ip_gain_offset）需进程内注册像素函数
# scene_fanout：目录输入 → 按景并行（派生 workflow 子进程）
add_executable(tiling tiling_main.cpp scene_fanout.cpp)
target_link_libraries(tiling PRIVATE ip_tiling ip_color_balance)

add_executable(workflow workflow_main.cpp scene_fanout.cpp)
target_link_libraries(workflow PRIVATE ip_color_balance ip_tiling)

# 服务化控制面（Phase 4）：HTTP 薄层 + 任务队列 + worker 进程池（fork/exec → workflow）
add_executable(ip_server
    server/server_main.cpp
    server/job_manager.cpp
)
target_link_libraries(ip_server PRIVATE ip_core httplib::httplib)
