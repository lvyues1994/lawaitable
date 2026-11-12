#!/usr/bin/env bash
# 用法：tools/run-matrix.sh [配置...]   （在仓库根目录运行）
# 配置名 = <preset>-cxx<14|17|20>，preset ∈ debug release asan tsan；默认跑全部 12 个。
# 编译器：AW_CXX（默认 /workspace/zigbin/zig-c++）；ASan+UBSan 用 AW_CXX_ASAN
# （默认 /workspace/zigbin/clang20++，带 compiler-rt 的 clang 20 + libc++）。
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
AW_CXX=${AW_CXX:-/workspace/zigbin/zig-c++}
AW_CXX_ASAN=${AW_CXX_ASAN:-/workspace/zigbin/clang20++}
# asio 对照组（tests/asio_parity）；设为空字符串可跳过。
AW_ASIO_ROOT=${AW_ASIO_ROOT-/workspace/asio}
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1}
configs=("$@")
if [ ${#configs[@]} -eq 0 ]; then
    for p in debug release asan tsan; do for s in 14 17 20; do configs+=("$p-cxx$s"); done; done
fi
status=0
for c in "${configs[@]}"; do
    preset=${c%-cxx*}; std=${c##*-cxx}
    cxx=$AW_CXX; [ "$preset" = asan ] && cxx=$AW_CXX_ASAN
    dir=$root/build/matrix/$c
    log=$root/build/matrix/$c.log
    mkdir -p "$root/build/matrix"
    if ! (cd "$root" && CXX=$cxx cmake --preset "$preset" -B "$dir" -DAW_TEST_CXX_STANDARD="$std" -DAW_ASIO_ROOT="$AW_ASIO_ROOT") > "$log" 2>&1; then
        echo "$c: CONFIGURE FAILED (see $log)"; status=1; continue
    fi
    if ! cmake --build "$dir" -j8 >> "$log" 2>&1; then
        echo "$c: BUILD FAILED (see $log)"; status=1; continue
    fi
    summary=$(cd "$dir" && ctest -j4 --timeout 300 --output-on-failure 2>&1 | tee -a "$log" | grep -E "tests passed")
    echo "$c: $summary"
    case $summary in "100% tests passed"*) ;; *) status=1;; esac
done
exit $status
