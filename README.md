# awaitable

[![CI](https://github.com/lvyues1994/lawaitable/actions/workflows/ci.yml/badge.svg)](https://github.com/lvyues1994/lawaitable/actions/workflows/ci.yml)

基于 co2 C++14 宏协程核心、按 asio 协程模型重建任务层的协程库：
执行线程 + 帧栈 + pump、asio 式取消（cancellation_signal / slot / state）、executor 亲和与 strand、
co_spawn 与完成 token、`||` / `&&`、有界 channel，并保留 async_generator 与 thread_pool / manual_executor。

头文件库（`include/`），最低 C++14，C++17 / C++20 下同样可用。

- 设计规格：[docs/superpowers/specs/2026-11-12-awaitable-design.md](docs/superpowers/specs/2026-11-12-awaitable-design.md)
- channel 增补规格：[docs/superpowers/specs/2026-11-13-channel-design.md](docs/superpowers/specs/2026-11-13-channel-design.md)

## channel（有界队列，生产快、消费慢时用）

`async_generator` 是拉式的（消费者拉一次，生产者走一步），背压天然但没有缓冲；真实数据源（相机、解码器、网络）
不会因为没人拉就停下，这时用 `aw::channel<T, Executor>`：多生产者多消费者、线程安全，满了按策略处理。

| 策略 | 满队列时 | 场景 |
| --- | --- | --- |
| `channel_policy::block` | 发送方等待空位（容量 0 即 rendezvous） | 录制、转码，不能丢 |
| `channel_policy::drop_oldest` | 丢掉最旧的，新值入队 | 实时预览，低延迟 |
| `channel_policy::drop_newest` | 丢掉新来的 | 只要先到的连续帧 |

```cpp
#include <awaitable.hpp>
using pool_ex = aw::thread_pool::executor_type;

aw::thread_pool pool{2};
aw::channel<frame, pool_ex> ch{pool, 3, aw::channel_policy::drop_oldest};
ch.set_drop_handler([](frame&& f) { recycle(std::move(f)); });   // 可选：被丢弃的帧还给缓冲池；也可读 ch.dropped()

// 相机线程（非库线程）：try_send 从不阻塞；block 策略下可以 ch.async_send(f, aw::use_future).get()
ch.try_send(std::move(f));
ch.close();                                                       // 接收方取完剩余元素后得到 channel_closed

// 消费协程
auto display(aw::channel<frame, pool_ex>* ch)
    CO2_BEG((aw::awaitable<void, pool_ex>), (ch), std::tuple<std::error_code, frame> r{};) {
    for (;;) {
        CO2_AWAIT_SET(r, ch->async_receive(aw::as_tuple(aw::use_awaitable)));
        if (std::get<0>(r)) break;                                // aw::channel_errc::channel_closed
        show(std::get<1>(r));
    }
    CO2_RETURN();
}
CO2_END
```

- 接口：`async_send(v, token)` → `void(error_code)`；`async_receive(token)` → `void(error_code, T)`（`T` 不可默认构造时为 `aw::optional<T>`）；
  `try_send` / `try_receive`；`close()`；`cancel()`（等待中的操作以 `channel_cancelled` 完成）。
- 取消：等待中的操作支持 slot 取消（terminal / partial / total），以 `aw::error::operation_aborted` 完成。
- 完成总是 post 到回调的关联 executor，协程在自己的 executor / strand 上恢复。
- 分配：缓冲区在构造时一次分配；打开回收缓存后，同线程收发循环稳态零分配。
- 完整示例（三种策略的对比输出、把 channel 包成 `async_generator`）：[examples/video_stream.cpp](examples/video_stream.cpp)。

## 平台支持

| 平台 | 工具链 | 状态 |
| --- | --- | --- |
| Linux x86_64 | GCC 14、Clang 18/20（libstdc++ / libc++） | CI 全矩阵：Debug / Release / ASan+UBSan / TSan × C++14/17/20 |
| Android arm64-v8a / x86_64 | NDK r25c（Clang 14 + libc++，`android-24` 起） | 库与全部测试交叉编译通过（C++14/17/20）；CI 有 arm64 交叉编译 job |
| QNX 7.1 aarch64le / x86_64 | SDP 7.1 的 qcc / q++（GCC 8.3 + LLVM libc++） | 用 qcc 交叉编译库与全部测试通过：aarch64le Release C++14/17/20 + Debug C++17，x86_64 Release C++17；需要 QNX 许可证，不进 CI |

交叉编译只构建库和测试可执行文件，测试要拷到目标机上运行（ctest 不会在主机上执行它们）。

```sh
# Android：ANDROID_NDK 指向 NDK 根目录
export ANDROID_NDK=/path/to/android-ndk-r25c
cmake --preset android-arm64-release -DAW_TEST_CXX_STANDARD=17
cmake --build --preset android-arm64-release

# QNX 7.1：先 source SDP 的环境脚本（导出 QNX_HOST / QNX_TARGET）
source /path/to/qnx710/qnxsdp-env.sh
# 许可证随 SDP 放在 <SDP>/.qnx/license 时（qnxsdp-env.sh 默认指向 ~/.qnx），把配置目录指过去
export QNX_CONFIGURATION=/path/to/qnx710/.qnx QNX_CONFIGURATION_EXCLUSIVE=/path/to/qnx710/.qnx
cmake --preset qnx-aarch64le-release -DAW_TEST_CXX_STANDARD=17
cmake --build --preset qnx-aarch64le-release
```

平台相关说明：

- **原生协程对照测试**：`tests/differential` 与 `tests/asio_parity`（以及 legacy 的 `differential_tests.cpp`）需要标准库的 `<coroutine>`。GCC 8（QNX 7.1）和 NDK r25c 的 libc++ 都没有，CMake 检测到后自动跳过这几项；库本身不依赖 `<coroutine>`。
- **QNX 严格标准模式**：`-std=c++NN`（无 GNU 扩展）下 QNX 的头文件会隐藏 libc++ `<thread>` / `<mutex>` 需要的 POSIX 扩展，必须定义 `_QNX_SOURCE`。工具链文件 `cmake/toolchains/qnx710-*.cmake` 已经加上；自带工具链的用户需要自己加。
- **QNX 的 `thread_local`**：SDP 7.1 的 GCC 以 `--disable-tls` 构建，`thread_local` 走 emutls（每次访问是一次函数调用）。功能不受影响，但帧回收缓存、strand 调用栈等热路径上的 TLS 访问比 Linux 慢。
- **线程池的可用核数**：Linux / Android 用 `sched_getaffinity` 读取亲和性掩码；QNX 退回 `hardware_concurrency`（不读 runmask）。
- **GCC 8 告警**：GCC 8 + libc++ 会在 libc++ 头文件的默认实参处报 `-Wzero-as-null-pointer-constant`，测试在 GCC < 9 下把这一条和 `-Wpragmas` 降为警告；库头文件本身不触发。
