#!/usr/bin/env bash
# 用法：tools/import-core.sh   （在仓库根目录运行）
# 按规格 §3：core 文件改写 include 后放进 include/awaitable/core/；
# 夹具头文件与 coro 测试原样放进 tests/legacy/；为 core 文件生成一行转发头。
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
src=$(mktemp -d)
trap 'rm -rf "$src"' EXIT
"$root/tools/patched-coro.sh" "$src/coro"
core_dst=$root/include/awaitable/core
fix_dst=$root/tests/legacy/include/co2
rm -rf "$core_dst" "$fix_dst" "$root/tests/legacy/tests"
mkdir -p "$core_dst/detail" "$fix_dst/detail"
sed_expr=()
while read -r f; do
    sed_expr+=(-e "s|^#include \"co2/${f}\"|#include \"awaitable/core/${f}\"|")
done < "$root/tools/core-manifest.txt"
while read -r f; do
    sed "${sed_expr[@]}" "$src/coro/include/co2/$f" > "$core_dst/$f"
    printf '#pragma once\n#include "awaitable/core/%s"\n' "$f" > "$fix_dst/$f"
done < "$root/tools/core-manifest.txt"
while read -r f; do
    cp "$src/coro/include/co2/$f" "$fix_dst/$f"
done < "$root/tools/fixture-manifest.txt"
cp -r "$src/coro/tests" "$root/tests/legacy/tests"
echo "imported $(wc -l < "$root/tools/core-manifest.txt") core files, $(wc -l < "$root/tools/fixture-manifest.txt") fixture files, $(find "$root/tests/legacy/tests" -type f | wc -l) legacy test files"
