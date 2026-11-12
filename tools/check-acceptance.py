#!/usr/bin/env python3
"""§15.4 第 2–4 条的验收检查。用法：tools/check-acceptance.py [benchmarks/results/<日期>.json]

- 第 2 条：每一项 aw（缓存开）的分配次数不多于 asio 的同一项（asio 没有的项记 n/a，不计失败）。
- 第 3 条：await_child、await_child_nested_1000、bridge_same_thread、bridge_cross_thread 的中位耗时不超过 asio 的 1.2 倍。
- 第 4 条：热路径（§0）在缓存打开时每次 0 次全局 operator new：await_child、bridge_same_thread、
  bridge_cross_thread、co_spawn、strand_post、generator_yield_1000。
全部通过时退出码 0，否则 1。
"""
import glob, json, os, sys

path = sys.argv[1] if len(sys.argv) > 1 else sorted(glob.glob(os.path.join(os.path.dirname(__file__), "..", "benchmarks", "results", "*.json")))[-1]
doc = json.load(open(path))
aw, asio = doc["results"]["aw-bench"], doc["results"].get("asio-bench", {})
failures = 0
def report(ok, text):
    global failures
    failures += 0 if ok else 1
    print(("PASS " if ok else "FAIL ") + text)
for item in sorted(aw):
    if item not in asio:
        print("n/a  allocations %s: asio has no counterpart" % item)
        continue
    report(aw[item]["news"] <= asio[item]["news"], "allocations %s: aw %.3g <= asio %.3g" % (item, aw[item]["news"], asio[item]["news"]))
for item in ["await_child", "await_child_nested_1000", "bridge_same_thread", "bridge_cross_thread"]:
    ratio = aw[item]["ns"] / asio[item]["ns"]
    report(ratio <= 1.2, "time %s: aw/asio = %.2f (<= 1.20)" % (item, ratio))
for item in ["await_child", "bridge_same_thread", "bridge_cross_thread", "co_spawn", "strand_post", "generator_yield_1000"]:
    report(aw[item]["news"] == 0, "zero-allocation %s: %.3g new/op" % (item, aw[item]["news"]))
print("ACCEPTANCE %s (%d failure(s)) — %s" % ("OK" if failures == 0 else "FAILED", failures, os.path.relpath(path)))
sys.exit(0 if failures == 0 else 1)
