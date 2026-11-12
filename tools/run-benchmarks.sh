#!/usr/bin/env bash
# 用法：tools/run-benchmarks.sh   （在仓库根目录运行）
# §15.3：Release（-O2 -DNDEBUG）构建全部基准；每个二进制用 taskset 绑核跑 5 次，取中位数；
# 结果追加写入 benchmarks/results/<日期>.md，并写 benchmarks/results/<日期>.json 供 check-acceptance.py 使用。
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
AW_CXX=${AW_CXX:-/workspace/zigbin/zig-c++}
AW_ASIO_ROOT=${AW_ASIO_ROOT:-/workspace/asio}
AW_BENCH_CPU=${AW_BENCH_CPU:-2}
AW_BENCH_RUNS=${AW_BENCH_RUNS:-5}
build=$root/build/bench
raw=$build/raw
mkdir -p "$raw" "$root/benchmarks/results"
CXX=$AW_CXX cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" \
    -DAW_BUILD_TESTS=OFF -DAW_BUILD_BENCHMARKS=ON -DAW_ASIO_ROOT="$AW_ASIO_ROOT" > "$raw/configure.log" 2>&1
cmake --build "$build" > "$raw/build.log" 2>&1
rm -f "$raw"/*.out
for bin in aw-bench aw-bench-nocache asio-bench coro-bench native-bench; do
    for run in $(seq 1 "$AW_BENCH_RUNS"); do
        taskset -c "$AW_BENCH_CPU" "$build/benchmarks/$bin" > "$raw/$bin.$run.out"
    done
done
date_tag=$(date +%Y-%m-%d)
compiler=$("$AW_CXX" --version | head -1)
cpu=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')
python3 - "$raw" "$root/benchmarks/results/$date_tag" "$compiler" "$cpu" "$AW_BENCH_RUNS" <<'PY'
import glob, json, os, re, statistics, sys, datetime
raw, out_stem, compiler, cpu, runs = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5])
libs = ["aw-bench", "aw-bench-nocache", "asio-bench", "coro-bench", "native-bench"]
line = re.compile(r"^bench (\S+) ns=([\d.]+) news=([\d.]+) bytes=([\d.]+)$")
data, sizes = {}, {}
for lib in libs:
    per_item = {}
    for path in sorted(glob.glob(os.path.join(raw, lib + ".*.out"))):
        for text in open(path):
            m = line.match(text.strip())
            if m:
                per_item.setdefault(m.group(1), []).append(tuple(float(x) for x in m.groups()[1:]))
            elif text.startswith("size ") and lib == "aw-bench":
                parts = text.split()
                sizes[" ".join(parts[1:-1])] = int(parts[-1])
    data[lib] = {k: {"ns": statistics.median(v[0] for v in vs), "news": statistics.median(v[1] for v in vs),
                     "bytes": statistics.median(v[2] for v in vs)} for k, vs in per_item.items()}
items = ["create_destroy", "await_child", "await_child_nested_1000", "bridge_same_thread", "bridge_cross_thread",
         "co_spawn", "strand_post", "generator_yield_1000", "or_ready", "and_ready"]
json.dump({"compiler": compiler, "cpu": cpu, "runs": runs, "sizes": sizes, "results": data},
          open(out_stem + ".json", "w"), indent=2)
def cell(lib, item):
    r = data.get(lib, {}).get(item)
    return "n/a" if r is None else "%.1f ns / %.3g new / %.0f B" % (r["ns"], r["news"], r["bytes"])
with open(out_stem + ".md", "a") as md:
    md.write("\n## %s\n\n" % datetime.datetime.now().strftime("%Y-%m-%d %H:%M (UTC+8)"))
    md.write("- 编译器：%s\n- CPU：%s\n- 每项 %d 次运行取中位数，taskset 绑核；-O2 -DNDEBUG；aw 为 C++14，asio / 原生为 C++20\n\n" % (compiler, cpu, runs))
    md.write("| 项 | aw（缓存开） | aw（缓存关） | asio | coro | 原生 C++20 |\n| --- | --- | --- | --- | --- | --- |\n")
    for item in items:
        md.write("| %s | %s |\n" % (item, " | ".join(cell(lib, item) for lib in libs)))
    md.write("\n帧大小探针（§9.1）：" + "，".join("`%s` = %d B" % kv for kv in sorted(sizes.items())) + "\n")
print(out_stem + ".md")
PY
