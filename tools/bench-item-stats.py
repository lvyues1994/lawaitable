#!/usr/bin/env python3
"""对单个基准项做多轮测量：aw-bench 与 asio-bench 交替各跑 N 次（同一机器、taskset 绑核），
输出两边耗时的中位数 / p10 / p90 和中位数比值。先运行 tools/run-benchmarks.sh 生成 build/bench。

用法：tools/bench-item-stats.py <项> [次数=20] [cpu 列表=2]
例：  tools/bench-item-stats.py bridge_cross_thread 30 2
"""
import os, re, statistics, subprocess, sys

item = sys.argv[1]
runs = int(sys.argv[2]) if len(sys.argv) > 2 else 20
cpus = sys.argv[3] if len(sys.argv) > 3 else "2"
bench = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "bench", "benchmarks")
pattern = re.compile(r"^bench %s ns=([0-9.]+) news=([0-9.]+)" % re.escape(item), re.M)

def pct(xs, p):
    xs = sorted(xs)
    k = (len(xs) - 1) * p
    lo = int(k)
    hi = min(lo + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)

samples = {"aw-bench": [], "asio-bench": []}
news = {}
for _ in range(runs):
    for name in samples:  # 交替运行，减少机器状态漂移对单边的影响
        out = subprocess.run(["taskset", "-c", cpus, os.path.join(bench, name)], check=True,
                             capture_output=True, text=True).stdout
        m = pattern.search(out)
        if m is None:
            sys.exit("%s: item %s not found" % (name, item))
        samples[name].append(float(m.group(1)))
        news[name] = float(m.group(2))

print("item %s, %d runs each, taskset -c %s" % (item, runs, cpus))
print("| | 中位数 ns | p10 ns | p90 ns | new/op |")
print("| --- | --- | --- | --- | --- |")
for name, xs in samples.items():
    print("| %s | %.1f | %.1f | %.1f | %.3g |" % (name, statistics.median(xs), pct(xs, 0.1), pct(xs, 0.9), news[name]))
aw, asio = samples["aw-bench"], samples["asio-bench"]
print("ratio aw/asio: median %.2f, p10 %.2f, p90 %.2f" % (statistics.median(aw) / statistics.median(asio),
                                                         pct(aw, 0.1) / pct(asio, 0.1), pct(aw, 0.9) / pct(asio, 0.9)))
