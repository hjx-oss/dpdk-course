# 用 Bash 执行 source ./env.sh；所有默认路径都从本文件的位置计算。
if [ -z "${BASH_VERSION:-}" ]; then
    printf '请在 Bash 中执行 source ./env.sh\n' >&2
    return 1 2>/dev/null || exit 1
fi
DPDK_COURSE_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)" || return 1
DPDK_INSTALL_PREFIX="${DPDK_INSTALL_PREFIX:-$DPDK_COURSE_ROOT/.deps/install}"
export DPDK_COURSE_ROOT DPDK_INSTALL_PREFIX
_dpdk_course_prepend() {
    local variable="$1" directory="$2" current
    current="${!variable:-}"
    case ":$current:" in
        *":$directory:"*) ;;
        *) printf -v "$variable" '%s' "$directory${current:+:$current}" ;;
    esac
    export "$variable"
}
_dpdk_course_prepend PATH "$DPDK_COURSE_ROOT/.deps/tools/bin"
_dpdk_course_prepend PATH "$DPDK_INSTALL_PREFIX/bin"
_dpdk_course_prepend PKG_CONFIG_PATH "$DPDK_INSTALL_PREFIX/lib/pkgconfig"
_dpdk_course_prepend LD_LIBRARY_PATH "$DPDK_INSTALL_PREFIX/lib"
unset -f _dpdk_course_prepend
