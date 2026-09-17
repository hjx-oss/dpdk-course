#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
source "$project_dir/env.sh"
test "$(uname -s)" = Linux || { echo '请在 Linux 中安装 DPDK。' >&2; exit 1; }
for tool in cc make pkg-config python3 curl tar; do
    command -v "$tool" >/dev/null || { echo "缺少 $tool，请先安装 README 中的系统依赖。" >&2; exit 1; }
done
pkg-config --exists numa || { echo '缺少 libnuma-dev。' >&2; exit 1; }
deps_dir="$project_dir/.deps"
mkdir -p "$deps_dir/downloads" "$deps_dir/sources"
python3 -m venv "$deps_dir/tools"
"$deps_dir/tools/bin/python" -m pip install meson==1.9.1 ninja==1.13.0 pyelftools==0.32
archive="$deps_dir/downloads/dpdk-25.11.3.tar.xz"
if [ ! -f "$archive" ]; then
    curl --fail --location --retry 3 https://fast.dpdk.org/rel/dpdk-25.11.3.tar.xz -o "$archive"
fi
printf '%s  %s\n' 3719acc586b310c4f60ba230683bf4f1e12c6f2f5bee11f7c01b1bbd0ded7490 "$archive" | sha256sum -c -
if [ ! -d "$deps_dir/sources/dpdk-stable-25.11.3" ]; then
    tar -xf "$archive" -C "$deps_dir/sources"
fi
if [ ! -f "$deps_dir/build/build.ninja" ]; then
    meson setup "$deps_dir/build" "$deps_dir/sources/dpdk-stable-25.11.3" \
        --prefix="$DPDK_INSTALL_PREFIX" --libdir=lib -Dplatform=generic \
        -Denable_drivers=net/af_packet -Denable_apps=test-pmd -Dtests=false -Dexamples=''
fi
ninja -C "$deps_dir/build" -j "${DPDK_BUILD_JOBS:-4}"
ninja -C "$deps_dir/build" install
pkg-config --modversion libdpdk
echo '安装完成。新终端中执行 source ./env.sh 后运行课程。'
