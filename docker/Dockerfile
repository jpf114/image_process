# ip_server 容器镜像（Phase 4 服务化）
#
# 前置：先生成交付包并解出构建上下文
#   scripts/package_release.sh
#   mkdir -p docker/ctx && tar xzf dist/image_process.tar.gz -C docker/ctx
# 构建：
#   docker build -f docker/Dockerfile -t image-process:0.1 docker/ctx
# 运行（--init 可省，镜像内已用 tini 作 PID 1 转发信号）：
#   docker run --rm -p 8080:8080 --cpus 4 --memory 8g \
#     -v "$PWD/ipdata:/data" -v "$PWD/tiles:/tiles" \
#     image-process:0.1 --data-dir /data --tiles-dir /tiles --workers 2
#
# 依据《Phase4服务化论证报告.md》§六：
#  - 显式线程数（GDAL_NUM_THREADS）——容器内 CPU 探测受 cgroup 配额影响会高估
#  - tini 作 PID 1 → SIGTERM 正确转发，ip_server 优雅退出并回收 worker
#  - 数据/瓦片走卷（避免写入容器可写层）
FROM debian:12-slim

# 运行时最小依赖：libstdc++（GDAL/自研 exe 需要）+ tini（信号转发）
RUN apt-get update \
 && apt-get install -y --no-install-recommends libstdc++6 tini \
 && rm -rf /var/lib/apt/lists/*

# 交付包（含全部非系统 so + GDAL/PROJ 数据 + bin/ipenv.sh）
COPY image_process /opt/image_process

ENV TZ=Asia/Shanghai \
    GDAL_DATA=/opt/image_process/share/gdal \
    PROJ_DATA=/opt/image_process/share/proj \
    PROJ_LIB=/opt/image_process/share/proj \
    LD_LIBRARY_PATH=/opt/image_process/lib \
    GDAL_NUM_THREADS=4

VOLUME ["/data", "/tiles"]
EXPOSE 8080

ENTRYPOINT ["/usr/bin/tini", "--", "/opt/image_process/bin/ip_server"]
CMD ["--data-dir", "/data", "--tiles-dir", "/tiles", "--workers", "2"]