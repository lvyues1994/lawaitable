#!/usr/bin/env bash
# 用法：tools/verify-core-verbatim.sh   （在仓库根目录运行）
# 校验规格 §15.4 第 5 条：
#   core 文件相对 coro 0be5ab2+01/02/03 只有 include 路径改写；
#   夹具头文件、legacy 测试逐字节相同；转发头只有两行固定内容。
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
src=$(mktemp -d)
trap 'rm -rf "$src"' EXIT
"$root/tools/patched-coro.sh" "$src/coro"
fail=0
while read -r f; do
    a=$src/coro/include/co2/$f
    b=$root/include/awaitable/core/$f
    [ -f "$b" ] || { echo "MISSING core/$f"; fail=1; continue; }
    # 所有差异行都必须是 include 行，且把 awaitable/core/ 还原成 co2/ 后逐字节相同
    other=$(diff "$a" "$b" | grep -E '^[<>]' | grep -vE '^[<>] #include "(co2|awaitable/core)/' || true)
    [ -z "$other" ] || { echo "NON-INCLUDE DIFF core/$f"; echo "$other"; fail=1; }
    cmp -s "$a" <(sed 's|^#include "awaitable/core/|#include "co2/|' "$b") \
        || { echo "DIFF core/$f"; fail=1; }
    n=$(diff "$a" "$b" | grep -c '^>' || true)
    echo "core/$f: include-rewrites=$n"
    fwd=$root/tests/legacy/include/co2/$f
    [ "$(cat "$fwd" 2>/dev/null)" = "$(printf '#pragma once\n#include "awaitable/core/%s"' "$f")" ] \
        || { echo "BAD FORWARDER co2/$f"; fail=1; }
done < "$root/tools/core-manifest.txt"
while read -r f; do
    cmp -s "$src/coro/include/co2/$f" "$root/tests/legacy/include/co2/$f" \
        || { echo "DIFF fixture co2/$f"; fail=1; }
done < "$root/tools/fixture-manifest.txt"
diff -r "$src/coro/tests" "$root/tests/legacy/tests" > /dev/null \
    || { echo "DIFF tests/legacy/tests"; fail=1; }
if [ $fail -eq 0 ]; then echo "VERBATIM OK"; else echo "VERBATIM FAIL"; exit 1; fi
