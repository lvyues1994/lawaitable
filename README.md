# awaitable

[![CI](https://github.com/lvyues1994/lawaitable/actions/workflows/ci.yml/badge.svg)](https://github.com/lvyues1994/lawaitable/actions/workflows/ci.yml)

基于 co2 C++14 宏协程核心、按 asio 协程模型重建任务层的协程库：
执行线程 + 帧栈 + pump、asio 式取消（cancellation_signal / slot / state）、executor 亲和与 strand、
co_spawn 与完成 token、`||` / `&&`，并保留 async_generator 与 thread_pool / manual_executor。

头文件库（`include/`），最低 C++14，C++17 / C++20 下同样可用。

- 设计规格：[docs/superpowers/specs/2026-11-12-awaitable-design.md](docs/superpowers/specs/2026-11-12-awaitable-design.md)

## 平台支持

| 平台 | 工具链 | 状态 |
| --- | --- | --- |
| Linux x86_64 | GCC 14、Clang 18/20（libstdc++ / libc++） | CI 全矩阵：Debug / Release / ASan+UBSan / TSan × C++14/17/20 |
| Android arm64-v8a / x86_64 | NDK r25c（Clang 14 + libc++，`android-24` 起） | 库与全部测试交叉编译通过（C++14/17/20）；CI 有 arm64 交叉编译 job |
| QNX 7.1 aarch64le / x86_64 | SDP 7.1 的 qcc / q++（GCC 8.3 + LLVM libc++） | aarch64le：库与全部测试交叉编译通过（C++14/17/20，用 SDP 自带的 GCC 8.3 验证）；x86_64 preset 未验证；需要 QNX 许可证，不进 CI |

交叉编译只构建库和测试可执行文件，测试要拷到目标机上运行（ctest 不会在主机上执行它们）。

```sh
# Android：ANDROID_NDK 指向 NDK 根目录
export ANDROID_NDK=/path/to/android-ndk-r25c
cmake --preset android-arm64-release -DAW_TEST_CXX_STANDARD=17
cmake --build --preset android-arm64-release

# QNX 7.1：先 source SDP 的环境脚本（导出 QNX_HOST / QNX_TARGET）
source /path/to/qnx710/qnxsdp-env.sh
cmake --preset qnx-aarch64le-release -DAW_TEST_CXX_STANDARD=17
cmake --build --preset qnx-aarch64le-release
```

平台相关说明：

- **原生协程对照测试**：`tests/differential` 与 `tests/asio_parity`（以及 legacy 的 `differential_tests.cpp`）需要标准库的 `<coroutine>`。GCC 8（QNX 7.1）和 NDK r25c 的 libc++ 都没有，CMake 检测到后自动跳过这几项；库本身不依赖 `<coroutine>`。
- **QNX 严格标准模式**：`-std=c++NN`（无 GNU 扩展）下 QNX 的头文件会隐藏 libc++ `<thread>` / `<mutex>` 需要的 POSIX 扩展，必须定义 `_QNX_SOURCE`。工具链文件 `cmake/toolchains/qnx710-*.cmake` 已经加上；自带工具链的用户需要自己加。
- **QNX 的 `thread_local`**：SDP 7.1 的 GCC 以 `--disable-tls` 构建，`thread_local` 走 emutls（每次访问是一次函数调用）。功能不受影响，但帧回收缓存、strand 调用栈等热路径上的 TLS 访问比 Linux 慢。
- **线程池的可用核数**：Linux / Android 用 `sched_getaffinity` 读取亲和性掩码；QNX 退回 `hardware_concurrency`（不读 runmask）。
- **GCC 8 告警**：GCC 8 + libc++ 会在 libc++ 头文件的默认实参处报 `-Wzero-as-null-pointer-constant`，测试在 GCC < 9 下把这一条和 `-Wpragmas` 降为警告；库头文件本身不触发。
