#!/usr/bin/env bash
# 用法：tools/patched-coro.sh <输出目录>
# 在 <输出目录> 生成 coro 0be5ab2 + 补丁 01→02→03 的源码树。
# 补丁默认取仓库内 tools/coro-patches/（与 /workspace/coro-issues/*/fix.patch 逐字节相同）；
# CORO_REPO 需含提交 0be5ab2（github.com/lvyues1994/coro）。
set -euo pipefail
out=$1
CORO_REPO=${CORO_REPO:-/workspace/coro}
CORO_ISSUES=${CORO_ISSUES:-$(cd "$(dirname "$0")" && pwd)/coro-patches}
CORO_COMMIT=0be5ab2
rm -rf "$out"
mkdir -p "$out"
git -C "$CORO_REPO" archive "$CORO_COMMIT" | tar -x -C "$out"
for p in 01-runramp-double-destroy 02-d9-ill-formed-accepted 03-perf-alloc-sync; do
    (cd "$out" && git apply --whitespace=nowarn "$CORO_ISSUES/$p/fix.patch")
done
