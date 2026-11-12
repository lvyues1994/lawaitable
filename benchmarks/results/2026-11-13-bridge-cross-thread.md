## bridge_cross_thread 多轮测量（2026-11-13，UTC+8）

用户决定（验收未通过项第 1 项）：对 bridge_cross_thread 做多轮测量，不改库。工具：`tools/bench-item-stats.py`，aw-bench 与 asio-bench 交替运行，同一机器（Intel Xeon，8 核，clang 22.1.8，-O2 -DNDEBUG）。

### 与 run-benchmarks.sh 相同的条件：全部线程绑在 1 个核（taskset -c 2），各 30 次

| | 中位数 ns | p10 ns | p90 ns | new/op |
| --- | --- | --- | --- | --- |
| aw-bench | 4490.3 | 4206.0 | 4724.9 | 1 |
| asio-bench | 3092.3 | 2943.8 | 3211.1 | 1 |
ratio aw/asio: median 1.45, p10 1.43, p90 1.47

### 参考：绑在 2 个核（taskset -c 2,3），各 20 次

| | 中位数 ns | p10 ns | p90 ns | new/op |
| --- | --- | --- | --- | --- |
| aw-bench | 5914.4 | 4926.8 | 6767.9 | 1 |
| asio-bench | 8607.5 | 8420.2 | 8863.8 | 1 |
ratio aw/asio: median 0.69, p10 0.59, p90 0.76

### 观察

- 单核条件下比值稳定在 1.43–1.47（p10–p90），不是测量噪声；按 §15.4 第 3 条（单核、中位数）仍未通过。
- 整个基准进程的上下文切换（单核，5 次取中位）：aw 自愿 194996 / 非自愿 195000；asio 自愿 211914 / 非自愿 156722。aw 多出约 0.38 次/操作的非自愿切换（被抢占）。
- aw 的 thread_pool（移植自 coro 的工作窃取池）在停车前会自旋：10 轮 pause 指数退避加 2 次 yield。单核时这段自旋和 completer 线程抢同一个核。诊断构建（只在 /tmp，把 spinRounds / yieldRounds 设为 0，未提交）单核中位数 4505 → 4160 ns，约缩小 8%，仍是 asio 的 1.35 倍。
- 两核时情况相反：aw 0.69 倍于 asio（5914 对 8607 ns）。自旋避免了一次睡眠 / 唤醒往返；关掉自旋后 aw 变成 8938 ns，与 asio 相当。
- 机器上没有 perf / strace，也没有 root，剩余约 1.35 倍的差距无法做到函数级归因。可能的来源：非库线程投递回 thread_pool 的路径（注入队列加锁、条件变量 notify）比 asio scheduler 多一次系统调用或锁往返。
