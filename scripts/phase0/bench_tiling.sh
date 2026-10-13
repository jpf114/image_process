#!/bin/bash
# Phase 0.5 性能基准：gdal raster tile 线程扩展性 + 编码格式对比
# 数据：2024 四景镶嵌 VRT（27801x18538，4490）-> WebMercatorQuad z12-z15
# 用时统计由 python 汇总
set -e
source ~/miniconda3/etc/profile.d/conda.sh
conda activate geo_env
export GDAL_DATA=/tmp/p0/gdal_data
MOSAIC=/tmp/p0/mosaic4490.vrt
OUT=/tmp/p0/bench
mkdir -p $OUT

run() {
  local name=$1; shift
  rm -rf $OUT/$name
  /usr/bin/time -f "%e" -o $OUT/${name}.time \
    gdal raster tile -i $MOSAIC -o $OUT/$name \
    --tiling-scheme WebMercatorQuad --convention xyz \
    --min-zoom 12 --max-zoom 15 "$@" >/dev/null 2>&1
  local t=$(cat $OUT/${name}.time)
  local n=$(find $OUT/$name -name "*.png" -o -name "*.jpg" -o -name "*.webp" | wc -l)
  local sz=$(du -sm $OUT/$name | cut -f1)
  echo "$name: ${t}s tiles=$n size=${sz}MB"
}

echo "== 线程扩展性（PNG, z12-z15）=="
for j in 1 2 4 8 16; do
  run "threads_$j" -f PNG -j $j
done
echo "== 编码格式对比（8 线程, z12-z15）=="
run "fmt_png"  -f PNG  -j 8
run "fmt_jpeg" -f JPEG -j 8
run "fmt_webp" -f WEBP -j 8
echo "== 完成 =="
