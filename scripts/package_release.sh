#!/usr/bin/env bash
# 打包脚本：构建 Release → 安装 → 递归收集全部动态依赖（so）→ 拷贝 GDAL/PROJ 数据
#           → 生成启动环境脚本 → tar.gz，可直接拷走解压使用
# 用法: scripts/package_release.sh [输出目录]（默认 dist/）
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VCPKG="${VCPKG_ROOT:-$HOME/vcpkg}"
TOOLCHAIN="$VCPKG/scripts/buildsystems/vcpkg.cmake"
BUILD_DIR="$ROOT/build/release"
DIST_ROOT="${1:-$ROOT/dist}"
STAGE="$DIST_ROOT/image_process"

# 目标机假设：与构建机同为 x86_64 Linux 且 glibc 不旧于构建机（系统库不打包）

# ---------- 1. 构建 Release ----------
cmake -S "$ROOT" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DVCPKG_MANIFEST_MODE=OFF \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"$(nproc)"

# ---------- 2. 安装（默认 Release）----------
rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/lib" "$STAGE/share"
cmake --install "$BUILD_DIR" --prefix "$STAGE" --config Release

# ---------- 3. 递归收集动态依赖（仅 vcpkg/非系统库）----------
VCPKG_LIB="$VCPKG/installed/x64-linux/lib"
VCPKG_BIN="$VCPKG/installed/x64-linux/bin"
is_system_lib() {
    case "$1" in
        /lib/*|/lib64/*|/usr/lib/*|/usr/lib64/*) return 0 ;;
        *) return 1 ;;
    esac
}
collect_deps() {
    local f="$1"
    # ldd 输出中提取 so 绝对路径
    ldd "$f" 2>/dev/null | awk '/=> \// {print $3} /^\s*\// {print $1}' |
    while read -r so; do
        [ -f "$so" ] || continue
        is_system_lib "$so" && continue
        local base
        base="$(basename "$so")"
        case "$base" in
            libip_*) continue ;;   # 自家库已在 bin/，避免重复
        esac
        if [ ! -f "$STAGE/lib/$base" ]; then
            cp -L "$so" "$STAGE/lib/$base"
            echo "  + $base"
            collect_deps "$STAGE/lib/$base"   # 递归
        fi
    done || true
}
echo "== 收集动态依赖 =="
for f in "$STAGE"/bin/* "$STAGE"/lib/*; do
    [ -f "$f" ] && file "$f" | grep -q "shared object\|pie executable\|dynamically linked" && collect_deps "$f" || true
done

# ---------- 4. GDAL/PROJ 运行时数据 ----------
GDAL_SHARE="$VCPKG/installed/x64-linux/share"
[ -d "$GDAL_SHARE/gdal" ] && cp -r "$GDAL_SHARE/gdal" "$STAGE/share/gdal" && echo "  + share/gdal"
[ -d "$GDAL_SHARE/proj" ] && cp -r "$GDAL_SHARE/proj" "$STAGE/share/proj" && echo "  + share/proj"

# TODO(tiling operator): 届时把项目 data/tms_CGCS2000.json 拷入 $STAGE/share/gdal/

# ---------- 5. 启动环境脚本 ----------
cat > "$STAGE/bin/ipenv.sh" <<'EOF'
#!/usr/bin/env bash
# source 本文件后可直接运行 bin/ 下程序
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export LD_LIBRARY_PATH="$DIR/lib:${LD_LIBRARY_PATH:-}"
export GDAL_DATA="$DIR/share/gdal"
export PROJ_DATA="$DIR/share/proj"
export PROJ_LIB="$DIR/share/proj"   # 兼容旧 PROJ 变量名
EOF
chmod +x "$STAGE/bin/ipenv.sh"

# ---------- 6. tar.gz ----------
cd "$DIST_ROOT"
tar czf image_process.tar.gz image_process
echo "== 完成 =="
echo "安装包: $DIST_ROOT/image_process.tar.gz"
echo "使用: tar xzf image_process.tar.gz && source image_process/bin/ipenv.sh && image_process/bin/color_balance --help"
