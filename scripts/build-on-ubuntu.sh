#!/bin/sh
# 在配套 Ubuntu 工具链中构建成对程序，既支持仓库也支持独立测试包。
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -d "$script_dir/project/server" ]; then
    package_dir=$script_dir
    source_dir=$script_dir/project/server
else
    package_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
    source_dir=$package_dir/server
fi
compiler=${1:-arm-linux-gnueabihf-gcc}
compiler=$(command -v "$compiler") || {
    printf '%s\n' '找不到 ARM 编译器，请把完整路径作为第一个参数。' >&2
    exit 1
}
case $("$compiler" -dumpmachine) in
    arm*gnueabihf*) ;;
    *) printf '%s\n' '编译器目标不是 ARM hard-float，停止构建。' >&2; exit 1 ;;
esac
mkdir -p "$package_dir/build-arm"
binary=$package_dir/build-arm/tcp_ping_server_mp
camera=$package_dir/build-arm/camera_service
log=$package_dir/build-arm/build.log
if ! make -B -C "$source_dir" CC="$compiler" TARGET="$binary" > "$log" 2>&1; then
    cat "$log"
    exit 1
fi
cat "$log"
readelf_bin=${compiler%gcc}readelf
if [ ! -x "$readelf_bin" ]; then readelf_bin=$(command -v readelf); fi
{
    date -u '+%Y-%m-%dT%H:%M:%SZ'
    "$compiler" --version
    "$compiler" -dumpmachine
    "$compiler" -print-sysroot
    for executable in "$binary" "$camera"; do
        file "$executable"
        sha256sum "$executable"
        "$readelf_bin" -h "$executable"
        "$readelf_bin" -l "$executable"
        "$readelf_bin" -d "$executable"
        "$readelf_bin" --version-info "$executable"
    done
} > "$package_dir/build-arm/build-manifest.txt" 2>&1
file "$binary" "$camera"
sha256sum "$binary" "$camera"
printf '两个程序编译完成：%s\n' "$package_dir/build-arm"
printf 'ABI 与依赖信息：%s\n' "$package_dir/build-arm/build-manifest.txt"
