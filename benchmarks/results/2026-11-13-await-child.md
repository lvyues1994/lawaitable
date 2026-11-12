# 验收修订第 3 项：await_child 快路径（2026-11-13）

## 归因（修改前，提交 9fa9c23）

工具：
- 单独的小基准（/tmp/xb/aw_c.cpp、asio_c.cpp，代码与 aw-bench / asio-bench 的 await_child 相同）。
- 用免 root 的 valgrind 3.24 callgrind 数指令（Debian 包，用 `dpkg -x` 解出）。跑 10 万次和 20 万次两轮，两轮之差除以次数即每次的指令数。
- 用 objdump 看热点函数的反汇编。

每次 await_child 的指令数：修改前 aw ≈ 300，asio ≈ 189。aw 层多出来的部分：

| 段 | aw 修改前 | asio 对应 | 原因 |
| --- | --- | --- | --- |
| 子帧分配（`thread_info::allocate`） | ≈ 40，外加 TLS ≈ 12 | ≈ 13（内联） | 没有内联，而且对齐检查 `p % align` 的 align 是运行期值，编译成了 `div` 指令（反汇编可见），一次就要几十个周期 |
| 子帧回收（`deallocate` + TLS） | ≈ 12 | ≈ 6 + 4 | `current_info()` 先读 `thread_context*` 再读 `info_`，多一次间接访问和判空 |
| `throw_if_cancelled_now()` | ≈ 19（不内联的调用） | 约 2（内联） | 函数体里有 `throw std::system_error` 的构造，编译器放弃内联 |
| `rethrow_exception()` | ≈ 11（不内联的调用） | 内联 | 同上，函数体里有 move 和重抛 |
| 帧析构时 `~exception_ptr` | ≈ 15（库调用，即使为空也调） | ≈ 15 | 两边相同，但这是 aw 层能省掉的部分 |
| 帧栈 push/pop、pump 循环、resume_context | ≈ 36 | 相当 | 与 asio 结构一致，没有改 |
| core：宏协程状态机、`coroutine_handle::resume` 的契约检查、帧头、await 槽、销毁 | 约 120 | 原生协程 | 属于 core，不能改 |

## 修改（只改 aw 层）

- `exec/detail/thread_context.hpp`：
  - `thread_info::allocate` 的快路径强制内联，对齐检查改用掩码（align 是 2 的幂，`recycling_allocate` 契约要求不超过 `max_align_t`）。
  - 未命中路径拆到不内联的 `allocate_miss`。
  - `current_info()` 改为直接读一份线程局部的 `thread_info*`，嵌套语义不变。
- `impl/awaitable_frame.hpp`：
  - `throw_if_cancelled_now()` 和 `rethrow_exception()` 只把判断留在内联路径，抛出部分移到不内联的函数里。
  - 新增 `detail::pending_exception`：`std::exception_ptr` 放进 union，只在非空时析构。大小不变，`awaitable_frame_base` 仍为 40 B。
- `impl/awaitable_thread.hpp`、`impl/entry_point.hpp`：跟着改用 `take()` / `store()`。

修改后每次 await_child 约 222 条指令（asio 约 189）。

## 多轮测量（`tools/bench-item-stats.py <项> 30 2`，aw 与 asio 交替各 30 次，单核）

| 项 | 版本 | aw 中位数（p10–p90） | asio 中位数（p10–p90） | aw/asio 中位数（p10，p90） |
| --- | --- | --- | --- | --- |
| await_child | 修改前 | 15.1（14.9–16.2）ns | 10.8（10.6–11.4）ns | 1.40（1.41，1.42） |
| await_child | 修改后 | 11.9（11.6–12.6）ns | 10.9（10.6–11.3）ns | 1.09（1.09，1.11） |
| await_child_nested_1000 | 修改前 | 41207.6（39721.6–44484.4）ns | 38428.2（37222.9–40758.9）ns | 1.07（1.07，1.09） |
| await_child_nested_1000 | 修改后 | 39546.7（38461.5–41567.8）ns | 39017.5（37749.9–40459.5）ns | 1.01（1.02，1.03） |
