add_executable(color_balance color_balance_main.cpp)
target_link_libraries(color_balance PRIVATE ip_color_balance)

add_executable(tiling tiling_main.cpp)
target_link_libraries(tiling PRIVATE ip_tiling)

add_executable(workflow workflow_main.cpp)
target_link_libraries(workflow PRIVATE ip_color_balance ip_tiling)

# 服务化控制面（Phase 4）：HTTP 薄层 + 任务队列 + worker 进程池（fork/exec → workflow）
add_executable(ip_server
    server/server_main.cpp
    server/job_manager.cpp
)
target_link_libraries(ip_server PRIVATE ip_core httplib::httplib)
