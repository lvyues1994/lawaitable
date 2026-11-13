# awaitable

[![CI](https://github.com/lvyues1994/lawaitable/actions/workflows/ci.yml/badge.svg)](https://github.com/lvyues1994/lawaitable/actions/workflows/ci.yml)

基于 co2 C++14 宏协程核心、按 asio 协程模型重建任务层的协程库：
执行线程 + 帧栈 + pump、asio 式取消（cancellation_signal / slot / state）、executor 亲和与 strand、
co_spawn 与完成 token、`||` / `&&`，并保留 async_generator 与 thread_pool / manual_executor。

当前阶段：只有设计规格，尚无实现代码。

- 设计规格：[docs/superpowers/specs/2026-11-12-awaitable-design.md](docs/superpowers/specs/2026-11-12-awaitable-design.md)
