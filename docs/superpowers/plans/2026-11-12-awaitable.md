# awaitable 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 `/workspace/awaitable` 中，以原样搬入的 co2 C++14 宏协程核心为底座，按 asio 的协程模型实现 `aw` 任务层（执行线程 + 帧栈 + pump、asio 式取消、executor 亲和与 strand、co_spawn 与完成 token、`||` / `&&`、async_generator、sync_wait），并通过规格 §14 的测试和 §15 的验收。

**Architecture:**
- 分层：core（`co2`，原样） ← exec / cancel ← awaitable 核心层 ← spawn 与桥接 ← 组合 / async_generator / sync_wait。
- 依赖只能沿这个方向，规格 §2.1 的依赖图是唯一依据。
- 每个任务交付一个可独立测试的模块。所有测试由 CMake / CTest 驱动；coro 原有的 22 个测试经 `tests/legacy` 夹具原样运行，贯穿全程作为 core 的回归。

**Tech Stack:**
- C++14（同时测 C++17 / C++20），header-only。
- CMake ≥ 3.21 + Ninja，CTest。
- 本机工具链：
  - `/workspace/zigbin/zig-c++`：clang 22 + libc++，用于 Debug / Release / TSan；
  - `/workspace/zigbin/clang20++`：clang 20 + libc++ + compiler-rt，用于 ASan + UBSan。
- 对照组：asio `190a8d8`（`/workspace/asio`），只在测试和基准里使用。

## Global Constraints

- 规格：`docs/superpowers/specs/2026-11-12-awaitable-design.md`，以下简称“规格”。所有任务都隐含遵守规格的全部条款；本节只摘出跨任务的硬约束。
- 协程机制：只用 co2 的 C++14 宏协程（`CO2_BEG` / `CO2_END` / `CO2_AWAIT*` / `CO2_YIELD` / `CO2_RETURN`），不依赖编译器协程支持。库代码不得包含 `<coroutine>`。
- 最低标准 C++14；测试矩阵为 C++14 / 17 / 20。
- 命名空间：新层一律 `aw`；core 保持 `co2`。宏前缀 `AW_`。公共名字用 snake_case（规格 §16 D1、D2）。
- core 原样：`include/awaitable/core/` 只允许与 coro `0be5ab2` + 补丁 01 → 02 → 03 存在 include 路径差异，由 `tools/verify-core-verbatim.sh` 检查。任何任务都不得修改 core 文件。
- 夹具原样：`tests/legacy/include/co2/` 的非转发头和 `tests/legacy/tests/` 与补丁后的 coro 逐字节相同。
- 编译告警：所有测试目标使用 coro 的告警集（`-Wall -Wextra -Wpedantic -Werror -Wconversion -Wsign-conversion -Wnon-virtual-dtor -Wold-style-cast -Wzero-as-null-pointer-constant -Werror=return-type -Werror=uninitialized -Werror=suggest-override -Werror=vla`）。
- 契约违规的退出码是 86（沿用 coro 的 `expect_exit_code.cmake`）。
- 明确不做（规格 §1.2）：
  - 跨挂起点 `try` / `catch`；
  - 带参 `operator new` 分配器；
  - HALO；
  - 捕获型 lambda 协程；
  - `any_io_executor` 及老 executor 模型；
  - `deferred` 和默认 token；
  - work 计数。
- 回收缓存：每个标签 2 格（`AW_RECYCLING_ALLOCATOR_CACHE_SIZE` 默认 2），4 字节粒度，只在库线程上生效；`AW_DISABLE_RECYCLING_ALLOCATOR` 全部关闭。
- `detached` 收到非空 `exception_ptr` 时重抛；`thread_pool` 中逸出的异常导致 `std::terminate`；`manual_executor::run` / `run_one` / `poll` 把异常传给调用者。
- 提交：每个任务结束时本地提交一次，提交身份 `yu.shang <yu.shang@thundersoft.com>`（只在本仓库设置）。**不 push。**
- 代码块是参考实现：执行任务时以该任务的测试为准。遇到编译错误可以就地修正实现细节，但不得改变本计划 Interfaces 中写明的名字、参数和返回类型；如果必须改，同步修改本计划后续任务的 Interfaces。

## 通用约定

- **配置构建目录（每个 clone 只需一次）：** `CXX=/workspace/zigbin/zig-c++ cmake --preset debug`。成功时输出的最后一行是 `-- Build files have been written to: /workspace/awaitable/build/debug`。
- **单测目标名：** `tests/unit/<name>.cpp` 对应目标和 CTest 名 `aw-<name>`，其中 `<name>` 的下划线换成连字符。例如 `tests/unit/manual_executor_test.cpp` 对应 `aw-manual-executor-test`。
- **全量回归：** `tools/run-matrix.sh`。12 个配置分别是 debug / release / asan / tsan 各配 cxx14 / 17 / 20；每行输出 `<配置>: 100% tests passed out of <N>`。

---

## 文件结构

```
/workspace/awaitable
├── CMakeLists.txt                       根：awaitable INTERFACE 库、AW_TEST_CXX_STANDARD、AW_BUILD_TESTS、AW_BUILD_BENCHMARKS、AW_ASIO_ROOT
├── CMakePresets.json                    debug / release / asan / tsan / debug-cxx17 / debug-cxx20（基准不走 preset，由 tools/run-benchmarks.sh 配置）
├── .gitignore                           build/
├── README.md
├── include/
│   ├── awaitable.hpp                    总头文件（Task 16）
│   └── awaitable/
│       ├── core/                        co2 核心（Task 1，原样 + include 改写）
│       ├── exec/
│       │   ├── detail/thread_context.hpp      thread_info 回收缓存 + thread_context 调用栈 + 启动/发起标记（Task 3）
│       │   ├── detail/recycling_allocator.hpp recycling_allocate / recycling_deallocate、mem_tag（Task 3）
│       │   ├── detail/operation.hpp           operation、function_operation、带标记队列项（Task 4）
│       │   ├── scheduler.hpp                  scheduler 接口、schedule_on（Task 4）
│       │   ├── executor.hpp                   basic_executor<Context>（Task 4）
│       │   ├── manual_executor.hpp            （Task 4）
│       │   ├── thread_pool.hpp                （Task 5）
│       │   └── strand.hpp                     strand<Executor>、make_strand（Task 6）
│       ├── cancel/
│       │   ├── cancellation_type.hpp          （Task 7）
│       │   ├── cancellation_signal.hpp        signal / slot / handler 存储（Task 7）
│       │   ├── cancellation_state.hpp         state / filter / 观察者钩子（Task 7）
│       │   ├── stop_token.hpp                 coro stop_token 移植（Task 8）
│       │   └── stop_token_adapter.hpp         get_stop_token / inherit_stop_token / 兼容名（Task 8）
│       ├── impl/
│       │   ├── awaitable_frame.hpp            awaitable_frame_base / awaitable_frame（Task 10；取消相关 await_transform 在 Task 13）
│       │   ├── awaitable_thread.hpp           awaitable_thread、entry_frame_base（Task 10，取消部分在 Task 13 扩充）
│       │   ├── entry_point.hpp                entry_frame_impl、co_spawn 发起（Task 10；spawn 取消桥在 Task 13）
│       │   ├── async_op_awaiter.hpp           use_awaitable 的 awaiter 与 handler（Task 11）
│       │   ├── parallel_group.hpp             || && 的组状态（Task 14）
│       │   └── generator_frame.hpp            async_generator 的 promise（Task 15）
│       ├── optional.hpp variant.hpp error.hpp multiple_exceptions.hpp   （Task 2）
│       ├── async_result.hpp associated.hpp detached.hpp use_future.hpp post.hpp dispatch.hpp （Task 9）
│       ├── awaitable.hpp this_coro.hpp co_spawn.hpp                      （Task 10，this_coro 在 Task 13 扩充）
│       ├── use_awaitable.hpp                                             （Task 11）
│       ├── as_tuple.hpp redirect_error.hpp                               （Task 12）
│       ├── operators.hpp                                                 （Task 14）
│       ├── async_generator.hpp                                           （Task 15）
│       └── sync_wait.hpp                                                 （Task 16）
├── tests/
│   ├── CMakeLists.txt                   include(AwTesting.cmake) + 各子目录
│   ├── AwTesting.cmake                  aw_unit_test / aw_contract_test / aw_compile_fail_test（Task 2）
│   ├── support/check.hpp                AW_CHECK（Task 2）
│   ├── support/counting_new.hpp         全局 operator new 计数（Task 3）
│   ├── support/test_coro.hpp            测试用小协程（Task 4）
│   ├── support/test_ops.hpp             async_value / async_immediate / async_from_foreign_thread 测试用异步操作（Task 11）
│   ├── support/test_cancellable_ops.hpp manual_event / async_cancellable_value 可取消的测试操作（Task 13）
│   ├── legacy/                          Task 1：夹具、原样测试、issue03 回归
│   ├── unit/                            Task 2–16
│   ├── differential/                    Task 17
│   ├── asio_parity/                     Task 18
│   └── stress/                          Task 19
├── benchmarks/                          Task 20：CMakeLists.txt、bench_common.hpp、aw_bench.cpp、asio_bench.cpp、coro_bench.cpp、native_bench.cpp、results/
└── tools/
    ├── core-manifest.txt fixture-manifest.txt   规格 §3.2 / §3.3 的文件清单
    ├── patched-coro.sh                  生成 coro 0be5ab2 + 01→02→03 的源码树
    ├── import-core.sh                   搬入 core、夹具和 legacy 测试
    ├── verify-core-verbatim.sh          原样校验
    ├── run-matrix.sh                    12 配置全量回归
    └── run-benchmarks.sh check-acceptance.py   （Task 20）
```

## 任务一览

| Task | 内容 | 累计测试数（run-matrix 每配置） |
| --- | --- | --- |
| 1 | 搬入 co2 core（原样 + include 改写），legacy 夹具，CMake / presets / 矩阵脚本，原样校验 | 27 |
| 2 | 测试基础设施，optional / variant / error / multiple_exceptions | 29 |
| 3 | 回收缓存与 thread_context | 31 |
| 4 | operation、scheduler、basic_executor、manual_executor | 33 |
| 5 | thread_pool 移植 | 35 |
| 6 | strand | 36 |
| 7 | cancellation_type / signal / slot / filter / state | 37 |
| 8 | stop_token 移植与适配器 | 38 |
| 9 | async_result / async_initiate / 关联器 / detached / use_future / post / dispatch | 39 |
| 10 | awaitable、帧栈、awaitable_thread pump、手写入口帧、co_spawn | 41 |
| 11 | use_awaitable 与直接 awaiter | 42 |
| 12 | as_tuple、redirect_error | 43 |
| 13 | 协程侧取消、co_spawn 取消桥、stop_token | 44 |
| 14 | `||` / `&&` | 45 |
| 15 | async_generator | 48 |
| 16 | sync_wait、总头文件、头文件自包含检查 | 51 |
| 17 | 帧栈差分测试 | 52 |
| 18 | asio 行为对照，`AW_ASIO_ROOT` | 53 |
| 19 | 并发压测 | 54 |
| 20 | 基准与 §15.4 验收检查（用户决策点） | 54（基准不进 ctest） |

## 与规格的已知偏差（执行前须知）

计划与规格不完全一致的地方如下。执行时按计划做，不要按规格“纠正”：
- `post.hpp` / `dispatch.hpp` 放在 `include/awaitable/` 顶层，而不是规格写的 `exec/` 下（Task 9）。
- 规格列出的 `impl/handlers.hpp` 没有单独建：handler 放在 `impl/async_op_awaiter.hpp`（Task 11）和 `impl/entry_point.hpp`（Task 10 / 13）里。
- `async_generator` 的左值 yield 是拷贝出去的，右值才移动（Task 15）。
- 异线程完成（`bridge_cross_thread`）不可能零分配：完成发生在非库线程，回收缓存按设计不在那里工作，asio 在这一项同样是 1 次。§15.4 第 4 条对这一项无法满足（Task 20）。
- asio 没有 async_generator，`generator_yield_1000` 只和 coro / 缓存关比较（Task 20）。
- 执行期修正（2026-11-13，用户同意）：Task 18 场景 6（affinity）的 asio 侧原先在 asio 池线程上 `std::vector<std::thread>::emplace_back` 启动外部线程，主线程在 `future.get()` 返回后遍历该 vector 并 join，二者存在数据竞争（TSan 下约 15% 失败，偶发 terminate）。现改为与 aw 侧相同的 `aw_test::foreign_threads`（加锁登记、析构时 join），并让它在 asio 线程池之前析构。只改测试，不改库。

---

### Task 1：搬入 core、建 CMake、legacy 夹具跑通 27 项

**交付物：** `include/awaitable/core/` 原样搬入（15 个文件）；`tests/legacy/` 夹具和原样测试；CMake 与 presets；`tools/` 的导入、校验、矩阵脚本。12 个配置下 legacy 全部 27 项通过。

**Files:**
- Create: `tools/core-manifest.txt`, `tools/fixture-manifest.txt`, `tools/patched-coro.sh`, `tools/import-core.sh`, `tools/verify-core-verbatim.sh`, `tools/run-matrix.sh`
- Create（由脚本生成）：`include/awaitable/core/**`（15 个文件），`tests/legacy/include/co2/**`（15 个转发头 + 13 个夹具头），`tests/legacy/tests/**`（coro 补丁后的 `tests/` 目录，32 个文件）
- Create: `CMakeLists.txt`, `CMakePresets.json`, `.gitignore`, `tests/CMakeLists.txt`, `tests/legacy/CMakeLists.txt`, `tests/legacy/issue03_allocation_counts_test.cpp`

**Interfaces:**
- Consumes：`/workspace/coro`（必须包含提交 `0be5ab2`）；`/workspace/coro-issues/0{1,2,3}-*/fix.patch`。
- Produces：
  - CMake 目标 `awaitable::awaitable`（INTERFACE，include 根是 `include/`，`cxx_std_14`，链接 `Threads::Threads`）。
  - 缓存变量 `AW_TEST_CXX_STANDARD`（14 / 17 / 20）和选项 `AW_BUILD_TESTS`。
  - presets：`debug`、`release`、`asan`、`tsan`、`debug-cxx17`、`debug-cxx20`。
  - 脚本：`tools/run-matrix.sh [配置...]` 和 `tools/verify-core-verbatim.sh`，后者成功时最后一行输出 `VERBATIM OK`。
  - core 头的 include 形式：`#include "awaitable/core/<路径>"`，例如 `awaitable/core/coroutine.hpp`、`awaitable/core/detail/result_storage.hpp`。

- [ ] **Step 1：写原样校验（先写“测试”）**

创建 `tools/core-manifest.txt`：

```text
config.hpp
contract.hpp
coroutine_handle.hpp
coroutine.hpp
dsl.hpp
generator.hpp
detail/await_slot.hpp
detail/awaitable.hpp
detail/frame.hpp
detail/move_only_function.hpp
detail/over_aligned.hpp
detail/preprocessor.hpp
detail/result_storage.hpp
detail/ring_queue.hpp
detail/uninitialized.hpp
```

创建 `tools/fixture-manifest.txt`：

```text
async_generator.hpp
callback.hpp
co2.hpp
env.hpp
manual_executor.hpp
scheduler.hpp
spawn.hpp
stop_token.hpp
sync_wait.hpp
task.hpp
thread_pool.hpp
when_all.hpp
detail/late_init.hpp
```

创建 `tools/patched-coro.sh`：

```bash
#!/usr/bin/env bash
# 用法：tools/patched-coro.sh <输出目录>
# 在 <输出目录> 生成 coro 0be5ab2 + /workspace/coro-issues 补丁 01→02→03 的源码树。
set -euo pipefail
out=$1
CORO_REPO=${CORO_REPO:-/workspace/coro}
CORO_ISSUES=${CORO_ISSUES:-/workspace/coro-issues}
CORO_COMMIT=0be5ab2
rm -rf "$out"
mkdir -p "$out"
git -C "$CORO_REPO" archive "$CORO_COMMIT" | tar -x -C "$out"
for p in 01-runramp-double-destroy 02-d9-ill-formed-accepted 03-perf-alloc-sync; do
    (cd "$out" && git apply --whitespace=nowarn "$CORO_ISSUES/$p/fix.patch")
done
```

创建 `tools/verify-core-verbatim.sh`：

```bash
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
```

- [ ] **Step 2：运行校验，确认失败**

Run: `chmod +x tools/*.sh && tools/verify-core-verbatim.sh; echo "exit=$?"`
Expected：先列出 `MISSING core/config.hpp` 等 15 行和 13 行 `DIFF fixture co2/…`，然后依次输出 `DIFF tests/legacy/tests`、`VERBATIM FAIL`、`exit=1`。

- [ ] **Step 3：写导入脚本并执行**

创建 `tools/import-core.sh`：

```bash
#!/usr/bin/env bash
# 用法：tools/import-core.sh   （在仓库根目录运行）
# 按规格 §3：core 文件改写 include 后放进 include/awaitable/core/；
# 夹具头文件与 coro 测试原样放进 tests/legacy/；为 core 文件生成一行转发头。
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
src=$(mktemp -d)
trap 'rm -rf "$src"' EXIT
"$root/tools/patched-coro.sh" "$src/coro"
core_dst=$root/include/awaitable/core
fix_dst=$root/tests/legacy/include/co2
rm -rf "$core_dst" "$fix_dst" "$root/tests/legacy/tests"
mkdir -p "$core_dst/detail" "$fix_dst/detail"
sed_expr=()
while read -r f; do
    sed_expr+=(-e "s|^#include \"co2/${f}\"|#include \"awaitable/core/${f}\"|")
done < "$root/tools/core-manifest.txt"
while read -r f; do
    sed "${sed_expr[@]}" "$src/coro/include/co2/$f" > "$core_dst/$f"
    printf '#pragma once\n#include "awaitable/core/%s"\n' "$f" > "$fix_dst/$f"
done < "$root/tools/core-manifest.txt"
while read -r f; do
    cp "$src/coro/include/co2/$f" "$fix_dst/$f"
done < "$root/tools/fixture-manifest.txt"
cp -r "$src/coro/tests" "$root/tests/legacy/tests"
echo "imported $(wc -l < "$root/tools/core-manifest.txt") core files, $(wc -l < "$root/tools/fixture-manifest.txt") fixture files, $(find "$root/tests/legacy/tests" -type f | wc -l) legacy test files"
```

Run: `chmod +x tools/import-core.sh && tools/import-core.sh && tools/verify-core-verbatim.sh | tail -1`
Expected：

```text
imported 15 core files, 13 fixture files, 32 legacy test files
VERBATIM OK
```

- [ ] **Step 4：写 CMake 和 03 回归测试**

创建 `CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.21)

project(awaitable VERSION 0.1.0 LANGUAGES CXX)

find_package(Threads REQUIRED)

add_library(awaitable INTERFACE)
add_library(awaitable::awaitable ALIAS awaitable)
target_compile_features(awaitable INTERFACE cxx_std_14)
target_link_libraries(awaitable INTERFACE Threads::Threads)
# core 的预处理器元编程需要标准一致的 __VA_ARGS__ 展开（沿用 coro）。
target_compile_options(awaitable INTERFACE $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>)
target_include_directories(
    awaitable INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                        $<INSTALL_INTERFACE:include>)

set(AW_TEST_CXX_STANDARD 14 CACHE STRING "C++ standard used to build the awaitable tests (14, 17 or 20)")
set_property(CACHE AW_TEST_CXX_STANDARD PROPERTY STRINGS 14 17 20)

option(AW_BUILD_TESTS "Build awaitable tests" ${PROJECT_IS_TOP_LEVEL})

if(AW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
```

创建 `tests/CMakeLists.txt`：

```cmake
add_subdirectory(legacy)
```

创建 `tests/legacy/CMakeLists.txt`：

```cmake
# coro 0be5ab2 + 01/02/03 的原有测试（tests/legacy/tests/，原样）对着搬入的 core 运行。
# 它们链接 co2::co2；这里把 co2::co2 定义成“core + 夹具头文件”。夹具里的 core 同名头只是
# 一行转发（见 tools/import-core.sh）。单独开一个 project，使原样的 tests/CMakeLists.txt
# 里 ${PROJECT_SOURCE_DIR}/include/co2 指向夹具目录（头文件自包含测试因此覆盖夹具与 core）。
project(awaitable_legacy LANGUAGES CXX)

add_library(awaitable_legacy_fixture INTERFACE)
add_library(co2::co2 ALIAS awaitable_legacy_fixture)
target_include_directories(awaitable_legacy_fixture INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(awaitable_legacy_fixture INTERFACE awaitable::awaitable)

set(CO2_TEST_CXX_STANDARD ${AW_TEST_CXX_STANDARD} CACHE STRING
    "C++ standard used to build the co2 tests" FORCE)

add_subdirectory(tests)

# 问题 03 的回归：分配次数断言（规格 §14.1）。
add_executable(co2-issue03-allocation-counts-test issue03_allocation_counts_test.cpp)
co2_configure_test_target(co2-issue03-allocation-counts-test)
add_test(NAME co2-issue03-allocation-counts-test COMMAND co2-issue03-allocation-counts-test)
```

创建 `tests/legacy/issue03_allocation_counts_test.cpp`：

```cpp
// 问题 03 的回归（规格 §14.1）：补丁 03 不改变分配次数，并且 syncWait 的两种完成顺序都正确。
//   syncWait(已就绪 Task)          每次 1 次分配（Task 帧）
//   syncWait(Task{whenAll(2)})     每次 5 次分配（父帧 + 2 个子帧 + 超槽的 awaiter + StopState）
//   spawn + run + join             每次 3 次分配（帧 + JoinState + StopState）
// 另在 ThreadPool 上跑“先等待、后完成”的 syncWait（走 parked 分支）。
//
// TSan 运行时自己定义了全局 operator new（与这里的替换版本链接冲突），所以 TSan 构建下
// 不替换、不计数，只跑行为检查；分配次数由其他配置验证。
#include <co2/co2.hpp>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define AW_ISSUE03_NO_COUNTING 1
#endif
#endif
#if defined(__SANITIZE_THREAD__) && !defined(AW_ISSUE03_NO_COUNTING)
#define AW_ISSUE03_NO_COUNTING 1
#endif

#include <cstdio>
#include <cstdlib>
#include <new>
#include <tuple>

namespace {
long allocations = 0;
int failures = 0;

#if defined(AW_ISSUE03_NO_COUNTING)
constexpr bool counting = false;
#else
constexpr bool counting = true;
#endif

void expectAllocations(char const* what, long actual, long expected);

void expectEqual(char const* what, long actual, long expected) {
    if (actual != expected) {
        std::printf("FAIL %s: %ld, expected %ld\n", what, actual, expected);
        ++failures;
    } else {
        std::printf("ok   %s: %ld\n", what, actual);
    }
}

void expectAllocations(char const* what, long actual, long expected) {
    if (counting) {
        expectEqual(what, actual, expected);
    } else {
        std::printf("skip %s (allocation counting disabled under TSan)\n", what);
    }
}

auto leaf(int v) CO2_BEG(co2::Task<int>, (v)) { CO2_RETURN(v); }
CO2_END

auto pairOf(int v) CO2_BEG(co2::Task<long>, (v), std::tuple<int, int> p{};) {
    CO2_AWAIT_SET(p, co2::whenAll(leaf(v), leaf(v)));
    CO2_RETURN(long{std::get<0>(p)} + std::get<1>(p));
}
CO2_END

auto onPool(co2::ThreadPool& pool, int v) CO2_BEG(co2::Task<int>, (pool, v)) {
    CO2_AWAIT(co2::scheduleOn(pool));
    CO2_RETURN(v + 1);
}
CO2_END

template <class F> long allocationsPerOp(int ops, F&& op) {
    long const before = allocations;
    for (int i = 0; i != ops; ++i) op(i);
    return (allocations - before) / ops;
}
} // namespace

#if !defined(AW_ISSUE03_NO_COUNTING)
void* operator new(std::size_t n) {
    ++allocations;
    if (void* p = std::malloc(n != 0U ? n : 1U)) return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#endif

int main() {
    long sum = 0;
    expectAllocations("syncWait(ready Task) allocations/op",
                allocationsPerOp(1000, [&](int i) { sum += co2::syncWait(leaf(i)); }), 1);
    expectAllocations("syncWait(Task{whenAll(2 ready)}) allocations/op",
                allocationsPerOp(1000, [&](int i) { sum += co2::syncWait(pairOf(i)); }), 5);
    co2::ManualExecutor executor;
    expectAllocations("spawn+run+join allocations/op", allocationsPerOp(1000, [&](int i) {
                    auto handle = co2::spawn(executor, leaf(i));
                    executor.run();
                    sum += handle.join();
                }),
                3);
    expectEqual("checksum", sum, 999L * 1000L / 2L * 4L);
    {
        co2::ThreadPool pool{2};
        long total = 0;
        for (int i = 0; i != 200; ++i) total += co2::syncWait(onPool(pool, i));
        expectEqual("syncWait on ThreadPool (wait before finish)", total, 199L * 200L / 2L + 200L);
    }
    return failures == 0 ? 0 : 1;
}
```

创建 `CMakePresets.json`：

```json
{
    "version": 3,
    "cmakeMinimumRequired": { "major": 3, "minor": 21, "patch": 0 },
    "configurePresets": [
        {
            "name": "base",
            "hidden": true,
            "generator": "Ninja",
            "binaryDir": "${sourceDir}/build/${presetName}",
            "cacheVariables": { "AW_TEST_CXX_STANDARD": "14" }
        },
        { "name": "debug", "inherits": "base", "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug" } },
        { "name": "release", "inherits": "base", "cacheVariables": { "CMAKE_BUILD_TYPE": "Release" } },
        {
            "name": "asan",
            "displayName": "ASan + UBSan",
            "inherits": "base",
            "cacheVariables": {
                "CMAKE_BUILD_TYPE": "Debug",
                "CMAKE_CXX_FLAGS": "-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g"
            }
        },
        {
            "name": "tsan",
            "inherits": "base",
            "cacheVariables": {
                "CMAKE_BUILD_TYPE": "Debug",
                "CMAKE_CXX_FLAGS": "-fsanitize=thread -fno-omit-frame-pointer -g"
            }
        },
        { "name": "debug-cxx17", "inherits": "debug", "cacheVariables": { "AW_TEST_CXX_STANDARD": "17" } },
        { "name": "debug-cxx20", "inherits": "debug", "cacheVariables": { "AW_TEST_CXX_STANDARD": "20" } }
    ],
    "buildPresets": [
        { "name": "debug", "configurePreset": "debug" },
        { "name": "release", "configurePreset": "release" },
        { "name": "asan", "configurePreset": "asan" },
        { "name": "tsan", "configurePreset": "tsan" },
        { "name": "debug-cxx17", "configurePreset": "debug-cxx17" },
        { "name": "debug-cxx20", "configurePreset": "debug-cxx20" }
    ],
    "testPresets": [
        { "name": "debug", "configurePreset": "debug", "output": { "outputOnFailure": true } },
        { "name": "release", "configurePreset": "release", "output": { "outputOnFailure": true } },
        {
            "name": "asan",
            "configurePreset": "asan",
            "output": { "outputOnFailure": true },
            "environment": { "ASAN_OPTIONS": "detect_leaks=0", "UBSAN_OPTIONS": "print_stacktrace=1" }
        },
        { "name": "tsan", "configurePreset": "tsan", "output": { "outputOnFailure": true } },
        { "name": "debug-cxx17", "configurePreset": "debug-cxx17", "output": { "outputOnFailure": true } },
        { "name": "debug-cxx20", "configurePreset": "debug-cxx20", "output": { "outputOnFailure": true } }
    ]
}
```

创建 `.gitignore`：

```text
build/
```

- [ ] **Step 5：Debug 构建并跑全部 legacy 测试**

Run: `CXX=/workspace/zigbin/zig-c++ cmake --preset debug && cmake --build build/debug -j8 && ctest --test-dir build/debug -j4`
Expected：最后几行是

```text
100% tests passed out of 27
```

再单独运行 issue03 测试：`build/debug/tests/legacy/co2-issue03-allocation-counts-test`。
Expected：

```text
ok   syncWait(ready Task) allocations/op: 1
ok   syncWait(Task{whenAll(2 ready)}) allocations/op: 5
ok   spawn+run+join allocations/op: 3
ok   checksum: 1998000
ok   syncWait on ThreadPool (wait before finish): 20100
```

- [ ] **Step 6：反向验证：这些测试能抓到回归**

在临时拷贝里，把 core 的 `frame.hpp` 和 `awaitable.hpp` 换回未打补丁的 0be5ab2 版本。预期 01 的回归用例和 02 的 4 个 compile-fail 都失败。

Run:

```bash
rm -rf /tmp/aw-neg && cp -r /workspace/awaitable /tmp/aw-neg && rm -rf /tmp/aw-neg/build && cd /tmp/aw-neg \
 && for f in detail/frame.hpp detail/awaitable.hpp; do git -C /workspace/coro show 0be5ab2:include/co2/$f \
      | sed 's|^#include "co2/|#include "awaitable/core/|' > include/awaitable/core/$f; done \
 && CXX=/workspace/zigbin/clang20++ cmake --preset asan -B build/neg > /dev/null && cmake --build build/neg -j8 > /dev/null \
 && ctest --test-dir build/neg -j4 | grep -E "tests passed|\(Failed\)"; cd /workspace/awaitable
```

Expected：

```text
81% tests passed, 5 tests failed out of 27
	  2 - co2-conformance-tests (Failed)
	 23 - co2-promise-operator-new-with-params-compile-fail-test (Failed)
	 24 - co2-final-suspend-not-noexcept-compile-fail-test (Failed)
	 25 - co2-return-void-and-return-value-compile-fail-test (Failed)
	 26 - co2-ambiguous-operator-co-await-compile-fail-test (Failed)
```

- [ ] **Step 7：写矩阵脚本，跑 12 个配置**

创建 `tools/run-matrix.sh`：

```bash
#!/usr/bin/env bash
# 用法：tools/run-matrix.sh [配置...]   （在仓库根目录运行）
# 配置名 = <preset>-cxx<14|17|20>，preset ∈ debug release asan tsan；默认跑全部 12 个。
# 编译器：AW_CXX（默认 /workspace/zigbin/zig-c++）；ASan+UBSan 用 AW_CXX_ASAN
# （默认 /workspace/zigbin/clang20++，带 compiler-rt 的 clang 20 + libc++）。
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
AW_CXX=${AW_CXX:-/workspace/zigbin/zig-c++}
AW_CXX_ASAN=${AW_CXX_ASAN:-/workspace/zigbin/clang20++}
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1}
configs=("$@")
if [ ${#configs[@]} -eq 0 ]; then
    for p in debug release asan tsan; do for s in 14 17 20; do configs+=("$p-cxx$s"); done; done
fi
status=0
for c in "${configs[@]}"; do
    preset=${c%-cxx*}; std=${c##*-cxx}
    cxx=$AW_CXX; [ "$preset" = asan ] && cxx=$AW_CXX_ASAN
    dir=$root/build/matrix/$c
    log=$root/build/matrix/$c.log
    mkdir -p "$root/build/matrix"
    if ! (cd "$root" && CXX=$cxx cmake --preset "$preset" -B "$dir" -DAW_TEST_CXX_STANDARD="$std") > "$log" 2>&1; then
        echo "$c: CONFIGURE FAILED (see $log)"; status=1; continue
    fi
    if ! cmake --build "$dir" -j8 >> "$log" 2>&1; then
        echo "$c: BUILD FAILED (see $log)"; status=1; continue
    fi
    summary=$(cd "$dir" && ctest -j4 --timeout 300 --output-on-failure 2>&1 | tee -a "$log" | grep -E "tests passed")
    echo "$c: $summary"
    case $summary in "100% tests passed"*) ;; *) status=1;; esac
done
exit $status
```

Run: `chmod +x tools/run-matrix.sh && tools/run-matrix.sh`
Expected：12 行，每行都是 `100% tests passed out of 27`：

```text
debug-cxx14: 100% tests passed out of 27
debug-cxx17: 100% tests passed out of 27
debug-cxx20: 100% tests passed out of 27
release-cxx14: 100% tests passed out of 27
release-cxx17: 100% tests passed out of 27
release-cxx20: 100% tests passed out of 27
asan-cxx14: 100% tests passed out of 27
asan-cxx17: 100% tests passed out of 27
asan-cxx20: 100% tests passed out of 27
tsan-cxx14: 100% tests passed out of 27
tsan-cxx17: 100% tests passed out of 27
tsan-cxx20: 100% tests passed out of 27
```

- [ ] **Step 8：提交**

```bash
git add CMakeLists.txt CMakePresets.json .gitignore include tests tools
git commit -m "build: import co2 core verbatim (0be5ab2 + issues 01/02/03), legacy fixture runs 27 tests"
```

### Task 2：测试基础设施与公共基础件（optional / variant / error / multiple_exceptions）

**交付物：** `tests/AwTesting.cmake` 提供三种测试目标函数，`tests/support/check.hpp` 提供 `AW_CHECK`；`aw::optional`、`aw::variant`、`aw::error::operation_aborted`、`aw::multiple_exceptions` 在 C++14 / 17 / 20 下通过单测。

**Files:**
- Create: `tests/AwTesting.cmake`, `tests/support/check.hpp`, `tests/unit/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`（整文件替换）
- Create: `include/awaitable/optional.hpp`, `include/awaitable/variant.hpp`, `include/awaitable/error.hpp`, `include/awaitable/multiple_exceptions.hpp`
- Test: `tests/unit/optional_variant_test.cpp`, `tests/unit/error_test.cpp`

**Interfaces:**
- Consumes（Task 1）：`awaitable::awaitable`、`AW_TEST_CXX_STANDARD`。
- Produces：
  - CMake 函数 `aw_unit_test(<name>)`、`aw_contract_test(<name>)`、`aw_compile_fail_test(<name>)`。`<name>` 是 `tests/<dir>/<name>.cpp` 的文件主名，目标名为 `aw-<name 中 _ 换成 ->`。compile-fail 源文件在定义了 `AW_EXPECT_COMPILE_FAILURE` 时必须编译失败。
  - `AW_CHECK(expr)`：失败时向 stderr 打印 `check failed at <file>:<line>: <expr>`，并以退出码 1 结束。
  - `aw::optional<T>`、`aw::nullopt_t`、`aw::nullopt`；`aw::variant<Ts...>`、`aw::monostate`、`aw::in_place_index_t<I>`、`aw::in_place_index<I>()`（C++14 下是函数，返回 `in_place_index_t<I>{}`）、`aw::get<I>(variant&)`、`aw::get_if<I>(variant*)`、`v.index()`。C++17 及以上全部别名到 `std::`；`in_place_index<I>()` 返回 `std::in_place_index_t<I>{}`。
  - `aw::error::errc { operation_aborted = 1 }`；`std::error_code aw::error::make_error_code(errc)`；`std::error_category const& aw::error::category()`（名字是 `"aw"`）；`std::is_error_code_enum<aw::error::errc>` 为真。
  - `class aw::multiple_exceptions : public std::exception { explicit multiple_exceptions(std::exception_ptr first) noexcept; std::exception_ptr first_exception() const; const char* what() const noexcept override; }`。

- [ ] **Step 1：写测试基础设施和失败的测试**

`tests/CMakeLists.txt`（替换全文）：

```cmake
include(${CMAKE_CURRENT_SOURCE_DIR}/AwTesting.cmake)
add_subdirectory(legacy)
add_subdirectory(unit)
```

`tests/AwTesting.cmake`：

```cmake
# 新层测试的公共设置：与 coro 相同的告警集；C++ 标准取 AW_TEST_CXX_STANDARD。
set(AW_TESTS_DIR ${CMAKE_CURRENT_LIST_DIR})

function(aw_configure_test target)
    target_link_libraries(${target} PRIVATE awaitable::awaitable)
    target_include_directories(${target} PRIVATE ${AW_TESTS_DIR}/support)
    set_target_properties(${target} PROPERTIES CXX_STANDARD ${AW_TEST_CXX_STANDARD}
                                               CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic -Werror -Wconversion -Wsign-conversion -Wnon-virtual-dtor
        -Wold-style-cast -Wzero-as-null-pointer-constant -Werror=return-type
        -Werror=uninitialized -Werror=suggest-override -Werror=vla)
endfunction()

function(aw_test_target_name name out_var)
    string(REPLACE "_" "-" stem "${name}")
    set(${out_var} "aw-${stem}" PARENT_SCOPE)
endfunction()

function(aw_unit_test name)
    aw_test_target_name(${name} target)
    add_executable(${target} ${name}.cpp)
    aw_configure_test(${target})
    add_test(NAME ${target} COMMAND ${target})
endfunction()

function(aw_contract_test name)
    aw_test_target_name(${name} target)
    add_executable(${target} ${name}.cpp)
    aw_configure_test(${target})
    add_test(NAME ${target}
             COMMAND ${CMAKE_COMMAND} -DTEST_EXECUTABLE=$<TARGET_FILE:${target}>
                     -DEXPECTED_EXIT_CODE=86
                     -P ${AW_TESTS_DIR}/legacy/tests/expect_exit_code.cmake)
endfunction()

function(aw_compile_fail_test name)
    aw_test_target_name(${name} target)
    add_library(${target}-control OBJECT ${name}.cpp)
    aw_configure_test(${target}-control)
    add_library(${target} OBJECT EXCLUDE_FROM_ALL ${name}.cpp)
    aw_configure_test(${target})
    target_compile_definitions(${target} PRIVATE AW_EXPECT_COMPILE_FAILURE)
    add_test(NAME ${target}
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target ${target} --config $<CONFIG>)
    set_tests_properties(${target} PROPERTIES WILL_FAIL TRUE RESOURCE_LOCK aw-build-tree)
endfunction()
```

`tests/support/check.hpp`：

```cpp
#pragma once

#include <cstdio>
#include <cstdlib>

#define AW_CHECK(expression)                                                           \
    do {                                                                               \
        if (!(expression)) {                                                           \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__,    \
                         #expression);                                                 \
            std::exit(1);                                                              \
        }                                                                              \
    } while (false)
```

`tests/unit/CMakeLists.txt`：

```cmake
aw_unit_test(optional_variant_test)
aw_unit_test(error_test)
```

`tests/unit/optional_variant_test.cpp`：

```cpp
#include <memory>
#include <string>
#include <utility>

#include "awaitable/optional.hpp"
#include "awaitable/variant.hpp"
#include "check.hpp"

namespace {

void optionalBasics() {
    aw::optional<int> empty;
    AW_CHECK(!empty.has_value());
    AW_CHECK(!empty);
    aw::optional<int> none{aw::nullopt};
    AW_CHECK(!none);
    aw::optional<std::string> text{std::string{"abc"}};
    AW_CHECK(text.has_value() && *text == "abc" && text->size() == 3U);
    AW_CHECK(text.value() == "abc");
    text.reset();
    AW_CHECK(!text);
    text.emplace(3U, 'x');
    AW_CHECK(*text == "xxx");
}

void optionalMoveOnly() {
    aw::optional<std::unique_ptr<int>> a{std::unique_ptr<int>{new int{7}}};
    aw::optional<std::unique_ptr<int>> b{std::move(a)};
    AW_CHECK(b && **b == 7);
    aw::optional<std::unique_ptr<int>> c;
    c = std::move(b);
    AW_CHECK(c && **c == 7);
}

void variantBasics() {
    aw::variant<aw::monostate, int, std::string> v;
    AW_CHECK(v.index() == 0U);
    aw::variant<aw::monostate, int, std::string> w{aw::in_place_index<2>(), "hi"};
    AW_CHECK(w.index() == 2U);
    AW_CHECK(aw::get<2>(w) == "hi");
    AW_CHECK(aw::get_if<1>(&w) == nullptr);
    AW_CHECK(aw::get_if<2>(&w) != nullptr);
    aw::variant<aw::monostate, int, std::string> moved{std::move(w)};
    AW_CHECK(moved.index() == 2U && aw::get<2>(moved) == "hi");
    aw::variant<int, std::unique_ptr<int>> owner{aw::in_place_index<1>(), new int{5}};
    AW_CHECK(*aw::get<1>(owner) == 5);
}

} // namespace

int main() {
    optionalBasics();
    optionalMoveOnly();
    variantBasics();
    return 0;
}
```

`tests/unit/error_test.cpp`：

```cpp
#include <exception>
#include <stdexcept>
#include <string>
#include <system_error>

#include "awaitable/error.hpp"
#include "awaitable/multiple_exceptions.hpp"
#include "check.hpp"

int main() {
    std::error_code const ec = aw::error::operation_aborted;
    AW_CHECK(ec.value() == 1);
    AW_CHECK(std::string{ec.category().name()} == "aw");
    AW_CHECK(ec == aw::error::make_error_code(aw::error::operation_aborted));
    AW_CHECK(!ec.message().empty());

    auto const first = std::make_exception_ptr(std::runtime_error{"first"});
    aw::multiple_exceptions const all{first};
    AW_CHECK(all.first_exception() == first);
    AW_CHECK(std::string{all.what()} == "multiple exceptions");
    return 0;
}
```

- [ ] **Step 2：运行，确认失败**

Run: `cmake --build build/debug --target aw-optional-variant-test aw-error-test 2>&1 | grep -m2 "fatal error"`
Expected：出现 `fatal error: 'awaitable/optional.hpp' file not found` 和 `fatal error: 'awaitable/error.hpp' file not found`。如果没有执行重新配置，先运行 `cmake --preset debug`。

- [ ] **Step 3：最小实现**

`include/awaitable/optional.hpp`：

```cpp
#pragma once

#include "awaitable/core/config.hpp"

#if CO2_CPLUSPLUS >= 201703L
#include <optional>
namespace aw {
using std::nullopt;
using std::nullopt_t;
using std::optional;
} // namespace aw
#else
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace aw {

struct nullopt_t {
    explicit constexpr nullopt_t(int) noexcept {}
};
constexpr nullopt_t nullopt{0};

struct bad_optional_access : std::logic_error {
    bad_optional_access() : std::logic_error{"bad optional access"} {}
};

template <class T> class optional {
    static_assert(std::is_object<T>::value && !std::is_array<T>::value,
                  "aw::optional<T> requires a non-array object type");

  public:
    using value_type = T;

    optional() noexcept {}
    optional(nullopt_t) noexcept {}
    optional(T const& value) { construct(value); }
    optional(T&& value) { construct(std::move(value)); }
    optional(optional const& other) {
        if (other.engaged_) construct(*other);
    }
    optional(optional&& other) noexcept(std::is_nothrow_move_constructible<T>::value) {
        if (other.engaged_) construct(std::move(*other));
    }
    ~optional() { reset(); }

    optional& operator=(nullopt_t) noexcept {
        reset();
        return *this;
    }
    optional& operator=(optional const& other) {
        if (this != &other) {
            reset();
            if (other.engaged_) construct(*other);
        }
        return *this;
    }
    optional& operator=(optional&& other) noexcept(std::is_nothrow_move_constructible<T>::value) {
        if (this != &other) {
            reset();
            if (other.engaged_) construct(std::move(*other));
        }
        return *this;
    }

    bool has_value() const noexcept { return engaged_; }
    explicit operator bool() const noexcept { return engaged_; }

    T& operator*() & noexcept { return *ptr(); }
    T const& operator*() const& noexcept { return *ptr(); }
    T&& operator*() && noexcept { return std::move(*ptr()); }
    T* operator->() noexcept { return ptr(); }
    T const* operator->() const noexcept { return ptr(); }

    T& value() & {
        if (!engaged_) throw bad_optional_access{};
        return *ptr();
    }
    T const& value() const& {
        if (!engaged_) throw bad_optional_access{};
        return *ptr();
    }
    T&& value() && {
        if (!engaged_) throw bad_optional_access{};
        return std::move(*ptr());
    }

    template <class... Args> T& emplace(Args&&... args) {
        reset();
        construct(std::forward<Args>(args)...);
        return *ptr();
    }

    void reset() noexcept {
        if (engaged_) {
            ptr()->~T();
            engaged_ = false;
        }
    }

  private:
    template <class... Args> void construct(Args&&... args) {
        ::new (static_cast<void*>(storage_)) T(std::forward<Args>(args)...);
        engaged_ = true;
    }
    T* ptr() noexcept { return reinterpret_cast<T*>(storage_); }
    T const* ptr() const noexcept { return reinterpret_cast<T const*>(storage_); }

    alignas(T) unsigned char storage_[sizeof(T)];
    bool engaged_{false};
};

} // namespace aw
#endif
```

`include/awaitable/variant.hpp`：

```cpp
#pragma once

#include <cstddef>

#include "awaitable/core/config.hpp"

#if CO2_CPLUSPLUS >= 201703L
#include <variant>
namespace aw {
using std::get;
using std::get_if;
using std::in_place_index_t;
using std::monostate;
using std::variant;
template <std::size_t I> constexpr std::in_place_index_t<I> in_place_index() noexcept {
    return std::in_place_index_t<I>{};
}
} // namespace aw
#else
#include <algorithm>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace aw {

struct monostate {};

template <std::size_t I> struct in_place_index_t {
    explicit constexpr in_place_index_t() noexcept = default;
};
template <std::size_t I> constexpr in_place_index_t<I> in_place_index() noexcept {
    return in_place_index_t<I>{};
}

struct bad_variant_access : std::logic_error {
    bad_variant_access() : std::logic_error{"bad variant access"} {}
};

namespace detail {

template <std::size_t I, class... Ts> struct nth_type;
template <class T, class... Ts> struct nth_type<0, T, Ts...> { using type = T; };
template <std::size_t I, class T, class... Ts> struct nth_type<I, T, Ts...> {
    using type = typename nth_type<I - 1, Ts...>::type;
};

template <std::size_t A, std::size_t... Rest> struct max_of {
    static constexpr std::size_t value = A > max_of<Rest...>::value ? A : max_of<Rest...>::value;
};
template <std::size_t A> struct max_of<A> { static constexpr std::size_t value = A; };

template <class... Ts> struct variant_ops;
template <> struct variant_ops<> {
    static void destroy(std::size_t, void*) noexcept {}
    static void move(std::size_t, void*, void*) {}
};
template <class T, class... Ts> struct variant_ops<T, Ts...> {
    static void destroy(std::size_t index, void* p) noexcept {
        if (index == 0U) {
            static_cast<T*>(p)->~T();
        } else {
            variant_ops<Ts...>::destroy(index - 1U, p);
        }
    }
    static void move(std::size_t index, void* to, void* from) {
        if (index == 0U) {
            ::new (to) T(std::move(*static_cast<T*>(from)));
        } else {
            variant_ops<Ts...>::move(index - 1U, to, from);
        }
    }
};

} // namespace detail

template <class... Ts> class variant {
    static_assert(sizeof...(Ts) > 0U, "aw::variant needs at least one alternative");
    using first_type = typename detail::nth_type<0, Ts...>::type;

  public:
    variant() : index_{0U} { ::new (static_cast<void*>(storage_)) first_type(); }

    template <std::size_t I, class... Args>
    explicit variant(in_place_index_t<I>, Args&&... args) : index_{I} {
        using alternative = typename detail::nth_type<I, Ts...>::type;
        ::new (static_cast<void*>(storage_)) alternative(std::forward<Args>(args)...);
    }

    variant(variant&& other) : index_{other.index_} {
        detail::variant_ops<Ts...>::move(index_, storage_, other.storage_);
    }
    variant(variant const&) = delete;
    variant& operator=(variant const&) = delete;
    variant& operator=(variant&& other) {
        if (this != &other) {
            detail::variant_ops<Ts...>::destroy(index_, storage_);
            index_ = other.index_;
            detail::variant_ops<Ts...>::move(index_, storage_, other.storage_);
        }
        return *this;
    }
    ~variant() { detail::variant_ops<Ts...>::destroy(index_, storage_); }

    std::size_t index() const noexcept { return index_; }

    template <std::size_t I> typename detail::nth_type<I, Ts...>::type* get_ptr() noexcept {
        using alternative = typename detail::nth_type<I, Ts...>::type;
        return index_ == I ? reinterpret_cast<alternative*>(storage_) : nullptr;
    }

  private:
    alignas(detail::max_of<alignof(Ts)...>::value) unsigned char
        storage_[detail::max_of<sizeof(Ts)...>::value];
    std::size_t index_;
};

template <std::size_t I, class... Ts>
typename detail::nth_type<I, Ts...>::type& get(variant<Ts...>& v) {
    auto* p = v.template get_ptr<I>();
    if (p == nullptr) throw bad_variant_access{};
    return *p;
}

template <std::size_t I, class... Ts>
typename detail::nth_type<I, Ts...>::type&& get(variant<Ts...>&& v) {
    return std::move(get<I>(v));
}

template <std::size_t I, class... Ts>
typename detail::nth_type<I, Ts...>::type* get_if(variant<Ts...>* v) noexcept {
    return v == nullptr ? nullptr : v->template get_ptr<I>();
}

} // namespace aw
#endif
```

`include/awaitable/error.hpp`：

```cpp
#pragma once

#include <string>
#include <system_error>
#include <type_traits>

namespace aw {
namespace error {

enum errc : int { operation_aborted = 1 };

namespace detail {
class category_impl final : public std::error_category {
  public:
    char const* name() const noexcept override { return "aw"; }
    std::string message(int value) const override {
        return value == operation_aborted ? "operation aborted" : "unknown aw error";
    }
};
} // namespace detail

inline std::error_category const& category() noexcept {
    static detail::category_impl const instance;
    return instance;
}

inline std::error_code make_error_code(errc value) noexcept {
    return std::error_code{static_cast<int>(value), category()};
}

} // namespace error
} // namespace aw

namespace std {
template <> struct is_error_code_enum<aw::error::errc> : true_type {};
} // namespace std
```

`include/awaitable/multiple_exceptions.hpp`：

```cpp
#pragma once

#include <exception>
#include <utility>

namespace aw {

class multiple_exceptions : public std::exception {
  public:
    explicit multiple_exceptions(std::exception_ptr first) noexcept : first_{std::move(first)} {}
    std::exception_ptr first_exception() const { return first_; }
    char const* what() const noexcept override { return "multiple exceptions"; }

  private:
    std::exception_ptr first_;
};

} // namespace aw
```

- [ ] **Step 4：运行，确认通过（含 C++17 / C++20 别名路径）**

Run: `cmake --build build/debug -j8 && ctest --test-dir build/debug -R '^aw-' --output-on-failure && tools/run-matrix.sh debug-cxx17 debug-cxx20`
Expected：`100% tests passed out of 2`，然后输出 `debug-cxx17: 100% tests passed out of 29` 和 `debug-cxx20: 100% tests passed out of 29`。

- [ ] **Step 5：提交**

```bash
git add tests/AwTesting.cmake tests/CMakeLists.txt tests/support/check.hpp tests/unit \
        include/awaitable/optional.hpp include/awaitable/variant.hpp include/awaitable/error.hpp \
        include/awaitable/multiple_exceptions.hpp
git commit -m "feat: test infrastructure, aw::optional/variant, error category, multiple_exceptions"
```

### Task 3：回收缓存与 thread_context

**交付物：** 照搬 asio `thread_info_base` 的按标签回收缓存，由 `thread_context` 调用栈承载；附带“启动中 / 正在发起 / 正在展开”三个线程局部标记，供 Task 10、11、15 使用。分配次数用计数分配器断言。

**Files:**
- Create: `include/awaitable/exec/detail/thread_context.hpp`, `include/awaitable/exec/detail/recycling_allocator.hpp`
- Create: `tests/support/counting_new.hpp`
- Test: `tests/unit/recycling_allocator_test.cpp`, `tests/unit/recycling_disabled_test.cpp`
- Modify: `tests/unit/CMakeLists.txt`（追加）

**Interfaces:**
- Consumes：Task 2 的 `aw_unit_test`、`AW_CHECK`。
- Produces：
  - 枚举 `aw::detail::mem_tag { default_tag, awaitable_frame, executor_function, cancellation_signal, parallel_group }` 和常量 `aw::detail::mem_tag_count = 5`。
  - `class aw::detail::thread_info`，提供：
    - `void* allocate(mem_tag, std::size_t size, std::size_t align)`；
    - `void deallocate(mem_tag, void*, std::size_t size) noexcept`；
    - `static constexpr std::size_t chunk_size = 4`；
    - `static constexpr std::size_t cache_size = AW_RECYCLING_ALLOCATOR_CACHE_SIZE`。
  - `class aw::detail::thread_context`（不可复制，构造即入栈、析构即出栈；嵌套时共享最外层的 `thread_info`），提供：
    - `static thread_info* current_info() noexcept`；
    - `static bool in_library_thread() noexcept`。
  - 线程局部标记：
    - `aw::detail::launching_marker()` / `struct launching_scope { explicit launching_scope(void const* key) noexcept; }`；
    - `aw::detail::initiating_marker()` / `struct initiating_scope`；
    - `aw::detail::unwinding()` / `struct unwinding_scope`。
  - 分配函数：
    - `void* aw::detail::recycling_allocate(mem_tag, std::size_t size, std::size_t align = alignof(std::max_align_t))`；
    - `void aw::detail::recycling_deallocate(mem_tag, void*, std::size_t size) noexcept`。

    `size` 必须与分配时相同；前置条件是 `align <= alignof(std::max_align_t)`。
  - 宏：`AW_RECYCLING_ALLOCATOR_CACHE_SIZE`（默认 2）、`AW_DISABLE_RECYCLING_ALLOCATOR`。
  - 测试支持 `tests/support/counting_new.hpp`：
    - 计数分配器：`aw_test::counting_new_enabled`（TSan 下为 false）、`aw_test::allocation_counter{}.news()` / `.deletes()`；
    - 每个测试可执行文件只能有一个 TU 包含它。

- [ ] **Step 1：写失败的测试**

`tests/support/counting_new.hpp`：

```cpp
#pragma once

// 替换全局 operator new / delete 并计数。每个测试可执行文件只允许一个翻译单元包含本文件。
// TSan 运行时自己定义了全局 operator new（与 Task 1 的 issue03 测试同一原因），TSan 构建下
// 不替换、不计数：counting_new_enabled 为 false，测试只做行为检查。

#include <atomic>
#include <cstdlib>
#include <new>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define AW_TEST_NO_COUNTING_NEW 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define AW_TEST_NO_COUNTING_NEW 1
#endif

namespace aw_test {

inline std::atomic<long>& new_calls() noexcept {
    static std::atomic<long> calls{0};
    return calls;
}

inline std::atomic<long>& delete_calls() noexcept {
    static std::atomic<long> calls{0};
    return calls;
}

#if defined(AW_TEST_NO_COUNTING_NEW)
constexpr bool counting_new_enabled = false;
#else
constexpr bool counting_new_enabled = true;
#endif

class allocation_counter {
  public:
    allocation_counter() noexcept
        : news_at_start_{new_calls().load()}, deletes_at_start_{delete_calls().load()} {}
    long news() const noexcept { return new_calls().load() - news_at_start_; }
    long deletes() const noexcept { return delete_calls().load() - deletes_at_start_; }

  private:
    long news_at_start_;
    long deletes_at_start_;
};

} // namespace aw_test

#if !defined(AW_TEST_NO_COUNTING_NEW)
void* operator new(std::size_t size) {
    aw_test::new_calls().fetch_add(1, std::memory_order_relaxed);
    if (void* const p = std::malloc(size == 0U ? 1U : size)) return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept {
    if (p != nullptr) aw_test::delete_calls().fetch_add(1, std::memory_order_relaxed);
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
#endif
```

`tests/unit/recycling_allocator_test.cpp`：

```cpp
#include "awaitable/exec/detail/recycling_allocator.hpp"
#include "awaitable/exec/detail/thread_context.hpp"
#include "check.hpp"
#include "counting_new.hpp"

using aw::detail::mem_tag;
using aw::detail::recycling_allocate;
using aw::detail::recycling_deallocate;

namespace {

void outsideLibraryThreadAlwaysAllocates() {
    AW_CHECK(!aw::detail::thread_context::in_library_thread());
    aw_test::allocation_counter counter;
    for (int i = 0; i != 3; ++i) {
        void* p = recycling_allocate(mem_tag::awaitable_frame, 100U);
        recycling_deallocate(mem_tag::awaitable_frame, p, 100U);
    }
    if (aw_test::counting_new_enabled) AW_CHECK(counter.news() == 3 && counter.deletes() == 3);
}

void reuseWithinTag() {
    aw::detail::thread_context context;
    AW_CHECK(aw::detail::thread_context::in_library_thread());
    void* first = recycling_allocate(mem_tag::awaitable_frame, 100U);
    recycling_deallocate(mem_tag::awaitable_frame, first, 100U);
    aw_test::allocation_counter counter;
    void* again = recycling_allocate(mem_tag::awaitable_frame, 100U);
    AW_CHECK(again == first);
    recycling_deallocate(mem_tag::awaitable_frame, again, 100U);
    void* smaller = recycling_allocate(mem_tag::awaitable_frame, 64U);  // 25 块够 16 块用
    AW_CHECK(smaller == first);
    recycling_deallocate(mem_tag::awaitable_frame, smaller, 64U);
    if (aw_test::counting_new_enabled) AW_CHECK(counter.news() == 0);
}

void tagsAreSeparate() {
    aw::detail::thread_context context;
    void* p = recycling_allocate(mem_tag::executor_function, 48U);
    recycling_deallocate(mem_tag::executor_function, p, 48U);
    aw_test::allocation_counter counter;
    void* q = recycling_allocate(mem_tag::cancellation_signal, 48U);
    recycling_deallocate(mem_tag::cancellation_signal, q, 48U);
    if (aw_test::counting_new_enabled) AW_CHECK(counter.news() == 1);
}

void cacheHoldsTwoBlocksPerTag() {
    aw::detail::thread_context context;
    void* a = recycling_allocate(mem_tag::default_tag, 32U);
    void* b = recycling_allocate(mem_tag::default_tag, 32U);
    void* c = recycling_allocate(mem_tag::default_tag, 32U);
    aw_test::allocation_counter counter;
    recycling_deallocate(mem_tag::default_tag, a, 32U);
    recycling_deallocate(mem_tag::default_tag, b, 32U);
    recycling_deallocate(mem_tag::default_tag, c, 32U);  // 第三块放不下，直接 delete
    if (aw_test::counting_new_enabled) AW_CHECK(counter.deletes() == 1);
}

void hugeRequestsBypassTheCache() {
    aw::detail::thread_context context;
    aw_test::allocation_counter counter;
    for (int i = 0; i != 2; ++i) {
        void* p = recycling_allocate(mem_tag::awaitable_frame, 4U * 300U);  // 300 块 > 255
        recycling_deallocate(mem_tag::awaitable_frame, p, 4U * 300U);
    }
    if (aw_test::counting_new_enabled) AW_CHECK(counter.news() == 2 && counter.deletes() == 2);
}

void nestedContextsShareTheOuterCache() {
    aw::detail::thread_context outer;
    void* p = nullptr;
    {
        aw::detail::thread_context inner;
        p = recycling_allocate(mem_tag::awaitable_frame, 80U);
        recycling_deallocate(mem_tag::awaitable_frame, p, 80U);
    }
    aw_test::allocation_counter counter;
    void* q = recycling_allocate(mem_tag::awaitable_frame, 80U);
    AW_CHECK(q == p);
    recycling_deallocate(mem_tag::awaitable_frame, q, 80U);
    if (aw_test::counting_new_enabled) AW_CHECK(counter.news() == 0);
}

void contextExitFreesCachedBlocks() {
    aw_test::allocation_counter counter;
    {
        aw::detail::thread_context context;
        void* p = recycling_allocate(mem_tag::awaitable_frame, 80U);
        recycling_deallocate(mem_tag::awaitable_frame, p, 80U);
    }
    AW_CHECK(!aw::detail::thread_context::in_library_thread());
    if (aw_test::counting_new_enabled) AW_CHECK(counter.news() == 1 && counter.deletes() == 1);
}

void blockFreedOnOtherThreadKindStillValid() {
    void* p = recycling_allocate(mem_tag::awaitable_frame, 40U);  // 非库线程分配
    aw::detail::thread_context context;
    recycling_deallocate(mem_tag::awaitable_frame, p, 40U);       // 库线程释放：进缓存
    void* q = recycling_allocate(mem_tag::awaitable_frame, 40U);
    AW_CHECK(q == p);
    recycling_deallocate(mem_tag::awaitable_frame, q, 40U);
}

void markers() {
    int key = 0;
    AW_CHECK(aw::detail::launching_marker() == nullptr);
    {
        aw::detail::launching_scope outer{&key};
        AW_CHECK(aw::detail::launching_marker() == &key);
        {
            aw::detail::launching_scope inner{nullptr};
            AW_CHECK(aw::detail::launching_marker() == nullptr);
        }
        AW_CHECK(aw::detail::launching_marker() == &key);
    }
    {
        aw::detail::initiating_scope scope{&key};
        AW_CHECK(aw::detail::initiating_marker() == &key);
    }
    AW_CHECK(aw::detail::initiating_marker() == nullptr);
    AW_CHECK(!aw::detail::unwinding());
    {
        aw::detail::unwinding_scope scope;
        AW_CHECK(aw::detail::unwinding());
    }
    AW_CHECK(!aw::detail::unwinding());
}

} // namespace

int main() {
    outsideLibraryThreadAlwaysAllocates();
    reuseWithinTag();
    tagsAreSeparate();
    cacheHoldsTwoBlocksPerTag();
    hugeRequestsBypassTheCache();
    nestedContextsShareTheOuterCache();
    contextExitFreesCachedBlocks();
    blockFreedOnOtherThreadKindStillValid();
    markers();
    return 0;
}
```

`tests/unit/recycling_disabled_test.cpp`：

```cpp
#define AW_DISABLE_RECYCLING_ALLOCATOR 1
#include "awaitable/exec/detail/recycling_allocator.hpp"
#include "awaitable/exec/detail/thread_context.hpp"
#include "check.hpp"
#include "counting_new.hpp"

int main() {
    aw::detail::thread_context context;
    aw_test::allocation_counter counter;
    for (int i = 0; i != 4; ++i) {
        void* p = aw::detail::recycling_allocate(aw::detail::mem_tag::awaitable_frame, 100U);
        aw::detail::recycling_deallocate(aw::detail::mem_tag::awaitable_frame, p, 100U);
    }
    if (aw_test::counting_new_enabled) AW_CHECK(counter.news() == 4 && counter.deletes() == 4);
    return 0;
}
```

`tests/unit/CMakeLists.txt` 追加：

```cmake
aw_unit_test(recycling_allocator_test)
aw_unit_test(recycling_disabled_test)
```

- [ ] **Step 2：运行，确认失败**

Run: `cmake --build build/debug --target aw-recycling-allocator-test 2>&1 | grep -m1 "fatal error"`
Expected: `fatal error: 'awaitable/exec/detail/recycling_allocator.hpp' file not found`

- [ ] **Step 3：最小实现**

`include/awaitable/exec/detail/thread_context.hpp`：

```cpp
#pragma once

#include <climits>
#include <cstddef>
#include <new>

#include "awaitable/core/contract.hpp"

#ifndef AW_RECYCLING_ALLOCATOR_CACHE_SIZE
#define AW_RECYCLING_ALLOCATOR_CACHE_SIZE 2
#endif

namespace aw {
namespace detail {

// 照搬 asio thread_info_base：每个标签若干格缓存；以 4 字节为一块，分配时多要 1 字节，把
// 块数写在 mem[size]，归还时搬到 mem[0]；块数超过 255 的请求不进缓存。
enum class mem_tag : unsigned {
    default_tag = 0,
    awaitable_frame,
    executor_function,
    cancellation_signal,
    parallel_group
};
constexpr std::size_t mem_tag_count = 5U;

class thread_info {
  public:
    static constexpr std::size_t chunk_size = 4U;
    static constexpr std::size_t cache_size = AW_RECYCLING_ALLOCATOR_CACHE_SIZE;

    thread_info() noexcept {
        for (auto& row : reusable_)
            for (auto& cell : row) cell = nullptr;
    }
    thread_info(thread_info const&) = delete;
    thread_info& operator=(thread_info const&) = delete;
    ~thread_info() {
        for (auto& row : reusable_)
            for (auto& cell : row) ::operator delete(cell);
    }

    static void* allocate_block(std::size_t size) {
        std::size_t const chunks = (size + chunk_size - 1U) / chunk_size;
        void* const pointer = ::operator new(chunks * chunk_size + 1U);
        auto* const mem = static_cast<unsigned char*>(pointer);
        mem[size] = (chunks <= UCHAR_MAX) ? static_cast<unsigned char>(chunks) : 0U;
        return pointer;
    }

    void* allocate(mem_tag tag, std::size_t size, std::size_t align) {
        std::size_t const chunks = (size + chunk_size - 1U) / chunk_size;
        auto& row = reusable_[static_cast<std::size_t>(tag)];
        for (auto& cell : row) {
            if (cell == nullptr) continue;
            auto* const mem = static_cast<unsigned char*>(cell);
            if (static_cast<std::size_t>(mem[0]) >= chunks &&
                reinterpret_cast<std::size_t>(cell) % align == 0U) {
                void* const pointer = cell;
                cell = nullptr;
                mem[size] = mem[0];
                return pointer;
            }
        }
        // 没有合适的块：先放掉一块缓存，避免缓存里囤着不合用的块。
        for (auto& cell : row) {
            if (cell != nullptr) {
                ::operator delete(cell);
                cell = nullptr;
                break;
            }
        }
        return allocate_block(size);
    }

    void deallocate(mem_tag tag, void* pointer, std::size_t size) noexcept {
        if (size <= chunk_size * UCHAR_MAX) {
            auto& row = reusable_[static_cast<std::size_t>(tag)];
            for (auto& cell : row) {
                if (cell == nullptr) {
                    auto* const mem = static_cast<unsigned char*>(pointer);
                    mem[0] = mem[size];
                    cell = pointer;
                    return;
                }
            }
        }
        ::operator delete(pointer);
    }

  private:
    void* reusable_[mem_tag_count][cache_size];
};

// 库线程的调用栈：thread_pool 的工作线程入口、manual_executor::run / run_one / poll 期间。
// 嵌套时共享最外层的 thread_info；最外层出栈时释放缓存。
class thread_context {
  public:
    thread_context() noexcept : next_{top()}, info_{next_ != nullptr ? next_->info_ : &own_} {
        top() = this;
    }
    thread_context(thread_context const&) = delete;
    thread_context& operator=(thread_context const&) = delete;
    ~thread_context() { top() = next_; }

    static thread_info* current_info() noexcept {
        thread_context* const t = top();
        return t != nullptr ? t->info_ : nullptr;
    }
    static bool in_library_thread() noexcept { return top() != nullptr; }

  private:
    static thread_context*& top() noexcept {
        static thread_local thread_context* current = nullptr;
        return current;
    }

    thread_context* next_;
    thread_info own_;
    thread_info* info_;
};

// ---- 线程局部标记 ----

// “启动中”：awaitable_thread::launch 的调用栈内，值为入口帧地址（§7.3 第 4 条）。
inline void const*& launching_marker() noexcept {
    static thread_local void const* marker = nullptr;
    return marker;
}
struct launching_scope {
    explicit launching_scope(void const* key) noexcept : previous_{launching_marker()} {
        launching_marker() = key;
    }
    launching_scope(launching_scope const&) = delete;
    launching_scope& operator=(launching_scope const&) = delete;
    ~launching_scope() { launching_marker() = previous_; }
    void const* previous_;
};

// “正在发起”：async_op_awaiter 调用 initiation 期间，值为 awaiter 地址（§8.3 同步完成）。
inline void const*& initiating_marker() noexcept {
    static thread_local void const* marker = nullptr;
    return marker;
}
struct initiating_scope {
    explicit initiating_scope(void const* key) noexcept : previous_{initiating_marker()} {
        initiating_marker() = key;
    }
    initiating_scope(initiating_scope const&) = delete;
    initiating_scope& operator=(initiating_scope const&) = delete;
    ~initiating_scope() { initiating_marker() = previous_; }
    void const* previous_;
};

// “正在展开被丢弃的执行线程”（§11）：此时销毁仍在帧栈里的生成器不算契约违规。
inline unsigned& unwinding_depth() noexcept {
    static thread_local unsigned depth = 0U;
    return depth;
}
inline bool unwinding() noexcept { return unwinding_depth() != 0U; }
struct unwinding_scope {
    unwinding_scope() noexcept { ++unwinding_depth(); }
    unwinding_scope(unwinding_scope const&) = delete;
    unwinding_scope& operator=(unwinding_scope const&) = delete;
    ~unwinding_scope() { --unwinding_depth(); }
};

} // namespace detail
} // namespace aw
```

`include/awaitable/exec/detail/recycling_allocator.hpp`：

```cpp
#pragma once

#include <cstddef>
#include <new>

#include "awaitable/core/contract.hpp"
#include "awaitable/exec/detail/thread_context.hpp"

namespace aw {
namespace detail {

// size 必须与分配时一致。只支持不超过 max_align_t 的对齐（C++14 没有对齐版 new）。
inline void* recycling_allocate(mem_tag tag, std::size_t size,
                                std::size_t align = alignof(std::max_align_t)) {
    CO2_CONTRACT_CHECK(align <= alignof(std::max_align_t));
#if defined(AW_DISABLE_RECYCLING_ALLOCATOR)
    static_cast<void>(tag);
    return ::operator new(size);
#else
    if (thread_info* const info = thread_context::current_info())
        return info->allocate(tag, size, align);
    return thread_info::allocate_block(size);
#endif
}

inline void recycling_deallocate(mem_tag tag, void* pointer, std::size_t size) noexcept {
#if defined(AW_DISABLE_RECYCLING_ALLOCATOR)
    static_cast<void>(tag);
    static_cast<void>(size);
    ::operator delete(pointer);
#else
    if (thread_info* const info = thread_context::current_info()) {
        info->deallocate(tag, pointer, size);
        return;
    }
    ::operator delete(pointer);
#endif
}

} // namespace detail
} // namespace aw
```

- [ ] **Step 4：运行，确认通过**

Run: `cmake --build build/debug -j8 && ctest --test-dir build/debug -R '^aw-recycling' --output-on-failure`
Expected: `100% tests passed out of 2`

- [ ] **Step 5：提交**

```bash
git add include/awaitable/exec/detail tests/support/counting_new.hpp tests/unit
git commit -m "feat(exec): recycling allocator (asio thread_info_base) and thread_context markers"
```

### Task 4：exec 基础：operation 队列项、scheduler、basic_executor、manual_executor、schedule_on

**交付物：** §5.1–§5.3、§5.5 的 executor 模型，以及 `manual_executor`。具体包括：带标记的队列项、`post` / `dispatch` / `schedule` / `running_in_this_thread`、`run` / `run_one` / `poll`、异常传给调用者、关闭时销毁 operation、带协程句柄项析构属于契约违规。

**Files:**
- Create: `include/awaitable/exec/detail/operation.hpp`, `include/awaitable/exec/scheduler.hpp`, `include/awaitable/exec/executor.hpp`, `include/awaitable/exec/manual_executor.hpp`
- Create: `tests/support/test_coro.hpp`
- Test: `tests/unit/manual_executor_test.cpp`, `tests/unit/manual_executor_destroy_with_handle_contract_test.cpp`
- Modify: `tests/unit/CMakeLists.txt`（追加）

**Interfaces:**
- Consumes（Task 3）：`recycling_allocate` / `recycling_deallocate`、`mem_tag::executor_function`、`thread_context`。
- Produces：
  - `struct aw::detail::operation`：
    - `using complete_fn = void (*)(operation*, bool invoke)`；
    - 成员 `complete_fn complete_`、`operation* next_`；
    - 方法 `void complete()`（执行并释放）、`void destroy() noexcept`（只释放）。
  - `template <class F> aw::detail::operation* aw::detail::make_function_operation(F&& f)`。内存来自 `executor_function` 标签；执行前先把 `f` 移到栈上并释放块。
  - 带标记的队列项：`void* tag_handle(co2::coroutine_handle<>)`、`void* tag_operation(operation*)`、`bool is_operation(void*)`、`void run_item(void*)`、`void destroy_item(void*) noexcept`（遇到句柄项就是契约违规）。
  - `class aw::scheduler`，三个纯虚函数：
    - `schedule(co2::coroutine_handle<>) noexcept`；
    - `post(detail::operation*) noexcept`；
    - `running_in_this_thread() const noexcept`。
  - `struct aw::schedule_on_awaiter { scheduler* target; }` 和 `aw::schedule_on(scheduler&)`。
  - `template <class Context> class aw::basic_executor`：
    - 构造：默认构造（无效句柄）、`explicit basic_executor(Context&)`；
    - 方法：`context()`、`post(F&&)`、`dispatch(F&&)`、`running_in_this_thread()`、`schedule(h)`；
    - 比较：`==` / `!=`。
  - trait `aw::is_executor<T>`：有 `context()` 和 `running_in_this_thread()` 时为真。
  - `class aw::manual_executor final : public scheduler`：
    - `using executor_type = basic_executor<manual_executor>`；
    - `executor_type get_executor() noexcept`；
    - `std::size_t run()`、`bool run_one()`、`std::size_t poll()`、`std::size_t pending() const noexcept`。
  - 测试支持 `aw_test::eager_coro`：最小 core 级协程，不在初始点挂起，结束时自毁。

- [ ] **Step 1：写失败的测试**

`tests/support/test_coro.hpp`：

```cpp
#pragma once

#include <exception>

#include "awaitable/core/coroutine.hpp"
#include "awaitable/core/dsl.hpp"

namespace aw_test {

// 最小的 core 级协程：立即开始，结束时帧自毁（final_suspend 不挂起）。只用来测 schedule。
struct eager_coro {
    struct promise_type {
        eager_coro get_return_object() noexcept { return {}; }
        co2::suspend_never initial_suspend() const noexcept { return {}; }
        co2::suspend_never final_suspend() const noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };
};

} // namespace aw_test
```

`tests/unit/manual_executor_test.cpp`：

```cpp
#include <stdexcept>
#include <string>
#include <vector>

#include "awaitable/exec/manual_executor.hpp"
#include "check.hpp"
#include "counting_new.hpp"
#include "test_coro.hpp"

namespace {

auto hop(aw::scheduler& target, std::vector<std::string>& trace)
    CO2_BEG(aw_test::eager_coro, (target, trace)) {
    trace.push_back("before");
    CO2_AWAIT(aw::schedule_on(target));
    trace.push_back("after");
    CO2_RETURN();
}
CO2_END

void postRunsLaterInFifoOrder() {
    aw::manual_executor ctx;
    auto ex = ctx.get_executor();
    std::vector<int> order;
    ex.post([&] { order.push_back(1); });
    ex.post([&] { order.push_back(2); });
    AW_CHECK(order.empty() && ctx.pending() == 2U);
    AW_CHECK(ctx.run() == 2U);
    AW_CHECK((order == std::vector<int>{1, 2}));
    AW_CHECK(!ctx.run_one());
}

void dispatchIsInlineOnlyInsideRun() {
    aw::manual_executor ctx;
    auto ex = ctx.get_executor();
    std::vector<std::string> trace;
    ex.dispatch([&] { trace.push_back("outside"); });
    AW_CHECK(trace.empty());
    ex.post([&] {
        AW_CHECK(ex.running_in_this_thread());
        ex.dispatch([&] { trace.push_back("inline"); });
        trace.push_back("after-dispatch");
    });
    AW_CHECK(!ex.running_in_this_thread());
    ctx.run();
    AW_CHECK((trace == std::vector<std::string>{"outside", "inline", "after-dispatch"}));
}

void pollRunsOnlyReadyItems() {
    aw::manual_executor ctx;
    auto ex = ctx.get_executor();
    int ran = 0;
    ex.post([&] {
        ++ran;
        ex.post([&] { ++ran; });
    });
    AW_CHECK(ctx.poll() == 1U && ran == 1);
    AW_CHECK(ctx.poll() == 1U && ran == 2);
}

void exceptionsPropagateToTheCaller() {
    aw::manual_executor ctx;
    auto ex = ctx.get_executor();
    int ran = 0;
    ex.post([] { throw std::runtime_error{"boom"}; });
    ex.post([&] { ++ran; });
    bool caught = false;
    try {
        ctx.run();
    } catch (std::runtime_error const& e) {
        caught = std::string{e.what()} == "boom";
    }
    AW_CHECK(caught && ran == 0 && ctx.pending() == 1U);
    AW_CHECK(!ex.running_in_this_thread());
    ctx.run();
    AW_CHECK(ran == 1);
}

void scheduleResumesCoreCoroutines() {
    aw::manual_executor ctx;
    std::vector<std::string> trace;
    hop(ctx, trace);
    AW_CHECK((trace == std::vector<std::string>{"before"}));
    ctx.run();
    AW_CHECK((trace == std::vector<std::string>{"before", "after"}));
}

void shutdownDestroysQueuedOperations() {
    int destroyed = 0;
    struct counted {
        int* destroyed;
        bool moved{false};
        counted(int* d) : destroyed{d} {}
        counted(counted&& other) noexcept : destroyed{other.destroyed} { other.moved = true; }
        ~counted() {
            if (!moved) ++*destroyed;
        }
        void operator()() const { AW_CHECK(false); }
    };
    {
        aw::manual_executor ctx;
        ctx.get_executor().post(counted{&destroyed});
    }
    AW_CHECK(destroyed == 1);
}

void operationsReuseRecycledMemoryInsideRun() {
    aw::manual_executor ctx;
    auto ex = ctx.get_executor();
    int remaining = 1000;
    long news_in_steady_state = -1;
    aw_test::allocation_counter* counter = nullptr;
    struct step {
        aw::manual_executor::executor_type ex;
        int* remaining;
        long* news;
        aw_test::allocation_counter** counter;
        void operator()() {
            if (*remaining == 900) *counter = new aw_test::allocation_counter{};
            if (--*remaining == 0) {
                *news = (*counter)->news();
                delete *counter;
                return;
            }
            ex.post(step{ex, remaining, news, counter});
        }
    };
    ex.post(step{ex, &remaining, &news_in_steady_state, &counter});
    ctx.run();
    AW_CHECK(remaining == 0);
    if (aw_test::counting_new_enabled) AW_CHECK(news_in_steady_state == 0);
}

void equality() {
    aw::manual_executor a;
    aw::manual_executor b;
    AW_CHECK(a.get_executor() == a.get_executor());
    AW_CHECK(a.get_executor() != b.get_executor());
    AW_CHECK(&a.get_executor().context() == &a);
    static_assert(aw::is_executor<aw::manual_executor::executor_type>::value, "executor");
    static_assert(!aw::is_executor<aw::manual_executor>::value, "context is not an executor");
}

} // namespace

int main() {
    postRunsLaterInFifoOrder();
    dispatchIsInlineOnlyInsideRun();
    pollRunsOnlyReadyItems();
    exceptionsPropagateToTheCaller();
    scheduleResumesCoreCoroutines();
    shutdownDestroysQueuedOperations();
    operationsReuseRecycledMemoryInsideRun();
    equality();
    return 0;
}
```

`tests/unit/manual_executor_destroy_with_handle_contract_test.cpp`：

```cpp
#include <cstdlib>
#include <string>
#include <vector>

#include "awaitable/exec/manual_executor.hpp"
#include "test_coro.hpp"

namespace {
auto hop(aw::scheduler& target, std::vector<std::string>& trace)
    CO2_BEG(aw_test::eager_coro, (target, trace)) {
    CO2_AWAIT(aw::schedule_on(target));
    trace.push_back("unreachable");
    CO2_RETURN();
}
CO2_END
} // namespace

int main() {
    co2::setContractViolationHandler([](co2::ContractViolation const&) { std::_Exit(86); });
    std::vector<std::string> trace;
    {
        aw::manual_executor ctx;
        hop(ctx, trace);
    } // 队列里还有协程句柄：契约违规
    return 0;
}
```

`tests/unit/CMakeLists.txt` 追加：

```cmake
aw_unit_test(manual_executor_test)
aw_contract_test(manual_executor_destroy_with_handle_contract_test)
```

- [ ] **Step 2：运行，确认失败**

Run: `cmake --build build/debug --target aw-manual-executor-test 2>&1 | grep -m1 "fatal error"`
Expected: `fatal error: 'awaitable/exec/manual_executor.hpp' file not found`

- [ ] **Step 3：最小实现**

`include/awaitable/exec/detail/operation.hpp`：

```cpp
#pragma once

#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#include "awaitable/core/contract.hpp"
#include "awaitable/core/coroutine_handle.hpp"
#include "awaitable/exec/detail/recycling_allocator.hpp"

namespace aw {
namespace detail {

// §5.3：队列里的普通函数。invoke=true 执行并释放；invoke=false 只销毁（关闭阶段）。
struct operation {
    using complete_fn = void (*)(operation*, bool invoke);

    explicit operation(complete_fn fn) noexcept : complete_{fn} {}

    void complete() { complete_(this, true); }
    void destroy() noexcept {
        // invoke=false 的路径只做析构和释放，不会抛出。
        complete_(this, false);
    }

    complete_fn complete_;
    operation* next_{nullptr}; // strand 的侵入式队列
};

template <class F> struct function_operation final : operation {
    template <class G>
    explicit function_operation(G&& g) : operation{&do_complete}, function_(std::forward<G>(g)) {}

    static void do_complete(operation* base, bool invoke) {
        auto* const self = static_cast<function_operation*>(base);
        // 先把函数移到栈上并归还内存，再执行：执行期间投递的下一个 operation 能复用这块内存。
        F function(std::move(self->function_));
        self->~function_operation();
        recycling_deallocate(mem_tag::executor_function, self, sizeof(function_operation));
        if (invoke) function();
    }

    F function_;
};

template <class F> operation* make_function_operation(F&& f) {
    using op_type = function_operation<typename std::decay<F>::type>;
    void* const memory =
        recycling_allocate(mem_tag::executor_function, sizeof(op_type), alignof(op_type));
    try {
        return ::new (memory) op_type(std::forward<F>(f));
    } catch (...) {
        recycling_deallocate(mem_tag::executor_function, memory, sizeof(op_type));
        throw;
    }
}

// ---- 带标记的队列项：最低位 0 = 协程帧地址，1 = operation* ----

inline void* tag_handle(co2::coroutine_handle<> handle) noexcept { return handle.address(); }

inline void* tag_operation(operation* op) noexcept {
    return reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(op) | std::uintptr_t{1});
}

inline bool is_operation(void* item) noexcept {
    return (reinterpret_cast<std::uintptr_t>(item) & std::uintptr_t{1}) != 0U;
}

inline operation* as_operation(void* item) noexcept {
    return reinterpret_cast<operation*>(reinterpret_cast<std::uintptr_t>(item) &
                                        ~std::uintptr_t{1});
}

inline void run_item(void* item) {
    if (is_operation(item)) {
        as_operation(item)->complete();
    } else {
        co2::coroutine_handle<>::from_address(item).resume();
    }
}

inline void destroy_item(void* item) noexcept {
    if (is_operation(item)) {
        as_operation(item)->destroy();
    } else {
        CO2_CONTRACT_FAIL("an execution context was destroyed with a queued coroutine handle");
    }
}

} // namespace detail
} // namespace aw
```

`include/awaitable/exec/scheduler.hpp`：

```cpp
#pragma once

#include "awaitable/core/coroutine_handle.hpp"
#include "awaitable/exec/detail/operation.hpp"

namespace aw {

// §5.2：保留 coro 的 Scheduler（改名），新增 post / running_in_this_thread。
class scheduler {
  public:
    scheduler() = default;
    scheduler(scheduler const&) = delete;
    scheduler& operator=(scheduler const&) = delete;
    virtual ~scheduler() = default;

    virtual void schedule(co2::coroutine_handle<> coroutine) noexcept = 0;
    virtual void post(detail::operation* op) noexcept = 0;
    virtual bool running_in_this_thread() const noexcept = 0;
};

// 原 ScheduleOn：只给 core 级协程用。awaitable 帧里等待它会被 await_transform 拒绝（§4.3）。
struct schedule_on_awaiter {
    scheduler* target;

    bool await_ready() const noexcept { return false; }
    void await_suspend(co2::coroutine_handle<> coroutine) noexcept { target->schedule(coroutine); }
    void await_resume() const noexcept {}
};

inline schedule_on_awaiter schedule_on(scheduler& target) noexcept {
    return schedule_on_awaiter{&target};
}

} // namespace aw
```

`include/awaitable/exec/executor.hpp`：

```cpp
#pragma once

#include <type_traits>
#include <utility>

#include "awaitable/core/coroutine_handle.hpp"
#include "awaitable/exec/detail/operation.hpp"

namespace aw {

// §5.1 的 executor：保存 Context*，对 final 的具体类非虚调用。默认构造的句柄无效，只能被赋值。
template <class Context> class basic_executor {
  public:
    basic_executor() noexcept = default;
    explicit basic_executor(Context& context) noexcept : context_{&context} {}

    Context& context() const noexcept { return *context_; }

    template <class F> void post(F&& f) const {
        context_->post(detail::make_function_operation(std::forward<F>(f)));
    }

    template <class F> void dispatch(F&& f) const {
        if (context_->running_in_this_thread()) {
            typename std::decay<F>::type function(std::forward<F>(f));
            function();
            return;
        }
        post(std::forward<F>(f));
    }

    bool running_in_this_thread() const noexcept { return context_->running_in_this_thread(); }

    void schedule(co2::coroutine_handle<> coroutine) const noexcept { context_->schedule(coroutine); }

    friend bool operator==(basic_executor const& a, basic_executor const& b) noexcept {
        return a.context_ == b.context_;
    }
    friend bool operator!=(basic_executor const& a, basic_executor const& b) noexcept {
        return !(a == b);
    }

  private:
    Context* context_{nullptr};
};

template <class T, class = void> struct is_executor : std::false_type {};
template <class T>
struct is_executor<T, decltype(static_cast<void>(std::declval<T const&>().context()),
                               static_cast<void>(std::declval<T const&>().running_in_this_thread()))>
    : std::true_type {};

} // namespace aw
```

`include/awaitable/exec/manual_executor.hpp`：

```cpp
#pragma once

#include <cstddef>
#include <mutex>

#include "awaitable/core/contract.hpp"
#include "awaitable/core/detail/ring_queue.hpp"
#include "awaitable/exec/detail/operation.hpp"
#include "awaitable/exec/detail/thread_context.hpp"
#include "awaitable/exec/executor.hpp"
#include "awaitable/exec/scheduler.hpp"

namespace aw {

// §5.5：调用方驱动的 FIFO 执行上下文。run / run_one / poll 期间调用线程是库线程。
class manual_executor final : public scheduler {
  public:
    using executor_type = basic_executor<manual_executor>;

    manual_executor() = default;
    ~manual_executor() override { shutdown(); }

    executor_type get_executor() noexcept { return executor_type{*this}; }

    void schedule(co2::coroutine_handle<> coroutine) noexcept override {
        CO2_CONTRACT_CHECK(coroutine);
        std::lock_guard<std::mutex> lock{mutex_};
        CO2_CONTRACT_CHECK(!shutting_down_);
        ready_.push(detail::tag_handle(coroutine));
    }

    void post(detail::operation* op) noexcept override {
        {
            std::lock_guard<std::mutex> lock{mutex_};
            if (!shutting_down_) {
                ready_.push(detail::tag_operation(op));
                return;
            }
        }
        op->destroy(); // 关闭期间投递的项立即销毁（§5.4 第 5 条）
    }

    bool running_in_this_thread() const noexcept override { return driver() == this; }

    bool run_one() {
        driver_scope scope{this};
        return run_one_in_scope();
    }

    std::size_t run() {
        driver_scope scope{this};
        std::size_t count = 0U;
        while (run_one_in_scope()) ++count;
        return count;
    }

    // 只执行调用时已经就绪的项。
    std::size_t poll() {
        driver_scope scope{this};
        std::size_t ready_now = pending();
        std::size_t count = 0U;
        while (ready_now-- != 0U && run_one_in_scope()) ++count;
        return count;
    }

    std::size_t pending() const noexcept {
        std::lock_guard<std::mutex> lock{mutex_};
        return ready_.size();
    }

  private:
    struct driver_scope {
        explicit driver_scope(manual_executor* self) noexcept : previous{driver()} {
            driver() = self;
        }
        driver_scope(driver_scope const&) = delete;
        driver_scope& operator=(driver_scope const&) = delete;
        ~driver_scope() { driver() = previous; }
        manual_executor* previous;
        detail::thread_context context;
    };

    static manual_executor*& driver() noexcept {
        static thread_local manual_executor* current = nullptr;
        return current;
    }

    bool run_one_in_scope() {
        void* item = nullptr;
        {
            std::lock_guard<std::mutex> lock{mutex_};
            if (ready_.empty()) return false;
            item = ready_.pop();
        }
        detail::run_item(item);
        return true;
    }

    void shutdown() noexcept {
        {
            std::lock_guard<std::mutex> lock{mutex_};
            shutting_down_ = true;
        }
        for (;;) {
            void* item = nullptr;
            {
                std::lock_guard<std::mutex> lock{mutex_};
                if (ready_.empty()) return;
                item = ready_.pop();
            }
            detail::destroy_item(item);
        }
    }

    mutable std::mutex mutex_;
    co2::detail::RingQueue<void*> ready_;
    bool shutting_down_{false};
};

} // namespace aw
```

- [ ] **Step 4：运行，确认通过**

Run: `cmake --build build/debug -j8 && ctest --test-dir build/debug -R '^aw-manual-executor' --output-on-failure`
Expected: `100% tests passed out of 2`

- [ ] **Step 5：提交**

```bash
git add include/awaitable/exec tests/support/test_coro.hpp tests/unit
git commit -m "feat(exec): operation items, scheduler, basic_executor, manual_executor, schedule_on"
```

### Task 5：thread_pool 移植

**交付物：** `aw::thread_pool`（§5.4）。保留 coro 的调度结构，在其上加入带标记的队列项、`get_executor` / `post` / `running_in_this_thread`，以及每线程的 `thread_context`。任务异常导致 `std::terminate`；析构时先排空再销毁。

**Files:**
- Create（由一次性脚本从夹具生成，脚本不入库）：`include/awaitable/exec/thread_pool.hpp`
- Test: `tests/unit/thread_pool_test.cpp`, `tests/unit/thread_pool_exception_terminates_contract_test.cpp`
- Modify: `tests/unit/CMakeLists.txt`（追加）

**Interfaces:**
- Consumes（Task 4）：`scheduler`、`basic_executor`、`detail::operation` / `tag_*` / `run_item` / `destroy_item`；（Task 3）`thread_context`。
- Produces：`class aw::thread_pool final : public scheduler`，包含：
  - `using executor_type = basic_executor<thread_pool>`；
  - 构造：`explicit thread_pool(unsigned thread_count = default_thread_count())`，传 0 是契约违规；
  - `static unsigned default_thread_count() noexcept`；
  - `executor_type get_executor() noexcept`；
  - `unsigned thread_count() const noexcept`；
  - `bool running_in_this_thread() const noexcept override`；
  - `void schedule(co2::coroutine_handle<>) noexcept override`；
  - `void post(detail::operation*) noexcept override`。

- [ ] **Step 1：写失败的测试**

`tests/unit/thread_pool_test.cpp`：

```cpp
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "awaitable/exec/thread_pool.hpp"
#include "check.hpp"
#include "counting_new.hpp"
#include "test_coro.hpp"

namespace {

class latch {
  public:
    explicit latch(int count) : count_{count} {}
    void count_down() {
        std::lock_guard<std::mutex> lock{mutex_};
        if (--count_ == 0) cv_.notify_all();
    }
    void wait() {
        std::unique_lock<std::mutex> lock{mutex_};
        cv_.wait(lock, [this] { return count_ == 0; });
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int count_;
};

void postedFunctionsAllRun() {
    aw::thread_pool pool{4};
    auto ex = pool.get_executor();
    std::atomic<int> sum{0};
    latch done{1000};
    for (int i = 0; i != 1000; ++i) {
        ex.post([&, i] {
            sum.fetch_add(i);
            done.count_down();
        });
    }
    done.wait();
    AW_CHECK(sum.load() == 999 * 1000 / 2);
}

void runningInThisThreadAndDispatch() {
    aw::thread_pool pool{2};
    auto ex = pool.get_executor();
    AW_CHECK(!ex.running_in_this_thread());
    std::vector<std::string> trace;
    latch done{1};
    ex.post([&] {
        AW_CHECK(ex.running_in_this_thread());
        ex.dispatch([&] { trace.push_back("inline"); });
        trace.push_back("after");
        done.count_down();
    });
    done.wait();
    AW_CHECK((trace == std::vector<std::string>{"inline", "after"}));
}

auto hopOnto(aw::thread_pool& pool, bool& onWorker, latch& done)
    CO2_BEG(aw_test::eager_coro, (pool, onWorker, done)) {
    CO2_AWAIT(aw::schedule_on(pool));
    onWorker = pool.running_in_this_thread();
    done.count_down();
    CO2_RETURN();
}
CO2_END

void scheduleResumesOnAWorker() {
    aw::thread_pool pool{2};
    bool onWorker = false;
    latch done{1};
    hopOnto(pool, onWorker, done);
    done.wait();
    AW_CHECK(onWorker);
}

void queuedWorkIsRunOrDestroyedOnShutdown() {
    auto token = std::make_shared<int>(0);
    {
        aw::thread_pool pool{1};
        auto ex = pool.get_executor();
        std::shared_ptr<int> copy = token;
        ex.post([ex, copy] { ex.post([copy] {}); });
    }
    AW_CHECK(token.use_count() == 1);
}

void workerChainsReuseOperationMemory() {
    aw::thread_pool pool{1};
    auto ex = pool.get_executor();
    latch done{1};
    long news = -1;
    struct step {
        aw::thread_pool::executor_type ex;
        int remaining;
        aw_test::allocation_counter* counter;
        long* news;
        latch* done;
        void operator()() {
            if (remaining == 900) counter = new aw_test::allocation_counter{};
            if (remaining == 0) {
                *news = counter->news();
                delete counter;
                done->count_down();
                return;
            }
            ex.post(step{ex, remaining - 1, counter, news, done});
        }
    };
    ex.post(step{ex, 1000, nullptr, &news, &done});
    done.wait();
    if (aw_test::counting_new_enabled) AW_CHECK(news == 0);
}

} // namespace

int main() {
    postedFunctionsAllRun();
    runningInThisThreadAndDispatch();
    scheduleResumesOnAWorker();
    queuedWorkIsRunOrDestroyedOnShutdown();
    workerChainsReuseOperationMemory();
    AW_CHECK(aw::thread_pool::default_thread_count() >= 1U);
    return 0;
}
```

`tests/unit/thread_pool_exception_terminates_contract_test.cpp`：

```cpp
#include <cstdlib>
#include <exception>
#include <stdexcept>

#include "awaitable/exec/thread_pool.hpp"

// §5.4 第 4 条：任务里逸出的异常导致 std::terminate。用 terminate 处理器把它转成退出码 86。
int main() {
    std::set_terminate([] { std::_Exit(86); });
    aw::thread_pool pool{1};
    pool.get_executor().post([] { throw std::runtime_error{"escapes"}; });
    for (;;) {
    }
}
```

`tests/unit/CMakeLists.txt` 追加：

```cmake
aw_unit_test(thread_pool_test)
aw_contract_test(thread_pool_exception_terminates_contract_test)
```

- [ ] **Step 2：运行，确认失败**

Run: `cmake --build build/debug --target aw-thread-pool-test 2>&1 | grep -m1 "fatal error"`
Expected: `fatal error: 'awaitable/exec/thread_pool.hpp' file not found`

- [ ] **Step 3：由夹具生成移植版**

在仓库根目录运行下面的一次性脚本（不入库）。它以夹具里原样的 `thread_pool.hpp` 为输入，每处替换都必须恰好命中一次，否则报错退出：

```bash
python3 - <<'PYEOF'
# 由夹具 thread_pool.hpp 生成 include/awaitable/exec/thread_pool.hpp（规格 §5.4）。每处替换必须恰好命中一次。
import pathlib, sys
src = pathlib.Path("tests/legacy/include/co2/thread_pool.hpp").read_text()
edits = [
 ('#include "co2/contract.hpp"\n#include "co2/detail/ring_queue.hpp"\n#include "co2/scheduler.hpp"\n',
  '#include "awaitable/core/contract.hpp"\n#include "awaitable/core/coroutine_handle.hpp"\n'
  '#include "awaitable/core/detail/ring_queue.hpp"\n#include "awaitable/exec/detail/operation.hpp"\n'
  '#include "awaitable/exec/detail/thread_context.hpp"\n#include "awaitable/exec/executor.hpp"\n'
  '#include "awaitable/exec/scheduler.hpp"\n'),
 ('// co2 执行层：工作窃取线程池。\n',
  '// aw 执行层：工作窃取线程池（移植自 coro 0be5ab2 的 co2::ThreadPool，规格 §5.4）。\n'
  '// 移植改动：队列项带标记（§5.3）；get_executor / running_in_this_thread / post；每个工作线程\n'
  '// 建立 thread_context；任务逸出的异常经 noexcept 的 workerMain 导致 std::terminate；\n'
  '// 析构 join 之后排空队列，operation 以 invoke=false 销毁，之后的 post 立即销毁。\n'),
 ('namespace co2 {\nnamespace detail {', 'namespace aw {\nnamespace detail {'),
 ('struct ThreadPool final : Scheduler {\n    static unsigned defaultThreadCount() noexcept {',
  'class thread_pool final : public scheduler {\n  public:\n    using executor_type = basic_executor<thread_pool>;\n\n'
  '    static unsigned default_thread_count() noexcept {'),
 ('    explicit ThreadPool(unsigned const threadCount = defaultThreadCount()) {',
  '    explicit thread_pool(unsigned const threadCount = default_thread_count()) {'),
 ('    ~ThreadPool() override {\n        requestStop();\n        joinWorkers();\n    }\n',
  '    ~thread_pool() override {\n        requestStop();\n        joinWorkers();\n'
  '        closed.store(true, std::memory_order_seq_cst);\n        drainAfterJoin();\n    }\n\n'
  '    executor_type get_executor() noexcept { return executor_type{*this}; }\n'),
 ('    void schedule(coroutine_handle<> const coroutine) noexcept override {\n        CO2_CONTRACT_CHECK(coroutine);\n        auto* const item = coroutine.address();\n',
  '    void schedule(co2::coroutine_handle<> const coroutine) noexcept override {\n        CO2_CONTRACT_CHECK(coroutine);\n'
  '        CO2_CONTRACT_CHECK(not closed.load(std::memory_order_relaxed));\n        submit(detail::tag_handle(coroutine));\n    }\n\n'
  '    void post(detail::operation* const op) noexcept override {\n'
  '        if (closed.load(std::memory_order_seq_cst)) {\n            op->destroy();\n            return;\n        }\n'
  '        submit(detail::tag_operation(op));\n    }\n\n'
  '    void submit(void* const item) noexcept {\n'),
 ('    unsigned threadCount() const noexcept {', '    unsigned thread_count() const noexcept {'),
 ('    // 当前线程是否是本线程池的工作线程。\n    bool isWorkerThread() const noexcept {',
  '    // 当前线程是否是本线程池的工作线程。\n    bool running_in_this_thread() const noexcept override {'),
 ('        ThreadPool* pool;', '        thread_pool* pool;'),
 ('        Worker(ThreadPool& pool_,', '        Worker(thread_pool& pool_,'),
 ('    void workerMain(Worker& worker) noexcept {\n        currentWorkerSlot() = &worker;',
  '    // noexcept：任务里逸出的异常在这里导致 std::terminate（与 asio thread_pool 相同）。\n'
  '    void workerMain(Worker& worker) noexcept {\n        detail::thread_context context;\n        currentWorkerSlot() = &worker;'),
 ('                coroutine_handle<>::from_address(item).resume();', '                detail::run_item(item);'),
 ('    detail::RingQueue<void*> globalQueue;', '    co2::detail::RingQueue<void*> globalQueue;'),
 ('    std::atomic<bool> stopping{false};\n};\n\n} // namespace co2',
  '    std::atomic<bool> stopping{false};\n    std::atomic<bool> closed{false};\n\n'
  '    // join 之后：剩下的项全部销毁；销毁过程中新投递的 operation 因 closed 立即销毁。\n'
  '    void drainAfterJoin() noexcept {\n        for (;;) {\n            void* item = popGlobal();\n'
  '            for (auto& worker : workers) {\n                if (item != nullptr) break;\n                item = takeLocal(*worker);\n            }\n'
  '            if (item == nullptr) return;\n            detail::destroy_item(item);\n        }\n    }\n};\n\n} // namespace aw'),
]
for old, new in edits:
    n = src.count(old)
    if n != 1:
        sys.exit(f"edit not unique ({n}): {old[:60]!r}")
    src = src.replace(old, new)
out = pathlib.Path("include/awaitable/exec/thread_pool.hpp")
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(src)
print("wrote", out, src.count("\n"), "lines")
PYEOF
```

Expected: `wrote include/awaitable/exec/thread_pool.hpp 532 lines`

再确认残留的旧名只出现在注释里：

Run: `rg -n "Scheduler|isWorkerThread|defaultThreadCount" include/awaitable/exec/thread_pool.hpp; rg -c "ThreadPool" include/awaitable/exec/thread_pool.hpp`
Expected：第一条命令没有输出；第二条输出 `2`，两处都是注释。

- [ ] **Step 4：运行，确认通过**

Run: `cmake --build build/debug -j8 && ctest --test-dir build/debug -R '^aw-thread-pool' --output-on-failure`
Expected: `100% tests passed out of 2`

再跑一次 TSan：

Run: `tools/run-matrix.sh tsan-cxx14`
Expected：`tsan-cxx14: 100% tests passed out of <N>`。这里的 `<N>` 是 27 加上到本任务为止的 aw 测试数，即 35。

- [ ] **Step 5：提交**

```bash
git add include/awaitable/exec/thread_pool.hpp tests/unit
git commit -m "feat(exec): port thread_pool (tagged items, executor, thread_context, terminate on exception)"
```

### Task 6：strand

**交付物：** `aw::strand<Executor>` 和 `aw::make_strand`（§5.6），结构照搬 asio 的 `strand_executor_service`，保证任务互不并发并保持 FIFO。

**Files:**
- Create: `include/awaitable/exec/strand.hpp`
- Test: `tests/unit/strand_test.cpp`
- Modify: `tests/unit/CMakeLists.txt`（追加）

**Interfaces:**
- Consumes（Task 4 / 5）：`basic_executor`、`detail::operation`（含 `next_`）、`make_function_operation`；`thread_pool`、`manual_executor`。
- Produces：`template <class Executor> class aw::strand`，包含：
  - `using inner_executor_type = Executor`；
  - 构造：默认构造（无效）、`explicit strand(Executor inner)`；
  - 访问：`get_inner_executor()`、`context()`（返回内层 executor 的上下文）；
  - 投递：`post(F&&)`、`dispatch(F&&)`、`schedule(h) noexcept`；
  - `running_in_this_thread()`；
  - 比较：`==` / `!=`，按共享的 impl 比较；
  - 自由函数 `template <class Executor> strand<Executor> aw::make_strand(Executor const&)`。

- [ ] **Step 1：写失败的测试**

`tests/unit/strand_test.cpp`：

```cpp
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "awaitable/exec/manual_executor.hpp"
#include "awaitable/exec/strand.hpp"
#include "awaitable/exec/thread_pool.hpp"
#include "check.hpp"

namespace {

void tasksNeverOverlapAndKeepFifoPerProducer() {
    aw::thread_pool pool{4};
    auto s = aw::make_strand(pool.get_executor());
    std::atomic<int> inside{0};
    std::atomic<bool> overlap{false};
    int counter = 0; // 只在 strand 上访问，不加锁
    std::vector<int> order[4];
    std::mutex done_mutex;
    std::condition_variable done_cv;
    int finished = 0;
    std::vector<std::thread> producers;
    for (int p = 0; p != 4; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i != 2500; ++i) {
                s.post([&, p, i] {
                    if (inside.fetch_add(1) != 0) overlap = true;
                    ++counter;
                    order[p].push_back(i);
                    inside.fetch_sub(1);
                    if (counter == 10000) {
                        std::lock_guard<std::mutex> lock{done_mutex};
                        finished = 1;
                        done_cv.notify_all();
                    }
                });
            }
        });
    }
    for (auto& t : producers) t.join();
    std::unique_lock<std::mutex> lock{done_mutex};
    done_cv.wait(lock, [&] { return finished == 1; });
    AW_CHECK(!overlap.load());
    AW_CHECK(counter == 10000);
    for (auto const& o : order) {
        AW_CHECK(o.size() == 2500U);
        for (std::size_t i = 0; i != o.size(); ++i) AW_CHECK(o[i] == static_cast<int>(i));
    }
}

void runningInThisThreadAndDispatch() {
    aw::manual_executor ctx;
    auto s = aw::make_strand(ctx.get_executor());
    auto other = aw::make_strand(ctx.get_executor());
    std::vector<int> trace;
    AW_CHECK(!s.running_in_this_thread());
    s.dispatch([&] { trace.push_back(0); }); // 不在 strand 上：排队
    AW_CHECK(trace.empty());
    s.post([&] {
        AW_CHECK(s.running_in_this_thread());
        AW_CHECK(!other.running_in_this_thread());
        s.dispatch([&] { trace.push_back(1); }); // 在 strand 上：内联
        trace.push_back(2);
    });
    ctx.get_executor().post([&] { AW_CHECK(!s.running_in_this_thread()); });
    ctx.run();
    AW_CHECK((trace == std::vector<int>{0, 1, 2}));
}

void exceptionsLeaveTheStrandUsable() {
    aw::manual_executor ctx;
    auto s = aw::make_strand(ctx.get_executor());
    int ran = 0;
    s.post([] { throw std::runtime_error{"boom"}; });
    s.post([&] { ++ran; });
    bool caught = false;
    try {
        ctx.run();
    } catch (std::runtime_error const&) {
        caught = true;
    }
    AW_CHECK(caught);
    ctx.run();
    AW_CHECK(ran == 1);
    s.post([&] { ++ran; });
    ctx.run();
    AW_CHECK(ran == 2);
}

void equalityAndAccessors() {
    aw::manual_executor ctx;
    auto a = aw::make_strand(ctx.get_executor());
    auto copy = a;
    auto b = aw::make_strand(ctx.get_executor());
    AW_CHECK(a == copy && a != b);
    AW_CHECK(a.get_inner_executor() == ctx.get_executor());
    AW_CHECK(&a.context() == &ctx);
    static_assert(aw::is_executor<aw::strand<aw::manual_executor::executor_type>>::value, "");
}

void destroyingTheLastHandleDestroysPendingWork() {
    int destroyed = 0;
    struct counted {
        int* destroyed;
        bool moved{false};
        counted(int* d) : destroyed{d} {}
        counted(counted&& other) noexcept : destroyed{other.destroyed} { other.moved = true; }
        ~counted() {
            if (!moved) ++*destroyed;
        }
        void operator()() const {}
    };
    {
        aw::manual_executor ctx;
        auto s = aw::make_strand(ctx.get_executor());
        s.post(counted{&destroyed});
        s.post(counted{&destroyed});
    }
    AW_CHECK(destroyed == 2);
}

} // namespace

int main() {
    tasksNeverOverlapAndKeepFifoPerProducer();
    runningInThisThreadAndDispatch();
    exceptionsLeaveTheStrandUsable();
    equalityAndAccessors();
    destroyingTheLastHandleDestroysPendingWork();
    return 0;
}
```

`tests/unit/CMakeLists.txt` 追加：

```cmake
aw_unit_test(strand_test)
```

- [ ] **Step 2：运行，确认失败**

Run: `cmake --build build/debug --target aw-strand-test 2>&1 | grep -m1 "fatal error"`
Expected: `fatal error: 'awaitable/exec/strand.hpp' file not found`

- [ ] **Step 3：最小实现**

`include/awaitable/exec/strand.hpp`：

```cpp
#pragma once

#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

#include "awaitable/core/coroutine_handle.hpp"
#include "awaitable/exec/detail/operation.hpp"

namespace aw {
namespace detail {

// 侵入式 FIFO（operation::next_）。
struct op_queue {
    operation* front{nullptr};
    operation* back{nullptr};

    bool empty() const noexcept { return front == nullptr; }
    void push(operation* op) noexcept {
        op->next_ = nullptr;
        if (back != nullptr) {
            back->next_ = op;
        } else {
            front = op;
        }
        back = op;
    }
    operation* pop() noexcept {
        operation* const op = front;
        front = op->next_;
        if (front == nullptr) back = nullptr;
        op->next_ = nullptr;
        return op;
    }
    void splice(op_queue& other) noexcept {
        if (other.empty()) return;
        if (back != nullptr) {
            back->next_ = other.front;
        } else {
            front = other.front;
        }
        back = other.back;
        other.front = other.back = nullptr;
    }
    void destroy_all() noexcept {
        while (!empty()) pop()->destroy();
    }
};

// 照搬 asio strand_executor_service::strand_impl：locked 为真表示有一个 invoker 已投递或正在运行；
// ready 只由持有 locked 的 invoker 访问，waiting 由互斥锁保护。
struct strand_impl {
    std::mutex mutex;
    bool locked{false};
    op_queue waiting;
    op_queue ready;

    strand_impl() = default;
    strand_impl(strand_impl const&) = delete;
    strand_impl& operator=(strand_impl const&) = delete;
    ~strand_impl() {
        waiting.destroy_all();
        ready.destroy_all();
    }

    // 线程局部的 strand 调用栈，供 running_in_this_thread。
    struct call_stack_entry {
        strand_impl const* impl;
        call_stack_entry* next;
    };
    static call_stack_entry*& top() noexcept {
        static thread_local call_stack_entry* current = nullptr;
        return current;
    }
    static bool running_in_this_thread(strand_impl const* impl) noexcept {
        for (call_stack_entry* e = top(); e != nullptr; e = e->next)
            if (e->impl == impl) return true;
        return false;
    }

    // 入队。返回 true 表示调用方需要投递 invoker。
    bool enqueue(operation* op) noexcept {
        std::lock_guard<std::mutex> lock{mutex};
        if (locked) {
            waiting.push(op);
            return false;
        }
        locked = true;
        ready.push(op);
        return true;
    }
};

template <class Executor> struct strand_invoker {
    std::shared_ptr<strand_impl> impl;
    Executor inner;

    // 收尾：把 waiting 移进 ready；ready 非空就重新投递 invoker，否则解锁。异常离开时同样执行。
    struct on_exit {
        strand_invoker* self;
        explicit on_exit(strand_invoker* s) noexcept : self{s} {}
        on_exit(on_exit const&) = delete;
        on_exit& operator=(on_exit const&) = delete;
        ~on_exit() {
            bool more = false;
            {
                std::lock_guard<std::mutex> lock{self->impl->mutex};
                self->impl->ready.splice(self->impl->waiting);
                more = !self->impl->ready.empty();
                if (!more) self->impl->locked = false;
            }
            if (more) self->inner.post(strand_invoker{self->impl, self->inner});
        }
    };

    void operator()() {
        strand_impl::call_stack_entry entry{impl.get(), strand_impl::top()};
        strand_impl::top() = &entry;
        struct pop_entry {
            strand_impl::call_stack_entry* entry;
            ~pop_entry() { strand_impl::top() = entry->next; }
        } pop{&entry};
        on_exit exit_guard{this};
        while (!impl->ready.empty()) impl->ready.pop()->complete();
    }
};

} // namespace detail

template <class Executor> class strand {
  public:
    using inner_executor_type = Executor;

    strand() noexcept = default;
    explicit strand(Executor inner)
        : inner_{std::move(inner)}, impl_{std::make_shared<detail::strand_impl>()} {}

    Executor const& get_inner_executor() const noexcept { return inner_; }
    auto& context() const noexcept { return inner_.context(); }

    template <class F> void post(F&& f) const {
        enqueue(detail::make_function_operation(std::forward<F>(f)));
    }

    template <class F> void dispatch(F&& f) const {
        if (running_in_this_thread()) {
            typename std::decay<F>::type function(std::forward<F>(f));
            function();
            return;
        }
        post(std::forward<F>(f));
    }

    bool running_in_this_thread() const noexcept {
        return detail::strand_impl::running_in_this_thread(impl_.get());
    }

    void schedule(co2::coroutine_handle<> coroutine) const noexcept {
        post([coroutine] { coroutine.resume(); });
    }

    friend bool operator==(strand const& a, strand const& b) noexcept { return a.impl_ == b.impl_; }
    friend bool operator!=(strand const& a, strand const& b) noexcept { return !(a == b); }

  private:
    void enqueue(detail::operation* op) const {
        if (impl_->enqueue(op)) inner_.post(detail::strand_invoker<Executor>{impl_, inner_});
    }

    Executor inner_{};
    std::shared_ptr<detail::strand_impl> impl_;
};

template <class Executor> strand<Executor> make_strand(Executor const& ex) {
    return strand<Executor>{ex};
}

} // namespace aw
```

- [ ] **Step 4：运行，确认通过（含 TSan）**

Run: `cmake --build build/debug -j8 && ctest --test-dir build/debug -R '^aw-strand' --output-on-failure && tools/run-matrix.sh tsan-cxx14`
Expected：先输出 `100% tests passed out of 1`，再输出 `tsan-cxx14: 100% tests passed out of 36`。

- [ ] **Step 5：提交**

```bash
git add include/awaitable/exec/strand.hpp tests/unit
git commit -m "feat(exec): strand (asio strand_executor_service structure)"
```

### Task 7：cancel 层：cancellation_type / signal / slot / filter / state

**交付物：** §6.1 的全部类型，语义照搬 asio。处理器内存来自 `cancellation_signal` 标签，可复用旧块。`cancellation_state` 提供观察者钩子，供 Task 8 的 stop_token 桥接使用。

**Files:**
- Create: `include/awaitable/cancel/cancellation_type.hpp`, `include/awaitable/cancel/cancellation_signal.hpp`, `include/awaitable/cancel/cancellation_state.hpp`
- Test: `tests/unit/cancellation_test.cpp`
- Modify: `tests/unit/CMakeLists.txt`（追加）

**Interfaces:**
- Consumes（Task 3）：`recycling_allocate` / `recycling_deallocate`、`mem_tag::cancellation_signal`、`thread_context`。cancel 层只依赖 core 和这两个分配函数所在的头。

  > 注：回收缓存头文件位于 `exec/detail/`，但它只依赖 core。cancel 包含它，不算 exec 与 cancel 互相依赖（自审第 3 条有说明）。
- Produces：
  - `enum class aw::cancellation_type : unsigned { none = 0, terminal = 1, partial = 2, total = 4, all = 0xFFFFFFFF }`，提供 `| & ^ ~ |= &= ^=` 和 `!`。
  - `class aw::cancellation_signal`：`emit(cancellation_type)`、`cancellation_slot slot() noexcept`。
  - `class aw::cancellation_slot`：
    - `template <class H, class... Args> H& emplace(Args&&...)`；
    - `template <class H> std::decay_t<H>& assign(H&&)`；
    - `clear()`、`is_connected()`、`has_handler()`；
    - 比较：`==` / `!=`。
  - `template <cancellation_type Mask> struct aw::cancellation_filter`，以及别名 `disable_cancellation`、`enable_terminal_cancellation`、`enable_partial_cancellation`、`enable_total_cancellation`。
  - `class aw::cancellation_state`：
    - 构造：默认构造；`template <class In = enable_terminal_cancellation, class Out = In> explicit cancellation_state(cancellation_slot parent, In in = {}, Out out = {})`；
    - `slot()`、`cancelled()`、`clear(mask = all)`；
    - 本库新增 `set_observer(void (*fn)(void*, cancellation_type), void* arg) noexcept`：in 过滤之后的非 none 取消都会先通知观察者。

- [ ] **Step 1：写失败的测试**

`tests/unit/cancellation_test.cpp`：

```cpp
#include <vector>

#include "awaitable/cancel/cancellation_signal.hpp"
#include "awaitable/cancel/cancellation_state.hpp"
#include "awaitable/cancel/cancellation_type.hpp"
#include "awaitable/exec/detail/thread_context.hpp"
#include "check.hpp"
#include "counting_new.hpp"

using aw::cancellation_type;

namespace {

struct recorder {
    std::vector<cancellation_type>* seen;
    void operator()(cancellation_type t) { seen->push_back(t); }
};

void typeOperators() {
    auto t = cancellation_type::terminal | cancellation_type::partial;
    AW_CHECK((t & cancellation_type::partial) == cancellation_type::partial);
    AW_CHECK(!(t & cancellation_type::total));
    AW_CHECK((~cancellation_type::none) == cancellation_type::all);
    t &= ~cancellation_type::terminal;
    AW_CHECK(t == cancellation_type::partial);
}

void slotBasics() {
    aw::cancellation_signal signal;
    aw::cancellation_slot slot = signal.slot();
    AW_CHECK(slot.is_connected() && !slot.has_handler());
    AW_CHECK(!aw::cancellation_slot{}.is_connected());
    signal.emit(cancellation_type::terminal); // 没有处理器：什么也不做
    std::vector<cancellation_type> seen;
    slot.emplace<recorder>(recorder{&seen});
    AW_CHECK(slot.has_handler() && signal.slot() == slot);
    signal.emit(cancellation_type::partial);
    AW_CHECK((seen == std::vector<cancellation_type>{cancellation_type::partial}));
    slot.clear();
    AW_CHECK(!slot.has_handler());
    signal.emit(cancellation_type::terminal);
    AW_CHECK(seen.size() == 1U);
}

void replacingAHandlerReusesItsMemory() {
    aw::detail::thread_context context;
    aw::cancellation_signal signal;
    std::vector<cancellation_type> seen;
    signal.slot().assign(recorder{&seen});
    aw_test::allocation_counter counter;
    for (int i = 0; i != 10; ++i) signal.slot().assign(recorder{&seen});
    signal.slot().clear();
    signal.slot().assign(recorder{&seen});
    if (aw_test::counting_new_enabled) AW_CHECK(counter.news() == 0);
}

void stateFiltersAndForwards() {
    aw::cancellation_signal parent;
    aw::cancellation_state state{parent.slot()}; // 默认只接收 terminal
    std::vector<cancellation_type> child;
    state.slot().assign(recorder{&child});
    parent.emit(cancellation_type::partial);
    AW_CHECK(state.cancelled() == cancellation_type::none && child.empty());
    parent.emit(cancellation_type::terminal | cancellation_type::partial);
    AW_CHECK(state.cancelled() == cancellation_type::terminal);
    AW_CHECK((child == std::vector<cancellation_type>{cancellation_type::terminal}));
    state.clear();
    AW_CHECK(state.cancelled() == cancellation_type::none);
}

void totalFilterAndSeparateOutFilter() {
    aw::cancellation_signal parent;
    aw::cancellation_state state{parent.slot(), aw::enable_total_cancellation{},
                                 aw::enable_terminal_cancellation{}};
    std::vector<cancellation_type> child;
    state.slot().assign(recorder{&child});
    parent.emit(cancellation_type::total);
    AW_CHECK(state.cancelled() == cancellation_type::total);
    AW_CHECK(child.empty()); // out 过滤器只放行 terminal
    aw::cancellation_state disabled{parent.slot(), aw::disable_cancellation{}};
    parent.emit(cancellation_type::all);
    AW_CHECK(disabled.cancelled() == cancellation_type::none);
}

void observerSeesFilteredCancellation() {
    aw::cancellation_signal parent;
    aw::cancellation_state state{parent.slot()};
    std::vector<cancellation_type> observed;
    state.set_observer(
        [](void* arg, cancellation_type t) {
            static_cast<std::vector<cancellation_type>*>(arg)->push_back(t);
        },
        &observed);
    parent.emit(cancellation_type::partial);
    parent.emit(cancellation_type::all);
    AW_CHECK((observed == std::vector<cancellation_type>{cancellation_type::terminal}));
}

void unconnectedState() {
    aw::cancellation_state state;
    AW_CHECK(!state.slot().is_connected());
    AW_CHECK(state.cancelled() == cancellation_type::none);
    aw::cancellation_state from_empty{aw::cancellation_slot{}};
    AW_CHECK(!from_empty.slot().is_connected());
}

} // namespace

int main() {
    typeOperators();
    slotBasics();
    replacingAHandlerReusesItsMemory();
    stateFiltersAndForwards();
    totalFilterAndSeparateOutFilter();
    observerSeesFilteredCancellation();
    unconnectedState();
    return 0;
}
```

`tests/unit/CMakeLists.txt` 追加：

```cmake
aw_unit_test(cancellation_test)
```

- [ ] **Step 2：运行，确认失败**

Run: `cmake --build build/debug --target aw-cancellation-test 2>&1 | grep -m1 "fatal error"`
Expected: `fatal error: 'awaitable/cancel/cancellation_signal.hpp' file not found`

- [ ] **Step 3：最小实现**

`include/awaitable/cancel/cancellation_type.hpp`：

```cpp
#pragma once

namespace aw {

enum class cancellation_type : unsigned {
    none = 0U,
    terminal = 1U,
    partial = 2U,
    total = 4U,
    all = 0xFFFFFFFFU
};

constexpr cancellation_type operator|(cancellation_type a, cancellation_type b) noexcept {
    return static_cast<cancellation_type>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}
constexpr cancellation_type operator&(cancellation_type a, cancellation_type b) noexcept {
    return static_cast<cancellation_type>(static_cast<unsigned>(a) & static_cast<unsigned>(b));
}
constexpr cancellation_type operator^(cancellation_type a, cancellation_type b) noexcept {
    return static_cast<cancellation_type>(static_cast<unsigned>(a) ^ static_cast<unsigned>(b));
}
constexpr cancellation_type operator~(cancellation_type a) noexcept {
    return static_cast<cancellation_type>(~static_cast<unsigned>(a));
}
constexpr bool operator!(cancellation_type a) noexcept { return static_cast<unsigned>(a) == 0U; }
inline cancellation_type& operator|=(cancellation_type& a, cancellation_type b) noexcept {
    return a = a | b;
}
inline cancellation_type& operator&=(cancellation_type& a, cancellation_type b) noexcept {
    return a = a & b;
}
inline cancellation_type& operator^=(cancellation_type& a, cancellation_type b) noexcept {
    return a = a ^ b;
}

} // namespace aw
```

`include/awaitable/cancel/cancellation_signal.hpp`：

```cpp
#pragma once

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

#include "awaitable/cancel/cancellation_type.hpp"
#include "awaitable/exec/detail/recycling_allocator.hpp"

namespace aw {
namespace detail {

class cancellation_handler_base {
  public:
    virtual void call(cancellation_type type) = 0;
    // 析构自身，返回所占内存（地址与分配时的大小），由调用方决定复用还是归还。
    virtual std::pair<void*, std::size_t> destroy() noexcept = 0;

  protected:
    virtual ~cancellation_handler_base() = default;
};

template <class Handler> class cancellation_handler final : public cancellation_handler_base {
  public:
    template <class... Args>
    explicit cancellation_handler(std::size_t size, Args&&... args)
        : handler_(std::forward<Args>(args)...), size_{size} {}

    void call(cancellation_type type) override { handler_(type); }

    std::pair<void*, std::size_t> destroy() noexcept override {
        std::pair<void*, std::size_t> const memory{this, size_};
        this->~cancellation_handler();
        return memory;
    }

    Handler& handler() noexcept { return handler_; }

  private:
    ~cancellation_handler() override = default;

    Handler handler_;
    std::size_t size_;
};

} // namespace detail

class cancellation_slot;

class cancellation_signal {
  public:
    cancellation_signal() noexcept = default;
    cancellation_signal(cancellation_signal const&) = delete;
    cancellation_signal& operator=(cancellation_signal const&) = delete;
    ~cancellation_signal() {
        if (handler_ != nullptr) {
            std::pair<void*, std::size_t> const memory = handler_->destroy();
            detail::recycling_deallocate(detail::mem_tag::cancellation_signal, memory.first,
                                         memory.second);
        }
    }

    void emit(cancellation_type type) {
        if (handler_ != nullptr) handler_->call(type);
    }

    cancellation_slot slot() noexcept;

  private:
    detail::cancellation_handler_base* handler_{nullptr};
};

class cancellation_slot {
  public:
    constexpr cancellation_slot() noexcept = default;

    template <class H, class... Args> H& emplace(Args&&... args) {
        using handler_type = detail::cancellation_handler<H>;
        std::pair<void*, std::size_t> const memory =
            prepare_memory(sizeof(handler_type), alignof(handler_type));
        handler_type* handler = nullptr;
        try {
            handler = ::new (memory.first) handler_type(memory.second, std::forward<Args>(args)...);
        } catch (...) {
            detail::recycling_deallocate(detail::mem_tag::cancellation_signal, memory.first,
                                         memory.second);
            throw;
        }
        *handler_ = handler;
        return handler->handler();
    }

    template <class H> typename std::decay<H>::type& assign(H&& handler) {
        return emplace<typename std::decay<H>::type>(std::forward<H>(handler));
    }

    void clear() {
        if (handler_ != nullptr && *handler_ != nullptr) {
            std::pair<void*, std::size_t> const memory = (*handler_)->destroy();
            *handler_ = nullptr;
            detail::recycling_deallocate(detail::mem_tag::cancellation_signal, memory.first,
                                         memory.second);
        }
    }

    bool is_connected() const noexcept { return handler_ != nullptr; }
    bool has_handler() const noexcept { return handler_ != nullptr && *handler_ != nullptr; }

    friend bool operator==(cancellation_slot const& a, cancellation_slot const& b) noexcept {
        return a.handler_ == b.handler_;
    }
    friend bool operator!=(cancellation_slot const& a, cancellation_slot const& b) noexcept {
        return !(a == b);
    }

  private:
    friend class cancellation_signal;
    explicit cancellation_slot(detail::cancellation_handler_base** handler) noexcept
        : handler_{handler} {}

    // 旧处理器的块够大、对齐合适就复用（asio cancellation_signal.hpp:150-198）。
    std::pair<void*, std::size_t> prepare_memory(std::size_t size, std::size_t align) {
        std::pair<void*, std::size_t> memory{nullptr, 0U};
        if (*handler_ != nullptr) {
            memory = (*handler_)->destroy();
            *handler_ = nullptr;
        }
        if (memory.first == nullptr || size > memory.second ||
            reinterpret_cast<std::size_t>(memory.first) % align != 0U) {
            if (memory.first != nullptr)
                detail::recycling_deallocate(detail::mem_tag::cancellation_signal, memory.first,
                                             memory.second);
            memory.first =
                detail::recycling_allocate(detail::mem_tag::cancellation_signal, size, align);
            memory.second = size;
        }
        return memory;
    }

    detail::cancellation_handler_base** handler_{nullptr};
};

inline cancellation_slot cancellation_signal::slot() noexcept { return cancellation_slot{&handler_}; }

} // namespace aw
```

`include/awaitable/cancel/cancellation_state.hpp`：

```cpp
#pragma once

#include "awaitable/cancel/cancellation_signal.hpp"
#include "awaitable/cancel/cancellation_type.hpp"

namespace aw {

template <cancellation_type Mask> struct cancellation_filter {
    cancellation_type operator()(cancellation_type type) const noexcept { return type & Mask; }
};

using disable_cancellation = cancellation_filter<cancellation_type::none>;
using enable_terminal_cancellation = cancellation_filter<cancellation_type::terminal>;
using enable_partial_cancellation =
    cancellation_filter<cancellation_type::terminal | cancellation_type::partial>;
using enable_total_cancellation =
    cancellation_filter<cancellation_type::terminal | cancellation_type::partial |
                        cancellation_type::total>;

// asio cancellation_state.hpp:31-197。impl 作为处理器住在父 slot 里，state 只是指向它的指针。
class cancellation_state {
  public:
    constexpr cancellation_state() noexcept = default;

    template <class In = enable_terminal_cancellation, class Out = In>
    explicit cancellation_state(cancellation_slot parent, In in = In{}, Out out = Out{})
        : impl_{parent.is_connected() ? &parent.template emplace<impl<In, Out>>(in, out) : nullptr} {}

    cancellation_slot slot() const noexcept {
        return impl_ != nullptr ? impl_->signal_.slot() : cancellation_slot{};
    }

    cancellation_type cancelled() const noexcept {
        return impl_ != nullptr ? impl_->cancelled_ : cancellation_type::none;
    }

    void clear(cancellation_type mask = cancellation_type::all) noexcept {
        if (impl_ != nullptr) impl_->cancelled_ &= ~mask;
    }

    void set_observer(void (*fn)(void*, cancellation_type), void* arg) noexcept {
        if (impl_ != nullptr) {
            impl_->observer_ = fn;
            impl_->observer_arg_ = arg;
        }
    }

  private:
    struct impl_base {
        impl_base() = default;
        impl_base(impl_base const&) = delete;
        impl_base& operator=(impl_base const&) = delete;
        cancellation_signal signal_;
        cancellation_type cancelled_{cancellation_type::none};
        void (*observer_)(void*, cancellation_type){nullptr};
        void* observer_arg_{nullptr};
    };

    template <class In, class Out> struct impl : impl_base {
        impl(In in, Out out) : in_{in}, out_{out} {}
        void operator()(cancellation_type type) {
            this->cancelled_ = in_(type);
            cancellation_type const forwarded = out_(this->cancelled_);
            if (!!this->cancelled_ && this->observer_ != nullptr)
                this->observer_(this->observer_arg_, this->cancelled_);
            if (!!forwarded) this->signal_.emit(forwarded);
        }
        In in_;
        Out out_;
    };

    impl_base* impl_{nullptr};
};

} // namespace aw
```

- [ ] **Step 4：运行，确认通过**

Run: `cmake --build build/debug -j8 && ctest --test-dir build/debug -R '^aw-cancellation' --output-on-failure`
Expected: `100% tests passed out of 1`

- [ ] **Step 5：提交**

```bash
git add include/awaitable/cancel tests/unit
git commit -m "feat(cancel): cancellation_type, signal/slot with recycled handler memory, filters, state"
```

### Task 8：stop_token 移植与适配器

**交付物：** `aw::stop_token` / `stop_source` / `stop_callback`。内容与 coro 相同，只是移入 `aw` 命名空间。另提供：
- `get_stop_token()` 标签，以及兼容名 `getStopToken()`；
- `inherit_stop_token`，以及兼容名 `inheritStopToken`；
- 单向桥 `detail::stop_bridge`，把 cancellation 转成 `request_stop`（§6.4）。

**Files:**
- Create（由命令从夹具生成）：`include/awaitable/cancel/stop_token.hpp`
- Create: `include/awaitable/cancel/stop_token_adapter.hpp`
- Test: `tests/unit/stop_token_adapter_test.cpp`
- Modify: `tests/unit/CMakeLists.txt`（追加）

**Interfaces:**
- Consumes（Task 7）：`cancellation_state::set_observer`、`cancellation_type`；（Task 3）`recycling_allocate`、`mem_tag::cancellation_signal`。
- Produces：
  - `aw::stop_token`、`aw::stop_source`、`aw::stop_callback<Callback>`、`aw::nostopstate`、`aw::detail::EnvStopToken`（提供 `own` / `borrow` / `get` / `reset`）。
  - `struct aw::get_stop_token_t { using aw_this_coro_tag = void; }`、`aw::get_stop_token()`、`aw::getStopToken()`。
  - `template <class Promise> void aw::inherit_stop_token(detail::EnvStopToken& into, co2::coroutine_handle<Promise> awaiting) noexcept`，另有同签名的 `aw::inheritStopToken`。
  - `class aw::detail::stop_bridge`：
    - `static stop_bridge* create()`、`static void destroy(stop_bridge*) noexcept`；
    - `void attach(cancellation_state&) noexcept`；
    - `stop_token token() const noexcept`；
    - `EnvStopToken const& env() const noexcept`。

- [ ] **Step 1：写失败的测试**

`tests/unit/stop_token_adapter_test.cpp`：

```cpp
#include <exception>
#include <type_traits>

#include "awaitable/cancel/cancellation_signal.hpp"
#include "awaitable/cancel/cancellation_state.hpp"
#include "awaitable/cancel/stop_token.hpp"
#include "awaitable/cancel/stop_token_adapter.hpp"
#include "awaitable/core/coroutine.hpp"
#include "awaitable/core/dsl.hpp"
#include "check.hpp"

namespace {

// 一个提供 envStopToken() 的 core 级 promise：inherit_stop_token 应当借用它的状态。
struct env_coro {
    struct promise_type {
        aw::detail::EnvStopToken env;
        aw::detail::EnvStopToken const& envStopToken() const noexcept { return env; }
        env_coro get_return_object() noexcept { return {}; }
        co2::suspend_never initial_suspend() const noexcept { return {}; }
        co2::suspend_never final_suspend() const noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };
};

// 只提供 get_stop_token() 的 promise：inherit_stop_token 应当自己持有一份引用。
struct token_coro {
    struct promise_type {
        aw::stop_token token;
        aw::stop_token get_stop_token() const noexcept { return token; }
        token_coro get_return_object() noexcept { return {}; }
        co2::suspend_never initial_suspend() const noexcept { return {}; }
        co2::suspend_never final_suspend() const noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };
};

struct capture_env {
    aw::detail::EnvStopToken* into;
    aw::stop_source* source;
    bool await_ready() const noexcept { return false; }
    template <class P> bool await_suspend(co2::coroutine_handle<P> h) noexcept {
        h.promise().set_source(*source);
        aw::inherit_stop_token(*into, h);
        return false;
    }
    void await_resume() const noexcept {}
};

struct env_coro_with_source {
    struct promise_type : env_coro::promise_type {
        void set_source(aw::stop_source& s) noexcept { env.own(s.get_token()); }
        env_coro_with_source get_return_object() noexcept { return {}; }
    };
};

struct token_coro_with_source {
    struct promise_type : token_coro::promise_type {
        void set_source(aw::stop_source& s) noexcept { token = s.get_token(); }
        token_coro_with_source get_return_object() noexcept { return {}; }
    };
};

auto borrowFromEnv(aw::detail::EnvStopToken& into, aw::stop_source& source, bool& requested)
    CO2_BEG(env_coro_with_source, (into, source, requested)) {
    CO2_AWAIT((capture_env{&into, &source}));
    source.request_stop();
    requested = into.get().stop_requested();
    into.reset(); // 借用的状态只在等待者活着时有效
    CO2_RETURN();
}
CO2_END

auto ownFromToken(aw::detail::EnvStopToken& into, aw::stop_source& source)
    CO2_BEG(token_coro_with_source, (into, source)) {
    CO2_AWAIT((capture_env{&into, &source}));
    CO2_RETURN();
}
CO2_END

void inheritBorrowsOrOwns() {
    aw::stop_source source;
    aw::detail::EnvStopToken borrowed;
    bool requested = false;
    borrowFromEnv(borrowed, source, requested);
    AW_CHECK(requested);

    aw::stop_source other;
    aw::detail::EnvStopToken owned;
    ownFromToken(owned, other); // 协程已结束：owned 必须自己持有引用
    other.request_stop();
    AW_CHECK(owned.get().stop_requested());
}

void bridgeTurnsCancellationIntoStopRequest() {
    aw::detail::thread_context context;
    aw::cancellation_signal parent;
    aw::cancellation_state state{parent.slot()};
    aw::detail::stop_bridge* bridge = aw::detail::stop_bridge::create();
    bridge->attach(state);
    aw::stop_token token = bridge->token();
    bool called = false;
    aw::stop_callback<> callback{token, [&] { called = true; }};
    AW_CHECK(token.stop_possible() && !token.stop_requested());
    parent.emit(aw::cancellation_type::partial); // 默认过滤器拦掉
    AW_CHECK(!token.stop_requested());
    parent.emit(aw::cancellation_type::terminal);
    AW_CHECK(token.stop_requested() && called);
    AW_CHECK(bridge->env().get().stop_requested());
    aw::detail::stop_bridge::destroy(bridge);
}

void attachAfterCancellationRequestsImmediately() {
    aw::cancellation_signal parent;
    aw::cancellation_state state{parent.slot()};
    parent.emit(aw::cancellation_type::terminal);
    aw::detail::stop_bridge* bridge = aw::detail::stop_bridge::create();
    bridge->attach(state);
    AW_CHECK(bridge->token().stop_requested());
    aw::detail::stop_bridge::destroy(bridge);
}

void legacyNames() {
    static_assert(std::is_same<decltype(aw::getStopToken()), aw::get_stop_token_t>::value, "");
    static_assert(std::is_same<decltype(aw::get_stop_token()), aw::get_stop_token_t>::value, "");
}

} // namespace

int main() {
    inheritBorrowsOrOwns();
    bridgeTurnsCancellationIntoStopRequest();
    attachAfterCancellationRequestsImmediately();
    legacyNames();
    return 0;
}
```

`tests/unit/CMakeLists.txt` 追加：

```cmake
aw_unit_test(stop_token_adapter_test)
```

- [ ] **Step 2：运行，确认失败**

Run: `cmake --build build/debug --target aw-stop-token-adapter-test 2>&1 | grep -m1 "fatal error"`
Expected: `fatal error: 'awaitable/cancel/stop_token.hpp' file not found`

- [ ] **Step 3：最小实现**

先从夹具生成 `stop_token.hpp`。只改 include 路径和命名空间，其余逐字保留：

Run:

```bash
sed -e 's|#include "co2/config.hpp"|#include "awaitable/core/config.hpp"|' \
    -e 's|^namespace co2 {|namespace aw {|' \
    -e 's|^} // namespace co2|} // namespace aw|' \
    tests/legacy/include/co2/stop_token.hpp > include/awaitable/cancel/stop_token.hpp
diff tests/legacy/include/co2/stop_token.hpp include/awaitable/cancel/stop_token.hpp | grep -c '^>'
```

Expected: `3`

`include/awaitable/cancel/stop_token_adapter.hpp`：

```cpp
#pragma once

#include <new>
#include <type_traits>

#include "awaitable/cancel/cancellation_state.hpp"
#include "awaitable/cancel/stop_token.hpp"
#include "awaitable/core/coroutine_handle.hpp"
#include "awaitable/exec/detail/recycling_allocator.hpp"

namespace aw {

// CO2_AWAIT_SET(token, aw::get_stop_token())：awaitable 帧的 await_transform 处理它（Task 13）。
struct get_stop_token_t {
    using aw_this_coro_tag = void;
};
inline get_stop_token_t get_stop_token() noexcept { return get_stop_token_t{}; }
// coro env.hpp 的旧名，逐字兼容。
inline get_stop_token_t getStopToken() noexcept { return get_stop_token_t{}; }

namespace detail {

// 移植自 coro env.hpp:25-85。
template <class Promise>
auto stop_token_of_promise(Promise& promise, int) noexcept
    -> decltype(static_cast<stop_token>(promise.get_stop_token())) {
    return promise.get_stop_token();
}
template <class Promise> stop_token stop_token_of_promise(Promise&, long) noexcept {
    return stop_token{};
}

template <class Promise>
auto inherit_from_promise(EnvStopToken& into, Promise& awaiting, int) noexcept
    -> decltype(static_cast<EnvStopToken const&>(awaiting.envStopToken()), void()) {
    into.borrow(awaiting.envStopToken());
}
template <class Promise>
void inherit_from_promise(EnvStopToken& into, Promise& awaiting, long) noexcept {
    into.own(stop_token_of_promise(awaiting, 0));
}

template <class Promise>
void inherit_from_handle(EnvStopToken& into, co2::coroutine_handle<Promise> awaiting,
                         std::false_type) noexcept {
    inherit_from_promise(into, awaiting.promise(), 0);
}
template <class Promise>
void inherit_from_handle(EnvStopToken& into, co2::coroutine_handle<Promise>,
                         std::true_type) noexcept {
    into.reset();
}

// §6.4 单向桥：cancellation_state 收到通过 in 过滤器的取消时 request_stop()。
// 桥本身来自 cancellation_signal 标签；stop_source 的停止状态按移植原样用 new 分配
// （只在有人取 token 时发生一次，不在热路径上）。
class stop_bridge {
  public:
    static stop_bridge* create() {
        void* const memory = recycling_allocate(mem_tag::cancellation_signal, sizeof(stop_bridge),
                                                alignof(stop_bridge));
        try {
            return ::new (memory) stop_bridge;
        } catch (...) {
            recycling_deallocate(mem_tag::cancellation_signal, memory, sizeof(stop_bridge));
            throw;
        }
    }

    static void destroy(stop_bridge* bridge) noexcept {
        bridge->~stop_bridge();
        recycling_deallocate(mem_tag::cancellation_signal, bridge, sizeof(stop_bridge));
    }

    void attach(cancellation_state& state) noexcept {
        state.set_observer(&on_cancel, this);
        if (!!state.cancelled()) source_.request_stop();
    }

    stop_token token() const noexcept { return source_.get_token(); }
    EnvStopToken const& env() const noexcept { return env_; }

  private:
    stop_bridge() { env_.own(source_.get_token()); }
    stop_bridge(stop_bridge const&) = delete;
    stop_bridge& operator=(stop_bridge const&) = delete;
    ~stop_bridge() = default;

    static void on_cancel(void* self, cancellation_type) noexcept {
        static_cast<stop_bridge*>(self)->source_.request_stop();
    }

    stop_source source_;
    EnvStopToken env_;
};

} // namespace detail

template <class Promise>
void inherit_stop_token(detail::EnvStopToken& into, co2::coroutine_handle<Promise> awaiting) noexcept {
    detail::inherit_from_handle(into, awaiting, std::is_void<Promise>{});
}

template <class Promise>
void inheritStopToken(detail::EnvStopToken& into, co2::coroutine_handle<Promise> awaiting) noexcept {
    inherit_stop_token(into, awaiting);
}

} // namespace aw
```

- [ ] **Step 4：运行，确认通过**

Run: `cmake --build build/debug -j8 && ctest --test-dir build/debug -R '^aw-stop-token' --output-on-failure`
Expected: `100% tests passed out of 1`

- [ ] **Step 5：提交**

```bash
git add include/awaitable/cancel tests/unit
git commit -m "feat(cancel): port stop_token into aw, stop_token adapter and one-way cancellation bridge"
```

### Task 9：完成 token 基础：async_result / async_initiate / 关联器 / detached / use_future / post / dispatch

**交付物：** §8.1 的定制点与关联器，§8.4 的 `detached`（默认重抛）和 `use_future`，§5.7 的 token 形式 `aw::post` / `aw::dispatch`。

**Files:**
- Create: `include/awaitable/async_result.hpp`, `include/awaitable/associated.hpp`, `include/awaitable/detached.hpp`, `include/awaitable/use_future.hpp`, `include/awaitable/post.hpp`, `include/awaitable/dispatch.hpp`
- Test: `tests/unit/completion_token_test.cpp`
- Modify: `tests/unit/CMakeLists.txt`（追加）

> 位置说明：规格 §2.2 的目录把 post / dispatch 列在 `exec/` 下，但它们依赖 `async_initiate`（spawn 层），放进 exec 会违反 §2.1“exec 只依赖 core”的依赖图。依赖图优先，所以这两个文件放在 `include/awaitable/` 下，见自审第 3 条。

**Interfaces:**
- Consumes（Task 4 / 6 / 7）：executor 概念、`strand`、`cancellation_slot`。
- Produces：
  - 定制点 `template <class Token, class Signature> struct aw::async_result`。默认版本 `static void initiate(Init&&, RawToken&&, Args&&...)` 把 token 原样当回调交给 initiation。
  - `template <class Token, class Signature, class Initiation, class... Args> auto aw::async_initiate(Initiation&& init, std::remove_reference_t<Token>& token, Args&&... args)`。
  - 关联器：
    - `template <class T, class Default> struct aw::associated_executor`，带 `type` 和 `static get(T const&, Default const&)`；
    - `aw::get_associated_executor(t, default_ex)`；
    - `template <class T> struct aw::associated_cancellation_slot`，带 `static cancellation_slot get(T const&)`；
    - `aw::get_associated_cancellation_slot(t)`。
  - 绑定器：
    - `aw::executor_binder<H, E>` 和 `aw::bind_executor(ex, h)`；
    - `aw::cancellation_slot_binder<H>` 和 `aw::bind_cancellation_slot(slot, h)`；
    - 两种绑定器都转发另一种关联。
  - `struct aw::detached_t` 和 `constexpr detached_t aw::detached{}`。处理器收到非空 `std::exception_ptr` 首参时重抛，其余参数一律忽略。
  - `struct aw::use_future_t` 和 `constexpr use_future_t aw::use_future{}`。按签名返回 `std::future<V>`：
    - `void()` 得到 `V = void`；`void(T)` 得到 `V = T`；`void(T1, T2, …)` 得到 `V = std::tuple<T1, T2, …>`；
    - 首参是 `std::error_code` 或 `std::exception_ptr` 时剥掉它：非零的 `error_code` 转成 `std::system_error` 交付，非空的 `exception_ptr` 原样交付；剩下的参数再按上一条确定 V。
  - `template <class Executor, class Token> auto aw::post(Executor const&, Token&&)` 和 `aw::dispatch(Executor const&, Token&&)`，签名 `void()`。

- [ ] **Step 1：写失败的测试**

`tests/unit/completion_token_test.cpp`：

```cpp
#include <future>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

#include "awaitable/associated.hpp"
#include "awaitable/async_result.hpp"
#include "awaitable/cancel/cancellation_signal.hpp"
#include "awaitable/detached.hpp"
#include "awaitable/dispatch.hpp"
#include "awaitable/error.hpp"
#include "awaitable/exec/manual_executor.hpp"
#include "awaitable/exec/strand.hpp"
#include "awaitable/post.hpp"
#include "awaitable/use_future.hpp"
#include "check.hpp"

namespace {

using ex_t = aw::manual_executor::executor_type;

// 示例操作：经回调的关联 executor 交付 (error_code, int)。
struct initiate_add {
    ex_t ex;
    template <class Handler> void operator()(Handler&& handler, int a, int b, bool fail) const {
        auto target = aw::get_associated_executor(handler, ex);
        ex.post([handler = std::forward<Handler>(handler), target, a, b, fail]() mutable {
            std::error_code const ec =
                fail ? std::error_code{aw::error::operation_aborted} : std::error_code{};
            target.dispatch([handler = std::move(handler), ec, a, b]() mutable { handler(ec, a + b); });
        });
    }
};

template <class Token> auto async_add(ex_t ex, int a, int b, bool fail, Token&& token) {
    return aw::async_initiate<Token, void(std::error_code, int)>(initiate_add{ex}, token, a, b, fail);
}

// 同步调用回调的 initiation，用来测 use_future 的各种签名。
template <class Signature> struct initiate_now;
template <class... Ts> struct initiate_now<void(Ts...)> {
    template <class Handler, class... Args> void operator()(Handler&& handler, Args&&... args) const {
        handler(std::forward<Args>(args)...);
    }
};

template <class Signature, class Token, class... Args> auto complete_now(Token&& token, Args&&... args) {
    return aw::async_initiate<Token, Signature>(initiate_now<Signature>{}, token,
                                                std::forward<Args>(args)...);
}

void callbackToken() {
    aw::manual_executor ctx;
    int got = 0;
    std::error_code err;
    async_add(ctx.get_executor(), 2, 3, false, [&](std::error_code ec, int v) {
        err = ec;
        got = v;
    });
    AW_CHECK(got == 0);
    ctx.run();
    AW_CHECK(got == 5 && !err);
}

void useFutureForms() {
    std::future<void> f0 = complete_now<void()>(aw::use_future);
    f0.get();
    std::future<int> f1 = complete_now<void(int)>(aw::use_future, 7);
    AW_CHECK(f1.get() == 7);
    std::future<std::tuple<int, std::string>> f2 =
        complete_now<void(int, std::string)>(aw::use_future, 1, std::string{"x"});
    AW_CHECK((f2.get() == std::make_tuple(1, std::string{"x"})));
    std::future<int> f3 =
        complete_now<void(std::error_code, int)>(aw::use_future, std::error_code{}, 9);
    AW_CHECK(f3.get() == 9);
    std::future<void> f4 = complete_now<void(std::error_code)>(
        aw::use_future, std::error_code{aw::error::operation_aborted});
    bool aborted = false;
    try {
        f4.get();
    } catch (std::system_error const& e) {
        aborted = e.code() == aw::error::operation_aborted;
    }
    AW_CHECK(aborted);
    std::future<int> f5 = complete_now<void(std::exception_ptr, int)>(
        aw::use_future, std::make_exception_ptr(std::runtime_error{"bad"}), 0);
    bool rethrown = false;
    try {
        static_cast<void>(f5.get());
    } catch (std::runtime_error const& e) {
        rethrown = std::string{e.what()} == "bad";
    }
    AW_CHECK(rethrown);
    std::future<std::tuple<int, int>> f6 = complete_now<void(std::exception_ptr, int, int)>(
        aw::use_future, std::exception_ptr{}, 1, 2);
    AW_CHECK((f6.get() == std::make_tuple(1, 2)));
}

void detachedRethrowsExceptionsOnly() {
    complete_now<void(std::error_code, int)>(aw::detached,
                                             std::error_code{aw::error::operation_aborted}, 1);
    complete_now<void()>(aw::detached);
    bool rethrown = false;
    try {
        complete_now<void(std::exception_ptr)>(aw::detached,
                                               std::make_exception_ptr(std::logic_error{"x"}));
    } catch (std::logic_error const&) {
        rethrown = true;
    }
    AW_CHECK(rethrown);
}

void bindExecutorRoutesTheCompletion() {
    aw::manual_executor io;
    aw::manual_executor other;
    bool on_other = false;
    async_add(io.get_executor(), 1, 1, false,
              aw::bind_executor(other.get_executor(), [&](std::error_code, int) {
                  on_other = other.get_executor().running_in_this_thread();
              }));
    io.run();
    AW_CHECK(!on_other && other.pending() == 1U);
    other.run();
    AW_CHECK(on_other);
}

void bindCancellationSlotIsAssociated() {
    aw::cancellation_signal signal;
    auto bound = aw::bind_cancellation_slot(signal.slot(), [](int) {});
    AW_CHECK(aw::get_associated_cancellation_slot(bound) == signal.slot());
    AW_CHECK(!aw::get_associated_cancellation_slot([](int) {}).is_connected());
    aw::manual_executor ctx;
    auto both = aw::bind_executor(ctx.get_executor(), aw::bind_cancellation_slot(signal.slot(), [](int) {}));
    AW_CHECK(aw::get_associated_cancellation_slot(both) == signal.slot());
    AW_CHECK(aw::get_associated_executor(both, ex_t{}) == ctx.get_executor());
}

void postAndDispatchTokens() {
    aw::manual_executor ctx;
    auto ex = ctx.get_executor();
    std::vector<std::string> trace;
    aw::post(ex, [&] { trace.push_back("posted"); });
    aw::dispatch(ex, [&] { trace.push_back("dispatched-from-outside"); });
    AW_CHECK(trace.empty());
    ex.post([&] {
        aw::dispatch(ex, [&] { trace.push_back("inline"); });
        aw::post(ex, [&] { trace.push_back("later"); });
        trace.push_back("end-of-task");
    });
    ctx.run();
    AW_CHECK((trace == std::vector<std::string>{"posted", "dispatched-from-outside", "inline",
                                                "end-of-task", "later"}));
    std::future<void> done = aw::post(ex, aw::use_future);
    ctx.run();
    done.get();
}

void strandAsHandlerExecutor() {
    aw::manual_executor ctx;
    auto s = aw::make_strand(ctx.get_executor());
    bool in_strand = false;
    aw::post(ctx.get_executor(), aw::bind_executor(s, [&] { in_strand = s.running_in_this_thread(); }));
    ctx.run();
    AW_CHECK(in_strand);
}

} // namespace

int main() {
    callbackToken();
    useFutureForms();
    detachedRethrowsExceptionsOnly();
    bindExecutorRoutesTheCompletion();
    bindCancellationSlotIsAssociated();
    postAndDispatchTokens();
    strandAsHandlerExecutor();
    return 0;
}
```

`tests/unit/CMakeLists.txt` 追加：

```cmake
aw_unit_test(completion_token_test)
```

- [ ] **Step 2：运行，确认失败**

Run: `cmake --build build/debug --target aw-completion-token-test 2>&1 | grep -m1 "fatal error"`
Expected: `fatal error: 'awaitable/associated.hpp' file not found`

- [ ] **Step 3：最小实现**

`include/awaitable/async_result.hpp`：

```cpp
#pragma once

#include <type_traits>
#include <utility>

namespace aw {

// §8.1 定制点：默认把 token 原样当回调交给 initiation。
template <class Token, class Signature> struct async_result {
    template <class Initiation, class RawToken, class... Args>
    static void initiate(Initiation&& init, RawToken&& token, Args&&... args) {
        std::forward<Initiation>(init)(std::forward<RawToken>(token), std::forward<Args>(args)...);
    }
};

template <class Token, class Signature, class Initiation, class... Args>
auto async_initiate(Initiation&& init, typename std::remove_reference<Token>::type& token,
                    Args&&... args) {
    return async_result<typename std::decay<Token>::type, Signature>::initiate(
        std::forward<Initiation>(init), static_cast<Token&&>(token), std::forward<Args>(args)...);
}

} // namespace aw
```

`include/awaitable/associated.hpp`：

```cpp
#pragma once

#include <type_traits>
#include <utility>

#include "awaitable/cancel/cancellation_signal.hpp"

namespace aw {

// ---- associated_executor ----

template <class T, class Default, class = void> struct associated_executor_impl {
    using type = Default;
    static type get(T const&, Default const& d) noexcept { return d; }
};
template <class T, class Default>
struct associated_executor_impl<T, Default,
                                decltype(static_cast<void>(std::declval<T const&>().get_executor()),
                                         static_cast<void>(sizeof(typename T::executor_type)))> {
    using type = typename T::executor_type;
    static type get(T const& t, Default const&) noexcept { return t.get_executor(); }
};

template <class T, class Default> struct associated_executor : associated_executor_impl<T, Default> {};

template <class T, class Default>
typename associated_executor<T, Default>::type get_associated_executor(T const& t, Default const& d) {
    return associated_executor<T, Default>::get(t, d);
}

// ---- associated_cancellation_slot ----

template <class T, class = void> struct associated_cancellation_slot_impl {
    static cancellation_slot get(T const&) noexcept { return cancellation_slot{}; }
};
template <class T>
struct associated_cancellation_slot_impl<
    T, decltype(static_cast<void>(std::declval<T const&>().get_cancellation_slot()))> {
    static cancellation_slot get(T const& t) noexcept { return t.get_cancellation_slot(); }
};

template <class T> struct associated_cancellation_slot : associated_cancellation_slot_impl<T> {};

template <class T> cancellation_slot get_associated_cancellation_slot(T const& t) {
    return associated_cancellation_slot<T>::get(t);
}

// ---- 绑定器 ----

template <class Handler, class Executor> class executor_binder {
  public:
    using executor_type = Executor;
    template <class H>
    executor_binder(Executor const& ex, H&& h) : executor_{ex}, handler_(std::forward<H>(h)) {}
    executor_type get_executor() const noexcept { return executor_; }
    Handler const& get() const noexcept { return handler_; }
    template <class... Args> void operator()(Args&&... args) { handler_(std::forward<Args>(args)...); }

  private:
    Executor executor_;
    Handler handler_;
};

template <class Executor, class H> executor_binder<typename std::decay<H>::type, Executor>
bind_executor(Executor const& ex, H&& h) {
    return executor_binder<typename std::decay<H>::type, Executor>{ex, std::forward<H>(h)};
}

template <class Handler> class cancellation_slot_binder {
  public:
    template <class H>
    cancellation_slot_binder(cancellation_slot slot, H&& h) : slot_{slot}, handler_(std::forward<H>(h)) {}
    cancellation_slot get_cancellation_slot() const noexcept { return slot_; }
    Handler const& get() const noexcept { return handler_; }
    template <class... Args> void operator()(Args&&... args) { handler_(std::forward<Args>(args)...); }

  private:
    cancellation_slot slot_;
    Handler handler_;
};

template <class H> cancellation_slot_binder<typename std::decay<H>::type>
bind_cancellation_slot(cancellation_slot slot, H&& h) {
    return cancellation_slot_binder<typename std::decay<H>::type>{slot, std::forward<H>(h)};
}

// 绑定器转发另一种关联。
template <class Handler, class Executor>
struct associated_cancellation_slot<executor_binder<Handler, Executor>> {
    static cancellation_slot get(executor_binder<Handler, Executor> const& b) noexcept {
        return get_associated_cancellation_slot(b.get());
    }
};
template <class Handler, class Default>
struct associated_executor<cancellation_slot_binder<Handler>, Default> {
    using type = typename associated_executor<Handler, Default>::type;
    static type get(cancellation_slot_binder<Handler> const& b, Default const& d) noexcept {
        return associated_executor<Handler, Default>::get(b.get(), d);
    }
};

} // namespace aw
```

`include/awaitable/detached.hpp`：

```cpp
#pragma once

#include <exception>
#include <type_traits>
#include <utility>

#include "awaitable/async_result.hpp"

namespace aw {

struct detached_t {
    constexpr detached_t() noexcept = default;
};
constexpr detached_t detached{};

namespace detail {
// §7.2 第 3 条：非空 exception_ptr 在交付点重抛（相对 asio 的偏离）；其余参数忽略。
struct detached_handler {
    void operator()() const noexcept {}
    template <class... Rest> void operator()(std::exception_ptr const& e, Rest&&...) const {
        if (e) std::rethrow_exception(e);
    }
    template <class First, class... Rest,
              typename std::enable_if<!std::is_same<typename std::decay<First>::type,
                                                    std::exception_ptr>::value,
                                      int>::type = 0>
    void operator()(First&&, Rest&&...) const noexcept {}
};
} // namespace detail

template <class Signature> struct async_result<detached_t, Signature> {
    template <class Initiation, class RawToken, class... Args>
    static void initiate(Initiation&& init, RawToken&&, Args&&... args) {
        std::forward<Initiation>(init)(detail::detached_handler{}, std::forward<Args>(args)...);
    }
};

} // namespace aw
```

`include/awaitable/use_future.hpp`：

```cpp
#pragma once

#include <exception>
#include <future>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

#include "awaitable/async_result.hpp"

namespace aw {

struct use_future_t {
    constexpr use_future_t() noexcept = default;
};
constexpr use_future_t use_future{};

namespace detail {

template <class... Ts> struct future_value { using type = std::tuple<typename std::decay<Ts>::type...>; };
template <> struct future_value<> { using type = void; };
template <class T> struct future_value<T> { using type = typename std::decay<T>::type; };

// lead：0 = 没有错误参数，1 = error_code，2 = exception_ptr。
template <class Signature> struct future_signature;
template <class... Ts> struct future_signature<void(Ts...)> {
    using value_type = typename future_value<Ts...>::type;
    static constexpr int lead = 0;
};
template <class... Ts> struct future_signature<void(std::error_code, Ts...)> {
    using value_type = typename future_value<Ts...>::type;
    static constexpr int lead = 1;
};
template <class... Ts> struct future_signature<void(std::exception_ptr, Ts...)> {
    using value_type = typename future_value<Ts...>::type;
    static constexpr int lead = 2;
};

template <class V> struct promise_setter {
    template <class T> static void set(std::promise<V>& p, T&& v) { p.set_value(std::forward<T>(v)); }
    template <class... Ts, typename std::enable_if<(sizeof...(Ts) > 1U), int>::type = 0>
    static void set(std::promise<V>& p, Ts&&... vs) {
        p.set_value(V(std::forward<Ts>(vs)...));
    }
};
template <> struct promise_setter<void> {
    static void set(std::promise<void>& p) { p.set_value(); }
};

template <class Signature> class use_future_handler {
    using traits = future_signature<Signature>;
    using value_type = typename traits::value_type;

  public:
    use_future_handler() = default;
    std::future<value_type> get_future() { return promise_.get_future(); }

    template <class... Args> void operator()(Args&&... args) {
        deliver(std::integral_constant<int, traits::lead>{}, std::forward<Args>(args)...);
    }

  private:
    template <class... Args> void deliver(std::integral_constant<int, 0>, Args&&... args) {
        promise_setter<value_type>::set(promise_, std::forward<Args>(args)...);
    }
    template <class... Args>
    void deliver(std::integral_constant<int, 1>, std::error_code const& ec, Args&&... args) {
        if (ec) {
            promise_.set_exception(std::make_exception_ptr(std::system_error{ec}));
            return;
        }
        promise_setter<value_type>::set(promise_, std::forward<Args>(args)...);
    }
    template <class... Args>
    void deliver(std::integral_constant<int, 2>, std::exception_ptr const& e, Args&&... args) {
        if (e) {
            promise_.set_exception(e);
            return;
        }
        promise_setter<value_type>::set(promise_, std::forward<Args>(args)...);
    }

    std::promise<value_type> promise_;
};

} // namespace detail

template <class Signature> struct async_result<use_future_t, Signature> {
    template <class Initiation, class RawToken, class... Args>
    static auto initiate(Initiation&& init, RawToken&&, Args&&... args) {
        detail::use_future_handler<Signature> handler;
        auto future = handler.get_future();
        std::forward<Initiation>(init)(std::move(handler), std::forward<Args>(args)...);
        return future;
    }
};

} // namespace aw
```

`include/awaitable/post.hpp`：

```cpp
#pragma once

#include <type_traits>
#include <utility>

#include "awaitable/associated.hpp"
#include "awaitable/async_result.hpp"

namespace aw {
namespace detail {

// asio work_dispatcher：在 ex 上执行，然后派发到回调自己的关联 executor。
template <class Handler, class HandlerExecutor> struct work_dispatcher {
    Handler handler;
    HandlerExecutor handler_ex;
    void operator()() { handler_ex.dispatch(std::move(handler)); }
};

template <class Executor> struct initiate_post {
    Executor ex;
    template <class Handler> void operator()(Handler&& handler) const {
        using handler_type = typename std::decay<Handler>::type;
        auto target = get_associated_executor(handler, ex);
        ex.post(work_dispatcher<handler_type, decltype(target)>{std::forward<Handler>(handler), target});
    }
};

} // namespace detail

template <class Executor, class Token> auto post(Executor const& ex, Token&& token) {
    return async_initiate<Token, void()>(detail::initiate_post<Executor>{ex}, token);
}

} // namespace aw
```

`include/awaitable/dispatch.hpp`：

```cpp
#pragma once

#include <type_traits>
#include <utility>

#include "awaitable/associated.hpp"
#include "awaitable/async_result.hpp"
#include "awaitable/post.hpp"

namespace aw {
namespace detail {

template <class Executor> struct initiate_dispatch {
    Executor ex;
    template <class Handler> void operator()(Handler&& handler) const {
        using handler_type = typename std::decay<Handler>::type;
        auto target = get_associated_executor(handler, ex);
        ex.dispatch(work_dispatcher<handler_type, decltype(target)>{std::forward<Handler>(handler), target});
    }
};

} // namespace detail

template <class Executor, class Token> auto dispatch(Executor const& ex, Token&& token) {
    return async_initiate<Token, void()>(detail::initiate_dispatch<Executor>{ex}, token);
}

} // namespace aw
```

- [ ] **Step 4：运行，确认通过**

Run: `cmake --build build/debug -j8 && ctest --test-dir build/debug -R '^aw-completion-token' --output-on-failure`
Expected: `100% tests passed out of 1`

- [ ] **Step 5：提交**

```bash
git add include/awaitable/async_result.hpp include/awaitable/associated.hpp include/awaitable/detached.hpp \
        include/awaitable/use_future.hpp include/awaitable/post.hpp include/awaitable/dispatch.hpp tests/unit
git commit -m "feat(spawn): async_result/async_initiate, associators and binders, detached, use_future, post/dispatch"
```

### Task 10：awaitable 核心层与 co_spawn

**交付物：** §4.1–§4.4 的 `awaitable<T, E>`、`awaitable_frame_base` / `awaitable_frame`、`awaitable_thread` 和 pump；§7.2 的手写入口帧；§8.2 的 `co_spawn`；以及 `this_coro::executor`。这一版只有子协程等待，异步操作桥接留给 Task 11，取消集成留给 Task 13。

**Files:**
- Create: `include/awaitable/this_coro.hpp`, `include/awaitable/impl/awaitable_frame.hpp`, `include/awaitable/awaitable.hpp`, `include/awaitable/impl/awaitable_thread.hpp`, `include/awaitable/impl/entry_point.hpp`, `include/awaitable/co_spawn.hpp`
- Test: `tests/unit/awaitable_test.cpp`, `tests/unit/await_arbitrary_awaiter_compile_fail_test.cpp`
- Modify: `tests/unit/CMakeLists.txt`（追加）

**Interfaces:**
- Consumes：
  - Task 3：`recycling_allocate`、`launching_scope` / `launching_marker`、`unwinding_scope`；
  - Task 4 / 5：`manual_executor`、`thread_pool`、`is_executor`；
  - Task 7：`cancellation_signal` / `cancellation_slot` / `cancellation_state`；
  - Task 9：`async_initiate`、`get_associated_executor`、`detached`、`use_future`。
- Produces：
  - `template <class T, class Executor> class aw::awaitable`：
    - 类型：`value_type`、`executor_type`、`promise_type`；
    - 构造：可移动、可移动赋值；
    - `valid()`、`await_ready()`、`await_suspend(coroutine_handle<P>)`、`await_resume()`。
  - 别名 `aw::pool_awaitable<T = void>`、`aw::manual_awaitable<T = void>`。
  - `aw::detail::awaitable_frame_base<Executor>`，成员按 §4.2：
    - 帧管理：`push_frame`、`pop_frame`、`attach_thread`、`detach_thread`、`resume`、`after_suspend`、`destroy`；
    - 异常：`rethrow_exception`；
    - 取消：`clear_cancellation_slot`；
    - 入口帧：`entry()`；
    - `await_transform`：接受同 Executor 的 awaitable、带 `aw_awaitable_tag` 的类型和 this_coro 标签，其余一律 `static_assert`；
    - 数据成员：`coro_`、`attached_thread_`、`caller_`、`pending_exception_`、`resume_context_`。
  - `aw::detail::awaitable_frame<T, Executor>`：`get_return_object`、`return_value` 或 `return_void`、`T get()`。
  - `aw::detail::entry_frame_base<Executor>`，数据成员：`block_`、`executor_`、`top_of_stack_`、`internal_signal_`、`parent_slot_`、`cancellation_state_`、`throw_if_cancelled_`、`stop_bridge_`（Task 13 使用，这里先置空）。
  - `aw::detail::awaitable_thread<Executor>`：
    - 类型：`executor_type`；
    - 访问：`get_executor()`、`get_cancellation_slot()`、`entry_point()`、`frame()`；
    - 运行：`launch()`、`pump()`。
  - `aw::detail::entry_frame_impl<Executor, T, Handler>`。
  - `aw::detail::spawn_signature<T>::type`：`void(std::exception_ptr, V)` 或 `void(std::exception_ptr)`，其中 V 是 `T`，T 不可默认构造时是 `aw::optional<T>`。
  - `aw::co_spawn(ex, awaitable<T, E>, token)` 和 `aw::co_spawn(ctx, awaitable<T, typename Context::executor_type>, token)`。
  - `aw::this_coro::executor_t`、`aw::this_coro::executor`。
  - 标签约定：类型带 `using aw_awaitable_tag = void;` 才能在 awaitable 帧里等待（取消检查之后原样等待）；带 `using aw_this_coro_tag = void;` 的是 this_coro 查询。

- [ ] **Step 1：写失败的测试**

`tests/unit/awaitable_test.cpp`：

```cpp
#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "awaitable/awaitable.hpp"
#include "awaitable/co_spawn.hpp"
#include "awaitable/detached.hpp"
#include "awaitable/use_future.hpp"
#include "check.hpp"
#include "counting_new.hpp"

namespace {

using ex_t = aw::manual_executor::executor_type;
template <class T> using task = aw::awaitable<T, ex_t>;

auto value(int v) CO2_BEG(task<int>, (v)) { CO2_RETURN(v); }
CO2_END

auto plus_one(int v) CO2_BEG(task<int>, (v), int r{};) {
    CO2_AWAIT_SET(r, value(v));
    CO2_RETURN(r + 1);
}
CO2_END

auto thrower(std::string message) CO2_BEG(task<int>, (message)) {
    throw std::runtime_error{message};
}
CO2_END

auto rethrows_child() CO2_BEG(task<int>, (), int r{};) {
    CO2_AWAIT_SET(r, thrower("deep"));
    CO2_RETURN(r);
}
CO2_END

auto level3(std::vector<std::string>& t) CO2_BEG(task<void>, (t)) {
    t.push_back("3");
    CO2_RETURN();
}
CO2_END

auto level2(std::vector<std::string>& t) CO2_BEG(task<void>, (t)) {
    t.push_back("2-begin");
    CO2_AWAIT(level3(t));
    t.push_back("2-end");
    CO2_RETURN();
}
CO2_END

auto level1(std::vector<std::string>& t) CO2_BEG(task<void>, (t)) {
    t.push_back("1-begin");
    CO2_AWAIT(level2(t));
    t.push_back("1-end");
    CO2_RETURN();
}
CO2_END

auto deep(int n) CO2_BEG(task<int>, (n), int r{};) {
    if (n == 0) CO2_RETURN(0);
    CO2_AWAIT_SET(r, deep(n - 1));
    CO2_RETURN(r + 1);
}
CO2_END

struct not_default {
    explicit not_default(int x) : v{x} {}
    int v;
};

auto make_not_default(int v) CO2_BEG(task<not_default>, (v)) { CO2_RETURN(not_default{v}); }
CO2_END

auto runs_on_its_executor() CO2_BEG(task<bool>, (), ex_t ex{};) {
    CO2_AWAIT_SET(ex, aw::this_coro::executor);
    CO2_RETURN(ex.running_in_this_thread());
}
CO2_END

auto holds(std::shared_ptr<int> p) CO2_BEG(task<int>, (p)) { CO2_RETURN(*p); }
CO2_END

auto child_loop(int n, long* news)
    CO2_BEG(task<int>, (n, news), int i{}; int sum{}; int r{}; aw_test::allocation_counter* counter{};) {
    for (i = 0; i != n; ++i) {
        if (i == 100) counter = new aw_test::allocation_counter{};
        CO2_AWAIT_SET(r, value(i));
        sum += r;
    }
    *news = counter->news();
    delete counter;
    CO2_RETURN(sum);
}
CO2_END

void spawnDeliversValue() {
    aw::manual_executor ctx;
    int got = 0;
    bool called = false;
    aw::co_spawn(ctx, plus_one(41), [&](std::exception_ptr e, int v) {
        AW_CHECK(!e);
        got = v;
        called = true;
    });
    AW_CHECK(!called); // 不在 run 里：co_spawn 先 dispatch，即排队
    ctx.run();
    AW_CHECK(called && got == 42);
}

void nestedFramesRunInOrder() {
    aw::manual_executor ctx;
    std::vector<std::string> t;
    bool done = false;
    aw::co_spawn(ctx.get_executor(), level1(t), [&](std::exception_ptr e) {
        AW_CHECK(!e);
        done = true;
    });
    ctx.run();
    AW_CHECK(done);
    AW_CHECK((t == std::vector<std::string>{"1-begin", "2-begin", "3", "2-end", "1-end"}));
}

void exceptionsTravelToTheHandler() {
    aw::manual_executor ctx;
    std::string message;
    int v = -1;
    aw::co_spawn(ctx, rethrows_child(), [&](std::exception_ptr e, int value) {
        v = value;
        try {
            std::rethrow_exception(e);
        } catch (std::runtime_error const& error) {
            message = error.what();
        }
    });
    ctx.run();
    AW_CHECK(message == "deep" && v == 0);
}

void deepRecursionIsIterative() {
    aw::manual_executor ctx;
    int got = 0;
    aw::co_spawn(ctx, deep(10000), [&](std::exception_ptr, int v) { got = v; });
    ctx.run();
    AW_CHECK(got == 10000);
}

void nonDefaultConstructibleResultUsesOptional() {
    aw::manual_executor ctx;
    int got = 0;
    aw::co_spawn(ctx, make_not_default(7), [&](std::exception_ptr e, aw::optional<not_default> v) {
        AW_CHECK(!e && v.has_value());
        got = v->v;
    });
    ctx.run();
    AW_CHECK(got == 7);
}

void thisCoroExecutor() {
    aw::manual_executor ctx;
    bool on_it = false;
    aw::co_spawn(ctx, runs_on_its_executor(), [&](std::exception_ptr, bool b) { on_it = b; });
    ctx.run();
    AW_CHECK(on_it);
}

void launchTimeCompletionIsNotDeliveredInline() {
    aw::manual_executor ctx;
    auto ex = ctx.get_executor();
    bool delivered = false;
    bool checked = false;
    ex.post([&] {
        aw::co_spawn(ex, value(1), [&](std::exception_ptr, int) { delivered = true; });
        AW_CHECK(!delivered); // §7.3 第 4 条：启动期同步完成，交付先 post
        checked = true;
    });
    ctx.run();
    AW_CHECK(checked && delivered);
}

void detachedRethrowsFromRun() {
    aw::manual_executor ctx;
    aw::co_spawn(ctx, thrower("detached"), aw::detached);
    bool caught = false;
    try {
        ctx.run();
    } catch (std::runtime_error const& e) {
        caught = std::string{e.what()} == "detached";
    }
    AW_CHECK(caught);
}

void handlerExceptionsEscapeRun() {
    aw::manual_executor ctx;
    aw::co_spawn(ctx, value(1), [](std::exception_ptr, int) { throw std::logic_error{"handler"}; });
    bool caught = false;
    try {
        ctx.run();
    } catch (std::logic_error const&) {
        caught = true;
    }
    AW_CHECK(caught);
}

void useFutureToken() {
    aw::manual_executor ctx;
    std::future<int> f = aw::co_spawn(ctx, plus_one(1), aw::use_future);
    ctx.run();
    AW_CHECK(f.get() == 2);
}

void abandonedThreadsAreUnwound() {
    auto p = std::make_shared<int>(5);
    {
        aw::manual_executor ctx;
        aw::co_spawn(ctx, holds(p), [](std::exception_ptr, int) { AW_CHECK(false); });
        AW_CHECK(p.use_count() == 2);
    } // 没有 run：launch 操作被销毁，执行线程经 executor 展开（关闭期间内联）
    AW_CHECK(p.use_count() == 1);
}

void childFramesReuseRecycledMemory() {
    aw::manual_executor ctx;
    long news = -1;
    int sum = 0;
    aw::co_spawn(ctx, child_loop(1100, &news), [&](std::exception_ptr e, int s) {
        AW_CHECK(!e);
        sum = s;
    });
    ctx.run();
    AW_CHECK(sum == 1099 * 1100 / 2);
    if (aw_test::counting_new_enabled) AW_CHECK(news == 0);
}

auto pool_plus_one(int v) CO2_BEG(aw::pool_awaitable<int>, (v)) { CO2_RETURN(v + 1); }
CO2_END

void runsOnThreadPool() {
    aw::thread_pool pool{2};
    std::future<int> f = aw::co_spawn(pool, pool_plus_one(9), aw::use_future);
    AW_CHECK(f.get() == 10);
}

} // namespace

int main() {
    spawnDeliversValue();
    nestedFramesRunInOrder();
    exceptionsTravelToTheHandler();
    deepRecursionIsIterative();
    nonDefaultConstructibleResultUsesOptional();
    thisCoroExecutor();
    launchTimeCompletionIsNotDeliveredInline();
    detachedRethrowsFromRun();
    handlerExceptionsEscapeRun();
    useFutureToken();
    abandonedThreadsAreUnwound();
    childFramesReuseRecycledMemory();
    runsOnThreadPool();
    return 0;
}
```

`tests/unit/await_arbitrary_awaiter_compile_fail_test.cpp`：

```cpp
// 编译失败测试（§4.3 第 5 条、§14.4）：awaitable 帧里等待任意 awaiter 必须编译失败。
// 定义 AW_EXPECT_COMPILE_FAILURE 时本文件必须编不过；不定义时是对照，必须能编过。
#include "awaitable/awaitable.hpp"

using ex_t = aw::manual_executor::executor_type;

auto awaits_something() CO2_BEG((aw::awaitable<void, ex_t>), ()) {
#ifdef AW_EXPECT_COMPILE_FAILURE
    CO2_AWAIT(co2::suspend_always{});
#endif
    CO2_RETURN();
}
CO2_END
```

`tests/unit/CMakeLists.txt` 追加：

```cmake
aw_unit_test(awaitable_test)
aw_compile_fail_test(await_arbitrary_awaiter_compile_fail_test)
```

- [ ] **Step 2：运行，确认失败**

Run: `cmake --build build/debug --target aw-awaitable-test 2>&1 | grep -m1 "fatal error"`
Expected: `fatal error: 'awaitable/awaitable.hpp' file not found`

- [ ] **Step 3：最小实现**

`include/awaitable/this_coro.hpp`：

```cpp
#pragma once

namespace aw {
namespace this_coro {

// CO2_AWAIT_SET(ex, aw::this_coro::executor);
struct executor_t {
    using aw_this_coro_tag = void;
    constexpr executor_t() noexcept = default;
};
constexpr executor_t executor{};

} // namespace this_coro
} // namespace aw
```

`include/awaitable/impl/awaitable_frame.hpp`：

```cpp
#pragma once

#include <cstddef>
#include <exception>
#include <system_error>
#include <type_traits>
#include <utility>

#include "awaitable/core/coroutine.hpp"
#include "awaitable/error.hpp"
#include "awaitable/exec/detail/recycling_allocator.hpp"
#include "awaitable/optional.hpp"
#include "awaitable/this_coro.hpp"

namespace aw {

template <class T, class Executor> class awaitable;

namespace detail {

template <class Executor> class awaitable_thread;
template <class Executor> class entry_frame_base;
template <class Executor, class T, class Handler> class entry_frame_impl;

// 帧 resume() 在栈上放一个，after_suspend 写进去（asio impl/awaitable.hpp:497-516）。
struct resume_context {
    void (*after_suspend_fn)(void*){nullptr};
    void* after_suspend_arg{nullptr};
};

template <class T> struct dependent_false : std::false_type {};
template <class...> struct make_void { using type = void; };

template <class T, class = void> struct has_aw_awaitable_tag : std::false_type {};
template <class T>
struct has_aw_awaitable_tag<T, typename make_void<typename T::aw_awaitable_tag>::type>
    : std::true_type {};

template <class T, class = void> struct has_aw_this_coro_tag : std::false_type {};
template <class T>
struct has_aw_this_coro_tag<T, typename make_void<typename T::aw_this_coro_tag>::type>
    : std::true_type {};

template <class T, class Executor> struct is_awaitable_of : std::false_type {};
template <class U, class Executor>
struct is_awaitable_of<awaitable<U, Executor>, Executor> : std::true_type {};

template <class Executor> class awaitable_frame_base {
  public:
    using executor_type = Executor;

    awaitable_frame_base() noexcept = default;
    awaitable_frame_base(awaitable_frame_base const&) = delete;
    awaitable_frame_base& operator=(awaitable_frame_base const&) = delete;

    // §9.4：core 的 FrameMemory 优先用 promise 的 operator new(size_t)。
    static void* operator new(std::size_t size) {
        return recycling_allocate(mem_tag::awaitable_frame, size);
    }
    static void operator delete(void* pointer, std::size_t size) noexcept {
        recycling_deallocate(mem_tag::awaitable_frame, pointer, size);
    }

    co2::suspend_always initial_suspend() const noexcept { return {}; }

    struct final_awaiter {
        awaitable_frame_base* self;
        bool await_ready() const noexcept { return false; }
        void await_suspend(co2::coroutine_handle<>) noexcept { self->pop_frame(); }
        void await_resume() const noexcept {}
    };
    final_awaiter final_suspend() noexcept { return final_awaiter{this}; }

    void unhandled_exception() noexcept { pending_exception_ = std::current_exception(); }

    void rethrow_exception() {
        if (pending_exception_) {
            std::exception_ptr e = std::move(pending_exception_);
            pending_exception_ = nullptr;
            std::rethrow_exception(e);
        }
    }

    // asio impl/awaitable.hpp:480-495。
    void push_frame(awaitable_frame_base* caller) noexcept {
        caller_ = caller;
        attached_thread_ = caller_->attached_thread_;
        attached_thread_->entry_point()->top_of_stack_ = this;
        caller_->attached_thread_ = nullptr;
    }

    void pop_frame() noexcept {
        if (caller_ != nullptr) caller_->attached_thread_ = attached_thread_;
        attached_thread_->entry_point()->top_of_stack_ = caller_;
        attached_thread_ = nullptr;
        caller_ = nullptr;
    }

    void attach_thread(awaitable_thread<Executor>* thread) noexcept { attached_thread_ = thread; }

    awaitable_thread<Executor>* detach_thread() noexcept {
        awaitable_thread<Executor>* const thread = attached_thread_;
        attached_thread_ = nullptr;
        return thread;
    }

    void resume() {
        resume_context context;
        resume_context_ = &context;
        coro_.resume();
        // 帧可能已在别的线程上继续运行：此后只读栈上的 context。
        if (context.after_suspend_fn != nullptr) context.after_suspend_fn(context.after_suspend_arg);
    }

    void after_suspend(void (*fn)(void*), void* arg) noexcept {
        resume_context_->after_suspend_fn = fn;
        resume_context_->after_suspend_arg = arg;
    }

    entry_frame_base<Executor>* entry() const noexcept { return attached_thread_->entry_point(); }

    void clear_cancellation_slot() { entry()->cancellation_state_.slot().clear(); }

    void destroy() noexcept { coro_.destroy(); }

    // ---- §4.3 await_transform ----

    template <class U>
    awaitable<U, Executor> await_transform(awaitable<U, Executor>&& child) const {
        throw_if_cancelled_now();
        return std::move(child);
    }

    template <class Op, typename std::enable_if<
                            has_aw_awaitable_tag<typename std::decay<Op>::type>::value, int>::type = 0>
    Op&& await_transform(Op&& op) const {
        throw_if_cancelled_now();
        return std::forward<Op>(op);
    }

    struct executor_awaiter {
        Executor ex;
        bool await_ready() const noexcept { return true; }
        void await_suspend(co2::coroutine_handle<>) const noexcept {}
        Executor await_resume() const noexcept { return ex; }
    };
    executor_awaiter await_transform(this_coro::executor_t) const noexcept {
        return executor_awaiter{entry()->executor_};
    }

    template <class Other,
              typename std::enable_if<
                  !is_awaitable_of<typename std::decay<Other>::type, Executor>::value &&
                      !has_aw_awaitable_tag<typename std::decay<Other>::type>::value &&
                      !has_aw_this_coro_tag<typename std::decay<Other>::type>::value,
                  int>::type = 0>
    co2::suspend_never await_transform(Other&&) const {
        static_assert(dependent_false<Other>::value,
                      "an aw::awaitable frame can only await aw::awaitable (same executor), "
                      "use_awaitable operations, this_coro queries and async_generator::next()");
        return {};
    }

    void throw_if_cancelled_now() const {
        entry_frame_base<Executor>* const e = entry();
        if (e->throw_if_cancelled_ && !!e->cancellation_state_.cancelled())
            throw std::system_error{std::error_code{error::operation_aborted}, "co_await"};
    }

    // ---- 数据成员（asio :523-528）----
    co2::coroutine_handle<> coro_{};
    awaitable_thread<Executor>* attached_thread_{nullptr};
    awaitable_frame_base* caller_{nullptr};
    std::exception_ptr pending_exception_{};
    resume_context* resume_context_{nullptr};
};

template <class T> struct frame_result {
    aw::optional<T> value;
    template <class U> void set(U&& u) { value.emplace(std::forward<U>(u)); }
    T take() { return std::move(*value); }
};
template <class T> struct frame_result<T&> {
    T* value{nullptr};
    void set(T& u) noexcept { value = &u; }
    T& take() noexcept { return *value; }
};

template <class T, class Executor> class awaitable_frame : public awaitable_frame_base<Executor> {
  public:
    awaitable<T, Executor> get_return_object() noexcept;

    template <class U> void return_value(U&& v) { result_.set(std::forward<U>(v)); }

    T get() {
        this->caller_ = nullptr;
        this->rethrow_exception();
        return result_.take();
    }

  private:
    frame_result<T> result_;
};

template <class Executor>
class awaitable_frame<void, Executor> : public awaitable_frame_base<Executor> {
  public:
    awaitable<void, Executor> get_return_object() noexcept;

    void return_void() noexcept {}

    void get() {
        this->caller_ = nullptr;
        this->rethrow_exception();
    }
};

} // namespace detail
} // namespace aw
```

`include/awaitable/awaitable.hpp`：

```cpp
#pragma once

#include <utility>

#include "awaitable/core/dsl.hpp"
#include "awaitable/exec/manual_executor.hpp"
#include "awaitable/exec/thread_pool.hpp"
#include "awaitable/impl/awaitable_frame.hpp"

namespace aw {

// §4.1：只能移动；只能在同一 Executor 的 awaitable 帧里等待（await_transform 保证）。
template <class T, class Executor> class awaitable {
  public:
    using value_type = T;
    using executor_type = Executor;
    using promise_type = detail::awaitable_frame<T, Executor>;

    constexpr awaitable() noexcept = default;
    awaitable(awaitable&& other) noexcept : frame_{other.frame_} { other.frame_ = nullptr; }
    awaitable& operator=(awaitable&& other) noexcept {
        if (this != &other) {
            reset();
            frame_ = other.frame_;
            other.frame_ = nullptr;
        }
        return *this;
    }
    awaitable(awaitable const&) = delete;
    awaitable& operator=(awaitable const&) = delete;
    ~awaitable() { reset(); }

    bool valid() const noexcept { return frame_ != nullptr; }

    bool await_ready() const noexcept { return false; }

    template <class P> void await_suspend(co2::coroutine_handle<P> caller) noexcept {
        frame_->push_frame(&caller.promise());
    }

    // asio awaitable.hpp:125-129：移进临时对象，表达式结束时销毁子帧。
    T await_resume() { return awaitable(static_cast<awaitable&&>(*this)).frame_->get(); }

  private:
    template <class, class> friend class detail::awaitable_frame;
    template <class, class, class> friend class detail::entry_frame_impl;

    explicit awaitable(detail::awaitable_frame<T, Executor>* frame) noexcept : frame_{frame} {}

    void reset() noexcept {
        if (frame_ != nullptr) {
            frame_->destroy();
            frame_ = nullptr;
        }
    }

    detail::awaitable_frame<T, Executor>* frame_{nullptr};
};

namespace detail {

template <class T, class Executor>
awaitable<T, Executor> awaitable_frame<T, Executor>::get_return_object() noexcept {
    this->coro_ = co2::coroutine_handle<awaitable_frame>::from_promise(*this);
    return awaitable<T, Executor>{this};
}

template <class Executor>
awaitable<void, Executor> awaitable_frame<void, Executor>::get_return_object() noexcept {
    this->coro_ = co2::coroutine_handle<awaitable_frame>::from_promise(*this);
    return awaitable<void, Executor>{this};
}

} // namespace detail

// §16 D4：Executor 没有默认值，提供常用别名。
template <class T = void> using pool_awaitable = awaitable<T, thread_pool::executor_type>;
template <class T = void> using manual_awaitable = awaitable<T, manual_executor::executor_type>;

} // namespace aw

#include "awaitable/impl/awaitable_thread.hpp"
```

`include/awaitable/impl/awaitable_thread.hpp`：

```cpp
#pragma once

#include <exception>
#include <utility>

#include "awaitable/cancel/cancellation_signal.hpp"
#include "awaitable/cancel/cancellation_state.hpp"
#include "awaitable/core/coroutine_handle.hpp"
#include "awaitable/exec/detail/thread_context.hpp"
#include "awaitable/impl/awaitable_frame.hpp"

namespace aw {
namespace detail {

class stop_bridge;

// 入口帧（栈底）。帧头与 core 兼容，可被 pump 恢复；状态机由 entry_frame_impl 实现。
template <class Executor> class entry_frame_base : public awaitable_frame_base<Executor> {
  public:
    struct header_block {
        co2::detail::FrameHeader header;
        entry_frame_base* self;
    };

    explicit entry_frame_base(Executor const& ex) : executor_{ex} {
        block_.self = this;
        this->coro_ = co2::coroutine_handle<>::from_address(&block_.header);
        top_of_stack_ = this;
        parent_slot_ = internal_signal_.slot();
        cancellation_state_ = cancellation_state{parent_slot_};
    }

    header_block block_{};
    Executor executor_;
    awaitable_frame_base<Executor>* top_of_stack_{nullptr};
    cancellation_signal internal_signal_;
    cancellation_slot parent_slot_;
    cancellation_state cancellation_state_;
    bool throw_if_cancelled_{true};
    stop_bridge* stop_bridge_{nullptr};
};

// §4.4：执行线程。只持有栈底；所有权在 pump 和完成回调之间转移。
template <class Executor> class awaitable_thread {
  public:
    using executor_type = Executor;

    explicit awaitable_thread(entry_frame_base<Executor>* entry) noexcept : bottom_of_stack_{entry} {}
    awaitable_thread(awaitable_thread&& other) noexcept : bottom_of_stack_{other.bottom_of_stack_} {
        other.bottom_of_stack_ = nullptr;
    }
    awaitable_thread(awaitable_thread const&) = delete;
    awaitable_thread& operator=(awaitable_thread const&) = delete;
    awaitable_thread& operator=(awaitable_thread&&) = delete;
    ~awaitable_thread() {
        if (bottom_of_stack_ != nullptr) unwind_through_executor();
    }

    executor_type get_executor() const noexcept { return bottom_of_stack_->executor_; }
    cancellation_slot get_cancellation_slot() const noexcept {
        return bottom_of_stack_->cancellation_state_.slot();
    }
    entry_frame_base<Executor>* entry_point() const noexcept { return bottom_of_stack_; }
    awaitable_frame_base<Executor>* frame() const noexcept { return bottom_of_stack_->top_of_stack_; }

    // asio :753-757 与 impl/awaitable.ipp:31-43：在“启动中”标记下 pump。
    void launch() {
        launching_scope scope{static_cast<void const*>(bottom_of_stack_)};
        frame()->attach_thread(this);
        pump();
    }

    // asio :778-786。
    void pump() {
        do {
            bottom_of_stack_->top_of_stack_->resume();
        } while (bottom_of_stack_ != nullptr && bottom_of_stack_->top_of_stack_ != nullptr);
        if (bottom_of_stack_ != nullptr) complete();
    }

  private:
    // asio :794-799：销毁入口帧；交付回调时抛出的异常在这里重抛（§7.5）。
    void complete() {
        entry_frame_base<Executor>* const entry = bottom_of_stack_;
        bottom_of_stack_ = nullptr;
        std::exception_ptr e = std::move(entry->pending_exception_);
        entry->destroy();
        if (e) std::rethrow_exception(e);
    }

    struct entry_owner {
        entry_frame_base<Executor>* entry;
        explicit entry_owner(entry_frame_base<Executor>* e) noexcept : entry{e} {}
        entry_owner(entry_owner&& other) noexcept : entry{other.entry} { other.entry = nullptr; }
        entry_owner(entry_owner const&) = delete;
        entry_owner& operator=(entry_owner const&) = delete;
        ~entry_owner() {
            if (entry != nullptr) {
                unwinding_scope scope;
                entry->destroy();
            }
        }
        void operator()() noexcept {}
    };

    // asio :763-774：被丢弃的执行线程经 executor 展开（关闭期间 post 立即销毁，即内联展开）。
    void unwind_through_executor() noexcept {
        entry_frame_base<Executor>* const entry = bottom_of_stack_;
        bottom_of_stack_ = nullptr;
        Executor ex = entry->executor_;
        ex.post(entry_owner{entry});
    }

    entry_frame_base<Executor>* bottom_of_stack_;
};

// 投递一个执行线程：在 executor 上重新 attach 并 pump。
template <class Executor> struct resume_thread_op {
    awaitable_thread<Executor> thread;
    void operator()() {
        thread.frame()->attach_thread(&thread);
        thread.pump();
    }
};

} // namespace detail
} // namespace aw
```

`include/awaitable/impl/entry_point.hpp`：

```cpp
#pragma once

#include <exception>
#include <new>
#include <type_traits>
#include <utility>

#include "awaitable/associated.hpp"
#include "awaitable/awaitable.hpp"
#include "awaitable/core/contract.hpp"
#include "awaitable/exec/detail/recycling_allocator.hpp"
#include "awaitable/exec/detail/thread_context.hpp"
#include "awaitable/optional.hpp"

namespace aw {
namespace detail {

template <class T> struct spawn_value {
    using type = typename std::conditional<std::is_default_constructible<T>::value, T,
                                           aw::optional<T>>::type;
};
template <class T> using spawn_value_t = typename spawn_value<T>::type;

template <class T> struct spawn_signature { using type = void(std::exception_ptr, spawn_value_t<T>); };
template <> struct spawn_signature<void> { using type = void(std::exception_ptr); };

template <class T> struct spawn_result {
    using value_type = spawn_value_t<T>;
    aw::optional<T> value;
    template <class F> void capture(F&& get) { value.emplace(get()); }
    value_type take() {
        if (value) return value_type(std::move(*value));
        return value_type();
    }
};

template <class Handler, class V> struct deliver_op {
    Handler handler;
    std::exception_ptr e;
    V v;
    void operator()() { handler(e, std::move(v)); }
};
template <class Handler> struct deliver_void_op {
    Handler handler;
    std::exception_ptr e;
    void operator()() { handler(e); }
};

// §7.2：手写入口帧。状态机：开始（压入用户帧）→ 运行中 → [启动期完成：先 post] → 交付。
template <class Executor, class T, class Handler>
class entry_frame_impl final : public entry_frame_base<Executor> {
    using base = entry_frame_base<Executor>;

  public:
    template <class H>
    static entry_frame_impl* create(Executor const& ex, awaitable<T, Executor> user, H&& handler) {
        void* const memory = recycling_allocate(mem_tag::awaitable_frame, sizeof(entry_frame_impl),
                                                alignof(entry_frame_impl));
        try {
            return ::new (memory) entry_frame_impl(ex, std::move(user), std::forward<H>(handler));
        } catch (...) {
            recycling_deallocate(mem_tag::awaitable_frame, memory, sizeof(entry_frame_impl));
            throw;
        }
    }

  private:
    enum class state : unsigned char { start, running, deliver };

    template <class H>
    entry_frame_impl(Executor const& ex, awaitable<T, Executor> user, H&& handler)
        : base{ex}, user_{std::move(user)}, handler_(std::forward<H>(handler)) {
        this->block_.header = co2::detail::FrameHeader{&destroy_entry, &step_entry, false, false};
    }
    ~entry_frame_impl() = default;

    static entry_frame_impl* from(co2::detail::FrameHeader* header) noexcept {
        return static_cast<entry_frame_impl*>(
            reinterpret_cast<typename base::header_block*>(header)->self);
    }

    static void destroy_entry(co2::detail::FrameHeader* header) noexcept {
        entry_frame_impl* const self = from(header);
        self->~entry_frame_impl();
        recycling_deallocate(mem_tag::awaitable_frame, self, sizeof(entry_frame_impl));
    }

    static co2::detail::FrameHeader* step_entry(co2::detail::FrameHeader* header) {
        from(header)->step();
        return nullptr;
    }

    void step() {
        switch (state_) {
        case state::start:
            CO2_CONTRACT_CHECK(user_.valid());
            state_ = state::running;
            user_.frame_->push_frame(this);
            return;
        case state::running:
            if (launching_marker() == static_cast<void const*>(static_cast<base*>(this))) {
                state_ = state::deliver;
                this->after_suspend(&reschedule_via_post, this);
                return;
            }
            deliver();
            return;
        case state::deliver:
            deliver();
            return;
        }
    }

    // §7.3 第 4 条：启动期同步完成，先 post 回 executor，再交付。
    static void reschedule_via_post(void* arg) {
        entry_frame_impl* const self = static_cast<entry_frame_impl*>(arg);
        awaitable_thread<Executor>* const thread = self->detach_thread();
        Executor ex = self->executor_;
        ex.post(resume_thread_op<Executor>{awaitable_thread<Executor>{std::move(*thread)}});
    }

    void deliver() { deliver(std::is_void<T>{}); }

    void deliver(std::false_type) {
        std::exception_ptr e;
        spawn_result<T> result;
        {
            awaitable<T, Executor> done{std::move(user_)};
            try {
                result.capture([&]() -> T { return done.frame_->get(); });
            } catch (...) {
                e = std::current_exception();
            }
        } // 用户帧在交付前归还缓存
        using op_type = deliver_op<Handler, spawn_value_t<T>>;
        dispatch_to_handler(op_type{std::move(handler_), e, result.take()});
    }

    void deliver(std::true_type) {
        std::exception_ptr e;
        {
            awaitable<T, Executor> done{std::move(user_)};
            try {
                done.frame_->get();
            } catch (...) {
                e = std::current_exception();
            }
        }
        dispatch_to_handler(deliver_void_op<Handler>{std::move(handler_), e});
    }

    // §7.3 第 5 条：交付到 token 的关联 executor；回调抛出的异常经 pump 的 complete 重抛。
    template <class Op> void dispatch_to_handler(Op&& op) {
        try {
            auto target = get_associated_executor(op.handler, this->executor_);
            target.dispatch(std::forward<Op>(op));
        } catch (...) {
            this->pending_exception_ = std::current_exception();
        }
        this->pop_frame();
    }

    awaitable<T, Executor> user_;
    Handler handler_;
    state state_{state::start};
};

template <class Executor> struct launch_op {
    awaitable_thread<Executor> thread;
    void operator()() { thread.launch(); }
};

template <class Executor> struct initiate_co_spawn {
    Executor ex;

    template <class Handler, class T> void operator()(Handler&& handler, awaitable<T, Executor> a) const {
        using handler_type = typename std::decay<Handler>::type;
        auto* const entry = entry_frame_impl<Executor, T, handler_type>::create(
            ex, std::move(a), std::forward<Handler>(handler));
        awaitable_thread<Executor> thread{entry};
        // §7.3 第 3 条：先 dispatch 到 ex，再 launch。
        ex.dispatch(launch_op<Executor>{std::move(thread)});
    }
};

} // namespace detail
} // namespace aw
```

`include/awaitable/co_spawn.hpp`：

```cpp
#pragma once

#include <type_traits>
#include <utility>

#include "awaitable/async_result.hpp"
#include "awaitable/awaitable.hpp"
#include "awaitable/exec/executor.hpp"
#include "awaitable/impl/entry_point.hpp"

namespace aw {

// §8.2：只接受 awaitable 对象；完成签名 void(exception_ptr, T) 或 void(exception_ptr)。
template <class Executor, class T, class Token,
          typename std::enable_if<is_executor<Executor>::value, int>::type = 0>
auto co_spawn(Executor const& ex, awaitable<T, Executor> a, Token&& token) {
    static_assert(!std::is_reference<T>::value,
                  "co_spawn: the awaitable must return void or an object type");
    return async_initiate<Token, typename detail::spawn_signature<T>::type>(
        detail::initiate_co_spawn<Executor>{ex}, token, std::move(a));
}

template <class Context, class T, class Token,
          typename std::enable_if<!is_executor<Context>::value, int>::type = 0>
auto co_spawn(Context& ctx, awaitable<T, typename Context::executor_type> a, Token&& token) {
    return co_spawn(ctx.get_executor(), std::move(a), std::forward<Token>(token));
}

} // namespace aw
```

- [ ] **Step 4：运行，确认通过（含 compile-fail 与 ASan）**

Run: `cmake --build build/debug -j8 && ctest --test-dir build/debug -R '^aw-(awaitable|await-arbitrary)' --output-on-failure && tools/run-matrix.sh asan-cxx14`
Expected：先输出 `100% tests passed out of 2`，再输出 `asan-cxx14: 100% tests passed out of 41`。

- [ ] **Step 5：提交**

```bash
git add include/awaitable tests/unit
git commit -m "feat(core): awaitable, frame stack, awaitable_thread pump, hand-written entry frame, co_spawn"
```

### Task 11：use_awaitable 与异步操作桥接

**交付物：** §8.3 的 `use_awaitable` 完成令牌，以及直接 awaiter `async_op_awaiter`：不像旧版 asio 那样再建一个 initiate 协程帧。
- 时序：`await_suspend` 只调用 `after_suspend(&on_suspend, this)`；`on_suspend` 先 `detach_thread` 交给回调，再调用 initiation。
- 完成：回调把参数地址写进 `result_`，然后 `attach_thread`、`clear_cancellation_slot`，在自己的栈内 `pump()`。
- 同步完成：initiation 在 `on_suspend` 返回之前就调用了回调时，回调不在栈内 pump，而是把执行线程 `post` 到它的 executor，不递归。
- awaiter 放进帧的 64 B awaiter 槽，同线程稳态零分配（§15.4 第 4 条）。

**Files:**
- Create: `include/awaitable/impl/async_op_awaiter.hpp`, `include/awaitable/use_awaitable.hpp`
- Test: `tests/support/test_ops.hpp`, `tests/unit/use_awaitable_test.cpp`
- Modify: `tests/unit/CMakeLists.txt`（追加）

**Interfaces:**
- Consumes：
  - Task 9：`async_result`、`async_initiate`、`get_associated_executor`、`get_associated_cancellation_slot`；
  - Task 10：`awaitable_thread<Executor>`、`awaitable_frame_base<Executor>::after_suspend`、`resume_context_`、`aw_awaitable_tag` 约定；
  - Task 3：`recycling_allocate`（`mem_tag::default_tag`）。
- Produces：
  - `template <class Executor = void> struct aw::use_awaitable_t`，常量 `aw::use_awaitable`；
  - `aw::async_result<use_awaitable_t<E>, void(Ts...)>::initiate(init, token, args...)` 返回 `async_op_awaiter<void(Ts...), E, Init, Args...>`；
  - `aw::detail::async_op_awaiter<void(Ts...), RequiredExecutor, Initiation, Args...>`：只可移动，带 `aw_awaitable_tag`，`await_resume()` 的结果按 §7.1 第 4 条和 §8.1（`error_code` 非零时抛 `std::system_error`）：首参数是 `std::error_code` 或 `std::exception_ptr` 时非空则抛出并去掉；剩余 0 / 1 / N 个值分别得到 `void` / `T` / `std::tuple<...>`；
  - `aw::detail::awaitable_async_op_handler<Executor, void(Ts...)>`：派生自 `awaitable_thread<Executor>`，`operator()(Ts...)` 保存结果并恢复线程；
  - 测试支持 `tests/support/test_ops.hpp`（命名空间 `aw_test`）：
    - `async_value(io_ex, v, token)`、`async_error(io_ex, ec, token)`、`async_exception(io_ex, e, token)`、`async_nothing(io_ex, token)`、`async_pair(io_ex, a, b, token)`：在 `io_ex` 上稍后完成；
    - `async_immediate(trace, v, token)`：在 initiation 内同步完成，并把顺序记入 `trace`；
    - `foreign_threads` 与 `async_from_foreign_thread(threads, v, token)`：在非库线程上完成。

- [ ] **Step 1：写失败的测试**

`tests/support/test_ops.hpp`：

```cpp
#pragma once

// 测试用异步操作。全部遵守 §8.1 的操作作者契约：完成时经回调的关联 executor 调用回调。

#include <exception>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "awaitable/associated.hpp"
#include "awaitable/async_result.hpp"

namespace aw_test {

// 把回调和参数绑成一个无参可调用对象。
template <class Handler, class... Vs> struct bound_completion {
    Handler handler;
    std::tuple<Vs...> values;
    void operator()() { call(std::index_sequence_for<Vs...>{}); }
    template <std::size_t... I> void call(std::index_sequence<I...>) {
        handler(std::move(std::get<I>(values))...);
    }
};

template <class Handler, class... Vs>
bound_completion<typename std::decay<Handler>::type, typename std::decay<Vs>::type...>
bind_completion(Handler&& handler, Vs&&... values) {
    return {std::forward<Handler>(handler),
            std::tuple<typename std::decay<Vs>::type...>{std::forward<Vs>(values)...}};
}

// 在 io_ex 上执行，然后 dispatch 到回调的关联 executor（asio 的 post + work_dispatcher）。
template <class Target, class Completion> struct dispatch_to {
    Target target;
    Completion completion;
    void operator()() { target.dispatch(std::move(completion)); }
};

template <class Executor> struct initiate_complete_later {
    Executor io_ex;
    template <class Handler, class... Vs> void operator()(Handler&& handler, Vs&&... values) const {
        auto target = aw::get_associated_executor(handler, io_ex);
        auto completion = bind_completion(std::forward<Handler>(handler), std::forward<Vs>(values)...);
        io_ex.post(dispatch_to<decltype(target), decltype(completion)>{target, std::move(completion)});
    }
};

// void(std::error_code, int)：先 post 到 io_ex，再在回调的 executor 上完成。
template <class Executor, class Token> auto async_value(Executor const& io_ex, int v, Token&& token) {
    return aw::async_initiate<Token, void(std::error_code, int)>(
        initiate_complete_later<Executor>{io_ex}, token, std::error_code{}, v);
}

// void(std::error_code, int)：以错误完成。
template <class Executor, class Token>
auto async_error(Executor const& io_ex, std::error_code ec, Token&& token) {
    return aw::async_initiate<Token, void(std::error_code, int)>(
        initiate_complete_later<Executor>{io_ex}, token, ec, 0);
}

// void(std::exception_ptr)。
template <class Executor, class Token>
auto async_exception(Executor const& io_ex, std::exception_ptr e, Token&& token) {
    return aw::async_initiate<Token, void(std::exception_ptr)>(initiate_complete_later<Executor>{io_ex},
                                                               token, e);
}

// void()。
template <class Executor, class Token> auto async_nothing(Executor const& io_ex, Token&& token) {
    return aw::async_initiate<Token, void()>(initiate_complete_later<Executor>{io_ex}, token);
}

// void(int, std::string)：没有错误参数，多个值。
template <class Executor, class Token>
auto async_pair(Executor const& io_ex, int a, std::string b, Token&& token) {
    return aw::async_initiate<Token, void(int, std::string)>(initiate_complete_later<Executor>{io_ex},
                                                             token, a, std::move(b));
}

// void(std::error_code, int)：initiation 在自己的栈里同步调用回调，前后打点。
struct initiate_immediately {
    std::vector<std::string>* trace;
    template <class Handler> void operator()(Handler&& handler, int v) const {
        trace->push_back("init-begin");
        handler(std::error_code{}, v);
        trace->push_back("init-end");
    }
};
template <class Token> auto async_immediate(std::vector<std::string>& trace, int v, Token&& token) {
    return aw::async_initiate<Token, void(std::error_code, int)>(initiate_immediately{&trace}, token, v);
}

// void(std::error_code, int)：在一个外部线程上完成（经关联 executor 回到协程的 executor）。
class foreign_threads {
  public:
    foreign_threads() = default;
    foreign_threads(foreign_threads const&) = delete;
    foreign_threads& operator=(foreign_threads const&) = delete;
    ~foreign_threads() {
        std::vector<std::thread> threads;
        {
            std::lock_guard<std::mutex> lock{mutex_};
            threads.swap(threads_);
        }
        for (auto& t : threads) t.join();
    }
    template <class F> void start(F&& f) {
        std::lock_guard<std::mutex> lock{mutex_};
        threads_.emplace_back(std::forward<F>(f));
    }

  private:
    std::mutex mutex_;
    std::vector<std::thread> threads_;
};

struct initiate_on_foreign_thread {
    foreign_threads* threads;
    template <class Handler> void operator()(Handler&& handler, int v) const {
        using handler_type = typename std::decay<Handler>::type;
        auto target = aw::get_associated_executor(handler, 0);
        threads->start([target, v, h = handler_type(std::forward<Handler>(handler))]() mutable {
            target.dispatch(bind_completion(std::move(h), std::error_code{}, v));
        });
    }
};
template <class Token> auto async_from_foreign_thread(foreign_threads& threads, int v, Token&& token) {
    return aw::async_initiate<Token, void(std::error_code, int)>(initiate_on_foreign_thread{&threads},
                                                                 token, v);
}

} // namespace aw_test
```

`tests/unit/use_awaitable_test.cpp`：

```cpp
#include <chrono>
#include <exception>
#include <future>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

#include "awaitable/awaitable.hpp"
#include "awaitable/co_spawn.hpp"
#include "awaitable/use_awaitable.hpp"
#include "awaitable/use_future.hpp"
#include "check.hpp"
#include "counting_new.hpp"
#include "test_ops.hpp"

namespace {

using ex_t = aw::manual_executor::executor_type;
template <class T> using task = aw::awaitable<T, ex_t>;

auto add_two_values(ex_t ex) CO2_BEG(task<int>, (ex), int a{}; int b{};) {
    CO2_AWAIT_SET(a, aw_test::async_value(ex, 40, aw::use_awaitable));
    CO2_AWAIT_SET(b, aw_test::async_value(ex, 2, aw::use_awaitable));
    CO2_RETURN(a + b);
}
CO2_END

auto await_error(ex_t ex) CO2_BEG(task<int>, (ex), int a{};) {
    CO2_AWAIT_SET(a, aw_test::async_error(ex, std::make_error_code(std::errc::timed_out),
                                          aw::use_awaitable));
    CO2_RETURN(a);
}
CO2_END

auto await_exception(ex_t ex) CO2_BEG(task<void>, (ex)) {
    CO2_AWAIT(aw_test::async_exception(ex, std::make_exception_ptr(std::logic_error{"op"}),
                                       aw::use_awaitable));
    CO2_RETURN();
}
CO2_END

auto void_and_tuple(ex_t ex) CO2_BEG(task<std::string>, (ex), std::tuple<int, std::string> p{};) {
    CO2_AWAIT(aw_test::async_nothing(ex, aw::use_awaitable));
    CO2_AWAIT(aw_test::async_exception(ex, std::exception_ptr{}, aw::use_awaitable));
    CO2_AWAIT_SET(p, aw_test::async_pair(ex, 7, std::string{"seven"}, aw::use_awaitable));
    CO2_RETURN(std::to_string(std::get<0>(p)) + std::get<1>(p));
}
CO2_END

auto immediate(std::vector<std::string>& trace) CO2_BEG(task<int>, (trace), int v{};) {
    trace.push_back("before");
    CO2_AWAIT_SET(v, aw_test::async_immediate(trace, 5, aw::use_awaitable));
    trace.push_back("after");
    CO2_RETURN(v);
}
CO2_END

auto many_awaits(ex_t ex, int n, long* news)
    CO2_BEG(task<int>, (ex, n, news), int i{}; int r{}; int sum{}; aw_test::allocation_counter* counter{};) {
    for (i = 0; i != n; ++i) {
        if (i == 100) counter = new aw_test::allocation_counter{};
        CO2_AWAIT_SET(r, aw_test::async_value(ex, i, aw::use_awaitable));
        sum += r;
    }
    *news = counter->news();
    delete counter;
    CO2_RETURN(sum);
}
CO2_END

using pool_ex_t = aw::thread_pool::executor_type;

auto resumes_on_pool(aw_test::foreign_threads& threads)
    CO2_BEG(aw::pool_awaitable<bool>, (threads), pool_ex_t ex{}; int v{};) {
    CO2_AWAIT_SET(ex, aw::this_coro::executor);
    CO2_AWAIT_SET(v, aw_test::async_from_foreign_thread(threads, 3, aw::use_awaitable));
    CO2_RETURN(v == 3 && ex.running_in_this_thread());
}
CO2_END

auto child(int v) CO2_BEG(task<int>, (v)) {
    if (v < 0) throw std::runtime_error{"negative"};
    CO2_RETURN(v * 2);
}
CO2_END

auto pool_child(int v) CO2_BEG(aw::pool_awaitable<int>, (v)) { CO2_RETURN(v * 3); }
CO2_END

auto spawn_and_wait(ex_t ex, aw::thread_pool* pool)
    CO2_BEG(task<bool>, (ex, pool), int a{}; int b{};) {
    CO2_AWAIT_SET(a, aw::co_spawn(ex, child(5), aw::use_awaitable));
    CO2_AWAIT_SET(b, aw::co_spawn(pool->get_executor(), pool_child(5), aw::use_awaitable));
    CO2_RETURN(a == 10 && b == 15 && ex.running_in_this_thread());
}
CO2_END

auto spawn_failing(ex_t ex) CO2_BEG(task<int>, (ex), int a{};) {
    CO2_AWAIT_SET(a, aw::co_spawn(ex, child(-1), aw::use_awaitable));
    CO2_RETURN(a);
}
CO2_END

template <class T, class Awaitable> T run_to_value(aw::manual_executor& ctx, Awaitable a) {
    std::future<T> f = aw::co_spawn(ctx, std::move(a), aw::use_future);
    ctx.run();
    return f.get();
}

void valuesAreDelivered() {
    aw::manual_executor ctx;
    AW_CHECK(run_to_value<int>(ctx, add_two_values(ctx.get_executor())) == 42);
}

void errorCodesBecomeSystemErrors() {
    aw::manual_executor ctx;
    std::future<int> f = aw::co_spawn(ctx, await_error(ctx.get_executor()), aw::use_future);
    ctx.run();
    bool caught = false;
    try {
        static_cast<void>(f.get());
    } catch (std::system_error const& e) {
        caught = e.code() == std::make_error_code(std::errc::timed_out);
    }
    AW_CHECK(caught);
}

void exceptionPointersAreRethrown() {
    aw::manual_executor ctx;
    std::future<void> f = aw::co_spawn(ctx, await_exception(ctx.get_executor()), aw::use_future);
    ctx.run();
    bool caught = false;
    try {
        f.get();
    } catch (std::logic_error const& e) {
        caught = std::string{e.what()} == "op";
    }
    AW_CHECK(caught);
}

void voidAndTupleResults() {
    aw::manual_executor ctx;
    AW_CHECK(run_to_value<std::string>(ctx, void_and_tuple(ctx.get_executor())) == "7seven");
}

void synchronousCompletionResumesViaPost() {
    aw::manual_executor ctx;
    std::vector<std::string> trace;
    AW_CHECK(run_to_value<int>(ctx, immediate(trace)) == 5);
    AW_CHECK((trace == std::vector<std::string>{"before", "init-begin", "init-end", "after"}));
}

void awaiterFitsTheSlot() {
    aw::manual_executor ctx;
    using awaiter = decltype(aw_test::async_value(ctx.get_executor(), 1, aw::use_awaitable));
    static_assert(sizeof(awaiter) <= 64U, "the standard test operation's awaiter must fit the 64 B slot");
    AW_CHECK(sizeof(awaiter) <= 64U);
}

void sequentialAwaitsDoNotAllocate() {
    aw::manual_executor ctx;
    long news = -1;
    AW_CHECK(run_to_value<int>(ctx, many_awaits(ctx.get_executor(), 1100, &news)) == 1099 * 1100 / 2);
    if (aw_test::counting_new_enabled) AW_CHECK(news == 0);
}

void foreignCompletionResumesOnTheExecutor() {
    aw_test::foreign_threads threads;
    aw::thread_pool pool{2};
    std::future<bool> f = aw::co_spawn(pool, resumes_on_pool(threads), aw::use_future);
    AW_CHECK(f.get());
}

void coSpawnCanBeAwaited() {
    aw::thread_pool pool{1};
    aw::manual_executor ctx;
    std::future<bool> f = aw::co_spawn(ctx, spawn_and_wait(ctx.get_executor(), &pool), aw::use_future);
    while (f.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready) ctx.run_one();
    AW_CHECK(f.get());
}

void awaitedCoSpawnRethrows() {
    aw::manual_executor ctx;
    std::future<int> f = aw::co_spawn(ctx, spawn_failing(ctx.get_executor()), aw::use_future);
    ctx.run();
    bool caught = false;
    try {
        static_cast<void>(f.get());
    } catch (std::runtime_error const& e) {
        caught = std::string{e.what()} == "negative";
    }
    AW_CHECK(caught);
}

} // namespace

int main() {
    valuesAreDelivered();
    errorCodesBecomeSystemErrors();
    exceptionPointersAreRethrown();
    voidAndTupleResults();
    synchronousCompletionResumesViaPost();
    awaiterFitsTheSlot();
    sequentialAwaitsDoNotAllocate();
    foreignCompletionResumesOnTheExecutor();
    coSpawnCanBeAwaited();
    awaitedCoSpawnRethrows();
    return 0;
}
```

`tests/unit/CMakeLists.txt` 追加：

```cmake
aw_unit_test(use_awaitable_test)
```

- [ ] **Step 2：运行，确认失败**

Run: `cmake --build build/debug --target aw-use-awaitable-test 2>&1 | grep -m1 "error:"`
Expected：包含 `'awaitable/use_awaitable.hpp' file not found`

- [ ] **Step 3：最小实现**

要点：`on_suspend` 里先把 `init_` 和 `args_` 移到栈上再调用 initiation，因为 initiation 可能同步完成并销毁 awaiter 所在的帧；`initiate` 是静态函数，签名 `(Initiation& init, Handler&&, std::tuple<Args...>& args, std::index_sequence<I...>)`。

`include/awaitable/impl/async_op_awaiter.hpp`：

```cpp
#pragma once

#include <cstddef>
#include <exception>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

#include "awaitable/core/coroutine_handle.hpp"
#include "awaitable/exec/detail/thread_context.hpp"
#include "awaitable/impl/awaitable_frame.hpp"
#include "awaitable/impl/awaitable_thread.hpp"

namespace aw {
namespace detail {

// ---- 完成参数 → await_resume 的结果（§7.1 第 4 条）----

template <class... Ts> struct op_lead { static constexpr std::size_t skip = 0U; };
template <class... Ts> struct op_lead<std::error_code, Ts...> { static constexpr std::size_t skip = 1U; };
template <class... Ts> struct op_lead<std::exception_ptr, Ts...> {
    static constexpr std::size_t skip = 1U;
};

template <class... Ts> struct op_value { using type = std::tuple<Ts...>; };
template <class T> struct op_value<T> { using type = T; };
template <> struct op_value<> { using type = void; };

template <std::size_t Skip, class... Ts> struct op_value_after;
template <class... Ts> struct op_value_after<0U, Ts...> { using type = typename op_value<Ts...>::type; };
template <class T, class... Ts> struct op_value_after<1U, T, Ts...> {
    using type = typename op_value<Ts...>::type;
};

inline void throw_if_error(std::error_code const& ec) {
    if (ec) throw std::system_error{ec};
}
inline void throw_if_error(std::exception_ptr const& e) {
    if (e) std::rethrow_exception(e);
}

template <class R> struct op_taker {
    template <std::size_t Skip, class Refs, std::size_t... I>
    static R take(Refs& refs, std::index_sequence<I...>) {
        return R(std::move(std::get<Skip + I>(refs))...);
    }
};
template <> struct op_taker<void> {
    template <std::size_t Skip, class Refs, std::size_t... I>
    static void take(Refs&, std::index_sequence<I...>) noexcept {}
};

template <class Signature> struct op_signature;
template <class... Ts> struct op_signature<void(Ts...)> {
    static_assert(std::is_same<std::tuple<Ts...>, std::tuple<typename std::decay<Ts>::type...>>::value,
                  "use_awaitable: completion signature arguments must be decayed object types");
    using refs_type = std::tuple<Ts&...>;
    static constexpr std::size_t skip = op_lead<Ts...>::skip;
    using result_type = typename op_value_after<skip, Ts...>::type;

    static result_type finish(refs_type& refs) {
        check(std::integral_constant<bool, skip == 1U>{}, refs);
        return op_taker<result_type>::template take<skip>(
            refs, std::make_index_sequence<sizeof...(Ts) - skip>{});
    }

  private:
    static void check(std::false_type, refs_type&) noexcept {}
    static void check(std::true_type, refs_type& refs) { throw_if_error(std::get<0>(refs)); }
};

// ---- 同步完成：参数移进投递的 operation，在那里 pump（§8.3 同步完成）----

template <class Executor, class... Ts> struct sync_resume_op {
    awaitable_thread<Executor> thread;
    std::tuple<Ts&...>** result;
    std::tuple<Ts...> values;

    void operator()() { run(std::index_sequence_for<Ts...>{}); }

    template <std::size_t... I> void run(std::index_sequence<I...>) {
        std::tuple<Ts&...> refs{std::get<I>(values)...};
        *result = &refs;
        thread.frame()->attach_thread(&thread);
        thread.frame()->clear_cancellation_slot();
        thread.pump();
    }
};

// §8.3：完成回调。移动持有执行线程；关联 executor / slot 来自执行线程（asio :804-1018）。
template <class Executor, class Signature> class awaitable_async_op_handler;

template <class Executor, class... Ts>
class awaitable_async_op_handler<Executor, void(Ts...)> : public awaitable_thread<Executor> {
  public:
    awaitable_async_op_handler(awaitable_thread<Executor>&& thread, std::tuple<Ts&...>** result,
                               void const* initiating_key) noexcept
        : awaitable_thread<Executor>{std::move(thread)}, result_{result}, key_{initiating_key} {}
    awaitable_async_op_handler(awaitable_async_op_handler&&) noexcept = default;

    void operator()(Ts... args) {
        if (initiating_marker() == key_) {
            // initiation 在自己的栈里同步调用了回调：不在这里 pump，先 post。
            Executor ex = this->get_executor();
            ex.post(sync_resume_op<Executor, Ts...>{awaitable_thread<Executor>{std::move(*this)},
                                                    result_, std::tuple<Ts...>{std::move(args)...}});
            return;
        }
        // 参数留在本栈上，awaiter 只拿到地址（asio :840-858）。
        std::tuple<Ts&...> refs{args...};
        *result_ = &refs;
        this->frame()->attach_thread(this);
        this->frame()->clear_cancellation_slot();
        this->pump();
    }

  private:
    std::tuple<Ts&...>** result_;
    void const* key_;
};

// §8.3：直接 awaiter，不为 use_awaitable 另开协程帧（asio :1020-1085）。
template <class Signature, class RequiredExecutor, class Initiation, class... Args> class async_op_awaiter;

template <class... Ts, class RequiredExecutor, class Initiation, class... Args>
class async_op_awaiter<void(Ts...), RequiredExecutor, Initiation, Args...> {
    using signature = op_signature<void(Ts...)>;

  public:
    using aw_awaitable_tag = void;

    template <class I, class... A>
    explicit async_op_awaiter(I&& init, A&&... args)
        : init_(std::forward<I>(init)), args_(std::forward<A>(args)...) {}
    async_op_awaiter(async_op_awaiter&&) = default;
    async_op_awaiter(async_op_awaiter const&) = delete;
    async_op_awaiter& operator=(async_op_awaiter const&) = delete;
    async_op_awaiter& operator=(async_op_awaiter&&) = delete;

    bool await_ready() const noexcept { return false; }

    template <class P> void await_suspend(co2::coroutine_handle<P> h) noexcept {
        using executor_type = typename P::executor_type;
        static_assert(std::is_void<RequiredExecutor>::value ||
                          std::is_same<RequiredExecutor, executor_type>::value,
                      "use_awaitable_t<E>: E must be the executor of the awaiting frame");
        awaitable_frame_base<executor_type>& frame = h.promise();
        frame_ = &frame;
        frame.after_suspend(&on_suspend<executor_type>, this);
    }

    typename signature::result_type await_resume() { return signature::finish(*result_); }

  private:
    // 帧的 resume() 返回之后执行：此时帧已挂起，可以把执行线程交给回调。
    template <class Executor> static void on_suspend(void* arg) {
        async_op_awaiter* const self = static_cast<async_op_awaiter*>(arg);
        auto* const frame = static_cast<awaitable_frame_base<Executor>*>(self->frame_);
        initiating_scope scope{self};
        awaitable_async_op_handler<Executor, void(Ts...)> handler{std::move(*frame->detach_thread()),
                                                                  &self->result_, self};
        // 回调可能在别的线程恢复帧并销毁本 awaiter，所以先把 init 和参数移到本栈上，
        // initiate 期间与之后都不再访问 self。
        Initiation init(std::move(self->init_));
        std::tuple<Args...> args(std::move(self->args_));
        initiate(init, std::move(handler), args, std::index_sequence_for<Args...>{});
    }

    template <class Handler, std::size_t... I>
    static void initiate(Initiation& init, Handler&& handler, std::tuple<Args...>& args,
                         std::index_sequence<I...>) {
        std::move(init)(std::forward<Handler>(handler), std::move(std::get<I>(args))...);
    }

    void* frame_{nullptr};
    std::tuple<Ts&...>* result_{nullptr};
    Initiation init_;
    std::tuple<Args...> args_;
};

} // namespace detail
} // namespace aw
```

`include/awaitable/use_awaitable.hpp`：

```cpp
#pragma once

#include <type_traits>
#include <utility>

#include "awaitable/async_result.hpp"
#include "awaitable/awaitable.hpp"
#include "awaitable/impl/async_op_awaiter.hpp"

namespace aw {

// §8.3：Executor = void 时取等待它的执行线程的 executor；否则必须与之相同（编译期检查）。
template <class Executor = void> struct use_awaitable_t {
    constexpr use_awaitable_t() noexcept = default;
};
constexpr use_awaitable_t<> use_awaitable{};

template <class Executor, class... Ts> struct async_result<use_awaitable_t<Executor>, void(Ts...)> {
    template <class Initiation, class RawToken, class... Args>
    static detail::async_op_awaiter<void(Ts...), Executor, typename std::decay<Initiation>::type,
                                    typename std::decay<Args>::type...>
    initiate(Initiation&& init, RawToken&&, Args&&... args) {
        return detail::async_op_awaiter<void(Ts...), Executor, typename std::decay<Initiation>::type,
                                        typename std::decay<Args>::type...>{
            std::forward<Initiation>(init), std::forward<Args>(args)...};
    }
};

} // namespace aw
```

- [ ] **Step 4：运行，确认通过（含 ASan）**

Run: `cmake --build build/debug -j8 && ctest --test-dir build/debug -R '^aw-use-awaitable' --output-on-failure && tools/run-matrix.sh asan-cxx14`
Expected：先输出 `100% tests passed out of 1`，再输出 `asan-cxx14: 100% tests passed out of 42`。

- [ ] **Step 5：提交**

```bash
git add include/awaitable tests/support tests/unit
git commit -m "feat(core): use_awaitable token with direct async_op_awaiter"
```

### Task 12：as_tuple 与 redirect_error

**交付物：** §8.4 的两个令牌适配器。`as_tuple(token)` 把 `void(Ts...)` 改写成 `void(std::tuple<Ts...>)`；`redirect_error(token, ec)` 把 `void(E, Ts...)` 的首参数写进 `ec`，剩下的转给内层令牌。两者都转发被包装 handler 的 executor 和取消槽关联（§8.1）。

**Files:**
- Create: `include/awaitable/as_tuple.hpp`, `include/awaitable/redirect_error.hpp`
- Test: `tests/unit/as_tuple_redirect_error_test.cpp`
- Modify: `tests/unit/CMakeLists.txt`（追加）

**Interfaces:**
- Consumes：
  - Task 9：`async_result`、`async_initiate`、`associated_executor`、`associated_cancellation_slot`、`use_future`、回调令牌；
  - Task 11：`use_awaitable`、`tests/support/test_ops.hpp`。
- Produces：
  - `template <class Token> struct aw::as_tuple_t`，`aw::as_tuple(token)`；
  - `aw::async_result<as_tuple_t<Token>, void(Ts...)>`，内层签名是 `void(std::tuple<Ts...>)`；
  - `template <class Token, class Error> struct aw::redirect_error_t`，`aw::redirect_error(token, Error& target)`；
  - `aw::async_result<redirect_error_t<Token, Error>, void(Error, Ts...)>`，内层签名是 `void(Ts...)`；
  - `associated_executor` 和 `associated_cancellation_slot` 对 `detail::as_tuple_handler`、`detail::redirect_error_handler` 的特化：都转发给内层 handler。

- [ ] **Step 1：写失败的测试**

`tests/unit/as_tuple_redirect_error_test.cpp`：

```cpp
#include <exception>
#include <future>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>

#include "awaitable/as_tuple.hpp"
#include "awaitable/awaitable.hpp"
#include "awaitable/co_spawn.hpp"
#include "awaitable/redirect_error.hpp"
#include "awaitable/use_awaitable.hpp"
#include "awaitable/use_future.hpp"
#include "check.hpp"
#include "test_ops.hpp"

namespace {

using ex_t = aw::manual_executor::executor_type;
template <class T> using task = aw::awaitable<T, ex_t>;

auto tuple_of_error(ex_t ex) CO2_BEG(task<bool>, (ex), std::tuple<std::error_code, int> r{};) {
    CO2_AWAIT_SET(r, aw_test::async_error(ex, std::make_error_code(std::errc::timed_out),
                                          aw::as_tuple(aw::use_awaitable)));
    CO2_RETURN(std::get<0>(r) == std::make_error_code(std::errc::timed_out) && std::get<1>(r) == 0);
}
CO2_END

auto redirected(ex_t ex) CO2_BEG(task<bool>, (ex), std::error_code ec{}; int a{}; int b{};) {
    CO2_AWAIT_SET(a, aw_test::async_error(ex, std::make_error_code(std::errc::timed_out),
                                          aw::redirect_error(aw::use_awaitable, ec)));
    if (ec != std::make_error_code(std::errc::timed_out) || a != 0) CO2_RETURN(false);
    CO2_AWAIT_SET(b, aw_test::async_value(ex, 9, aw::redirect_error(aw::use_awaitable, ec)));
    CO2_RETURN(!ec && b == 9);
}
CO2_END

auto child(int v) CO2_BEG(task<int>, (v)) {
    if (v < 0) throw std::runtime_error{"negative"};
    CO2_RETURN(v);
}
CO2_END

// §7.4：拿到子协程的异常而不传播。
auto catches_child(ex_t ex)
    CO2_BEG(task<std::string>, (ex), std::tuple<std::exception_ptr, int> r{}; std::exception_ptr e{};
            int v{};) {
    CO2_AWAIT_SET(r, aw::co_spawn(ex, child(-1), aw::as_tuple(aw::use_awaitable)));
    CO2_AWAIT_SET(v, aw::co_spawn(ex, child(-2), aw::redirect_error(aw::use_awaitable, e)));
    std::string out;
    try {
        std::rethrow_exception(std::get<0>(r));
    } catch (std::runtime_error const& error) {
        out += error.what();
    }
    try {
        std::rethrow_exception(e);
    } catch (std::runtime_error const& error) {
        out += error.what();
    }
    CO2_RETURN(out + std::to_string(v));
}
CO2_END

template <class T, class Awaitable> T run_to_value(aw::manual_executor& ctx, Awaitable a) {
    std::future<T> f = aw::co_spawn(ctx, std::move(a), aw::use_future);
    ctx.run();
    return f.get();
}

void asTupleKeepsTheErrorCode() {
    aw::manual_executor ctx;
    AW_CHECK(run_to_value<bool>(ctx, tuple_of_error(ctx.get_executor())));
}

void redirectErrorWritesTheTarget() {
    aw::manual_executor ctx;
    AW_CHECK(run_to_value<bool>(ctx, redirected(ctx.get_executor())));
}

void childExceptionsCanBeCaptured() {
    aw::manual_executor ctx;
    AW_CHECK(run_to_value<std::string>(ctx, catches_child(ctx.get_executor())) == "negativenegative0");
}

void asTupleWithCallbackAndFuture() {
    aw::manual_executor ctx;
    auto ex = ctx.get_executor();
    std::tuple<std::error_code, int> got{};
    aw_test::async_value(ex, 3, aw::as_tuple([&](std::tuple<std::error_code, int> t) { got = t; }));
    std::future<std::tuple<std::error_code, int>> f = aw_test::async_value(ex, 4, aw::as_tuple(aw::use_future));
    ctx.run();
    AW_CHECK(!std::get<0>(got) && std::get<1>(got) == 3);
    AW_CHECK(std::get<1>(f.get()) == 4);
}

void wrappersForwardAssociations() {
    aw::manual_executor io;
    aw::manual_executor target;
    bool on_target = false;
    auto bound = aw::bind_executor(target.get_executor(), [&](std::tuple<std::error_code, int>) {
        on_target = target.get_executor().running_in_this_thread();
    });
    aw_test::async_value(io.get_executor(), 1, aw::as_tuple(bound));
    io.run();
    AW_CHECK(!on_target && target.pending() == 1U);
    target.run();
    AW_CHECK(on_target);

    aw::cancellation_signal signal;
    auto slot_bound = aw::bind_cancellation_slot(signal.slot(), [](int) {});
    using inner = decltype(slot_bound);
    aw::detail::redirect_error_handler<inner, std::error_code, void(int)> redirect{slot_bound, nullptr};
    aw::detail::as_tuple_handler<decltype(redirect), void(std::error_code, int)> wrapped{redirect};
    AW_CHECK(aw::get_associated_cancellation_slot(wrapped) == signal.slot());
}

} // namespace

int main() {
    asTupleKeepsTheErrorCode();
    redirectErrorWritesTheTarget();
    childExceptionsCanBeCaptured();
    asTupleWithCallbackAndFuture();
    wrappersForwardAssociations();
    return 0;
}
```

`tests/unit/CMakeLists.txt` 追加：

```cmake
aw_unit_test(as_tuple_redirect_error_test)
```

- [ ] **Step 2：运行，确认失败**

Run: `cmake --build build/debug --target aw-as-tuple-redirect-error-test 2>&1 | grep -m1 "error:"`
Expected：包含 `'awaitable/as_tuple.hpp' file not found`

- [ ] **Step 3：最小实现**

`include/awaitable/as_tuple.hpp`：

```cpp
#pragma once

#include <tuple>
#include <type_traits>
#include <utility>

#include "awaitable/associated.hpp"
#include "awaitable/async_result.hpp"

namespace aw {

// §8.4：void(Ts...) 变成 void(std::tuple<Ts...>)，不再把错误转成异常（asio as_tuple.hpp:37）。
template <class Token> struct as_tuple_t {
    Token token_;
};

template <class Token> as_tuple_t<typename std::decay<Token>::type> as_tuple(Token&& token) {
    return as_tuple_t<typename std::decay<Token>::type>{std::forward<Token>(token)};
}

namespace detail {

template <class Handler, class Signature> class as_tuple_handler;

template <class Handler, class... Ts> class as_tuple_handler<Handler, void(Ts...)> {
  public:
    template <class H> explicit as_tuple_handler(H&& h) : handler_(std::forward<H>(h)) {}

    void operator()(Ts... args) { handler_(std::tuple<Ts...>(std::move(args)...)); }

    Handler const& get() const noexcept { return handler_; }

  private:
    Handler handler_;
};

template <class Initiation, class Signature> struct as_tuple_initiation {
    Initiation init;
    template <class Handler, class... Args> void operator()(Handler&& handler, Args&&... args) {
        std::move(init)(
            as_tuple_handler<typename std::decay<Handler>::type, Signature>{std::forward<Handler>(handler)},
            std::forward<Args>(args)...);
    }
};

} // namespace detail

template <class Token, class... Ts> struct async_result<as_tuple_t<Token>, void(Ts...)> {
    template <class Initiation, class RawToken, class... Args>
    static auto initiate(Initiation&& init, RawToken&& token, Args&&... args) {
        return async_initiate<Token, void(std::tuple<Ts...>)>(
            detail::as_tuple_initiation<typename std::decay<Initiation>::type, void(Ts...)>{
                std::forward<Initiation>(init)},
            token.token_, std::forward<Args>(args)...);
    }
};

// 包装后的回调转发内层回调的关联 executor / slot。
template <class Handler, class Signature, class Default>
struct associated_executor<detail::as_tuple_handler<Handler, Signature>, Default> {
    using type = typename associated_executor<Handler, Default>::type;
    static type get(detail::as_tuple_handler<Handler, Signature> const& h, Default const& d) noexcept {
        return associated_executor<Handler, Default>::get(h.get(), d);
    }
};
template <class Handler, class Signature>
struct associated_cancellation_slot<detail::as_tuple_handler<Handler, Signature>> {
    static cancellation_slot get(detail::as_tuple_handler<Handler, Signature> const& h) noexcept {
        return get_associated_cancellation_slot(h.get());
    }
};

} // namespace aw
```

`include/awaitable/redirect_error.hpp`：

```cpp
#pragma once

#include <exception>
#include <system_error>
#include <type_traits>
#include <utility>

#include "awaitable/associated.hpp"
#include "awaitable/async_result.hpp"

namespace aw {

// §8.4：剥掉签名里打头的 error_code / exception_ptr，写进引用（asio redirect_error.hpp:36）。
// 被引用的变量必须活过挂起点：在宏协程里写在 CO2_BEG 的局部列表里（§13 第 1 条）。
template <class Token, class Error> struct redirect_error_t {
    static_assert(std::is_same<Error, std::error_code>::value ||
                      std::is_same<Error, std::exception_ptr>::value,
                  "redirect_error: the target must be std::error_code or std::exception_ptr");
    Token token_;
    Error* target_;
};

template <class Token, class Error>
redirect_error_t<typename std::decay<Token>::type, Error> redirect_error(Token&& token, Error& target) {
    return redirect_error_t<typename std::decay<Token>::type, Error>{std::forward<Token>(token), &target};
}

namespace detail {

template <class Handler, class Error, class Signature> class redirect_error_handler;

template <class Handler, class Error, class... Ts> class redirect_error_handler<Handler, Error, void(Ts...)> {
  public:
    template <class H>
    redirect_error_handler(H&& h, Error* target) : handler_(std::forward<H>(h)), target_{target} {}

    void operator()(Error error, Ts... args) {
        *target_ = std::move(error);
        handler_(std::move(args)...);
    }

    Handler const& get() const noexcept { return handler_; }

  private:
    Handler handler_;
    Error* target_;
};

template <class Initiation, class Error, class Signature> struct redirect_error_initiation {
    Initiation init;
    Error* target;
    template <class Handler, class... Args> void operator()(Handler&& handler, Args&&... args) {
        std::move(init)(redirect_error_handler<typename std::decay<Handler>::type, Error, Signature>{
                            std::forward<Handler>(handler), target},
                        std::forward<Args>(args)...);
    }
};

} // namespace detail

template <class Token, class Error, class... Ts>
struct async_result<redirect_error_t<Token, Error>, void(Error, Ts...)> {
    template <class Initiation, class RawToken, class... Args>
    static auto initiate(Initiation&& init, RawToken&& token, Args&&... args) {
        return async_initiate<Token, void(Ts...)>(
            detail::redirect_error_initiation<typename std::decay<Initiation>::type, Error, void(Ts...)>{
                std::forward<Initiation>(init), token.target_},
            token.token_, std::forward<Args>(args)...);
    }
};

template <class Handler, class Error, class Signature, class Default>
struct associated_executor<detail::redirect_error_handler<Handler, Error, Signature>, Default> {
    using type = typename associated_executor<Handler, Default>::type;
    static type get(detail::redirect_error_handler<Handler, Error, Signature> const& h,
                    Default const& d) noexcept {
        return associated_executor<Handler, Default>::get(h.get(), d);
    }
};
template <class Handler, class Error, class Signature>
struct associated_cancellation_slot<detail::redirect_error_handler<Handler, Error, Signature>> {
    static cancellation_slot get(detail::redirect_error_handler<Handler, Error, Signature> const& h) noexcept {
        return get_associated_cancellation_slot(h.get());
    }
};

} // namespace aw
```

- [ ] **Step 4：运行，确认通过（含 ASan）**

Run: `cmake --build build/debug -j8 && ctest --test-dir build/debug -R '^aw-as-tuple' --output-on-failure && tools/run-matrix.sh asan-cxx14`
Expected：先输出 `100% tests passed out of 1`，再输出 `asan-cxx14: 100% tests passed out of 43`。

- [ ] **Step 5：提交**

```bash
git add include/awaitable tests/unit
git commit -m "feat(tokens): as_tuple and redirect_error adapters"
```

### Task 13：取消与 awaitable 集成

**交付物：** §6.2、§6.4 和 §8.2 的协程侧取消：
- `this_coro::cancellation_state`、`reset_cancellation_state(...)`、`throw_if_cancelled()` / `throw_if_cancelled(bool)`；
- 每个入口帧一个 `cancellation_state`，默认过滤器只放行 terminal；
- 已记录的取消会让下一次 await 抛出 `operation_aborted`（`throw_if_cancelled` 默认开）；
- `co_spawn` 把完成令牌的取消槽桥接到协程：用引用计数的 `spawn_cancellation_bridge`，因为处理器可能在执行线程结束后才运行（§8.2）；
- `aw::get_stop_token()` 返回跟随取消状态的 `stop_token`（§6.4，单向桥接，见 D10；`stop_bridge` 在第一次取 token 时才创建）。

这一步替换 Task 10 的四个文件。为了让计划可以照抄，下面给出完整的新内容。

**Files:**
- Modify（整文件替换）：`include/awaitable/this_coro.hpp`, `include/awaitable/impl/awaitable_frame.hpp`, `include/awaitable/impl/awaitable_thread.hpp`, `include/awaitable/impl/entry_point.hpp`
- Test: `tests/support/test_cancellable_ops.hpp`, `tests/unit/awaitable_cancellation_test.cpp`
- Modify: `tests/unit/CMakeLists.txt`（追加）

**Interfaces:**
- Consumes：
  - Task 7：`cancellation_signal` / `cancellation_slot` / `cancellation_state`、`enable_terminal_cancellation` / `enable_partial_cancellation` / `enable_total_cancellation`；
  - Task 8：`stop_token`、`detail::stop_bridge`、`detail::EnvStopToken`、`get_stop_token_t`；
  - Task 10：`entry_frame_base` 的 `parent_slot_`、`cancellation_state_`、`throw_if_cancelled_`、`stop_bridge_`；
  - Task 11：`async_op_awaiter`（它从 `entry()->cancellation_state_.slot()` 取槽）。
- Produces：
  - `aw::this_coro::cancellation_state_t` / `cancellation_state`；
  - `aw::this_coro::reset_cancellation_state_t<In, Out>`，以及 `reset_cancellation_state()`、`reset_cancellation_state(filter)`、`reset_cancellation_state(in, out)`；
  - `aw::this_coro::throw_if_cancelled_0_t` / `throw_if_cancelled_1_t`，`throw_if_cancelled()` / `throw_if_cancelled(bool)`；
  - `awaitable_frame_base<E>` 新增的 `await_transform` 重载：上面这些标签和 `get_stop_token_t`；新增 `envStopToken()`；
  - `entry_frame_base<E>`：`use_parent_slot(slot)`、`reset_cancellation_state(in, out)`、`bridge()`；析构时清空父槽并销毁 `stop_bridge`；
  - `aw::detail::spawn_cancellation_bridge`（`create` / `add_ref` / `release` / `signal`，用 `mem_tag::cancellation_signal` 分配）、`bridge_ref`、`spawn_cancellation_handler`、`hold_bridge`；
  - 测试支持 `tests/support/test_cancellable_ops.hpp`（命名空间 `aw_test`）：
    - `manual_event<Executor>(io_ex)`：`async_wait(token)` 完成签名 `void(std::error_code)`；`set()` 成功完成；被取消槽 emit 时以 `operation_aborted` 完成（总是 post，不在 emit 里清槽）；
    - `async_cancellable_value(io_ex, v, token)`：完成签名 `void(std::error_code, int)`，安装取消处理器，被取消时以 `operation_aborted` 完成。

- [ ] **Step 1：写失败的测试**

七个场景：terminal 到达异步操作；默认过滤器丢掉 partial；关掉 `throw_if_cancelled` 时 partial 也能放行；已记录的取消在下一次 await 抛出；`reset_cancellation_state` 清掉记录；`stop_token` 跟随取消；完成之后再 emit 无害。

`tests/support/test_cancellable_ops.hpp`：

```cpp
#pragma once

// 支持取消的测试操作（§8.1 操作作者契约第 3 条）。

#include <system_error>
#include <type_traits>
#include <utility>

#include "awaitable/associated.hpp"
#include "awaitable/async_result.hpp"
#include "awaitable/cancel/cancellation_signal.hpp"
#include "awaitable/cancel/cancellation_type.hpp"
#include "awaitable/core/detail/move_only_function.hpp"
#include "awaitable/error.hpp"
#include "test_ops.hpp"

namespace aw_test {

// void(std::error_code)：等到 set() 或被取消。只在 io_ex 上使用（单线程）。
template <class Executor> class manual_event {
  public:
    explicit manual_event(Executor const& io_ex) noexcept : io_ex_{io_ex} {}
    manual_event(manual_event const&) = delete;
    manual_event& operator=(manual_event const&) = delete;

    template <class Token> auto async_wait(Token&& token) {
        return aw::async_initiate<Token, void(std::error_code)>(initiate_wait{this}, token);
    }

    bool waiting() const noexcept { return static_cast<bool>(pending_); }
    aw::cancellation_type last_cancel() const noexcept { return last_cancel_; }

    // 成功完成：先清 slot（处理器指向本对象），再经关联 executor 交付。
    void set() {
        if (!pending_) return;
        slot_.clear();
        slot_ = aw::cancellation_slot{};
        auto pending = std::move(pending_);
        pending(std::error_code{});
    }

  private:
    struct initiate_wait {
        manual_event* self;
        template <class Handler> void operator()(Handler&& handler) const {
            self->start(std::forward<Handler>(handler));
        }
    };

    template <class Handler> struct completion {
        Handler handler;
        Executor io_ex;
        void operator()(std::error_code ec) {
            auto target = aw::get_associated_executor(handler, io_ex);
            target.dispatch(bind_completion(std::move(handler), ec));
        }
    };

    struct cancel_handler {
        manual_event* self;
        void operator()(aw::cancellation_type type) { self->cancel(type); }
    };

    template <class Handler> void start(Handler&& handler) {
        using handler_type = typename std::decay<Handler>::type;
        aw::cancellation_slot slot = aw::get_associated_cancellation_slot(handler);
        pending_ = co2::detail::MoveOnlyFunction<void(std::error_code)>(
            completion<handler_type>{std::forward<Handler>(handler), io_ex_});
        if (slot.is_connected()) {
            slot_ = slot;
            slot.template emplace<cancel_handler>(cancel_handler{this});
        }
    }

    // 在 slot 的 emit 里运行：不能清 slot（会销毁正在运行的处理器），交付总是 post。
    void cancel(aw::cancellation_type type) {
        last_cancel_ = type;
        if (!pending_) return;
        slot_ = aw::cancellation_slot{};
        auto pending = std::move(pending_);
        io_ex_.post(bind_completion(std::move(pending), std::error_code{aw::error::operation_aborted}));
    }

    Executor io_ex_;
    co2::detail::MoveOnlyFunction<void(std::error_code)> pending_;
    aw::cancellation_slot slot_;
    aw::cancellation_type last_cancel_{aw::cancellation_type::none};
};

// void(std::error_code, int)：在 slot 上装一个处理器，然后像 async_value 一样完成。
struct noop_cancel_handler {
    void operator()(aw::cancellation_type) noexcept {}
};

template <class Executor> struct initiate_cancellable_value {
    Executor io_ex;
    template <class Handler> void operator()(Handler&& handler, int v) const {
        aw::cancellation_slot slot = aw::get_associated_cancellation_slot(handler);
        if (slot.is_connected()) slot.template emplace<noop_cancel_handler>();
        initiate_complete_later<Executor>{io_ex}(std::forward<Handler>(handler), std::error_code{}, v);
    }
};

template <class Executor, class Token>
auto async_cancellable_value(Executor const& io_ex, int v, Token&& token) {
    return aw::async_initiate<Token, void(std::error_code, int)>(initiate_cancellable_value<Executor>{io_ex},
                                                                 token, v);
}

} // namespace aw_test
```

`tests/unit/awaitable_cancellation_test.cpp`：

```cpp
#include <exception>
#include <system_error>
#include <tuple>

#include "awaitable/as_tuple.hpp"
#include "awaitable/associated.hpp"
#include "awaitable/awaitable.hpp"
#include "awaitable/cancel/cancellation_signal.hpp"
#include "awaitable/cancel/stop_token_adapter.hpp"
#include "awaitable/co_spawn.hpp"
#include "awaitable/error.hpp"
#include "awaitable/this_coro.hpp"
#include "awaitable/use_awaitable.hpp"
#include "check.hpp"
#include "test_cancellable_ops.hpp"

namespace {

using ex_t = aw::manual_executor::executor_type;
template <class T> using task = aw::awaitable<T, ex_t>;
using event_t = aw_test::manual_event<ex_t>;

bool is_aborted(std::exception_ptr const& e) {
    if (!e) return false;
    try {
        std::rethrow_exception(e);
    } catch (std::system_error const& error) {
        return error.code() == std::error_code{aw::error::operation_aborted};
    } catch (...) {
        return false;
    }
}

struct record {
    bool called{false};
    std::exception_ptr e{};
    int value{-1};
};

struct record_int {
    record* r;
    void operator()(std::exception_ptr e, int v) const {
        r->called = true;
        r->e = e;
        r->value = v;
    }
};

// 等待事件；被取消时异常逸出到 co_spawn 的回调。
auto wait_once(event_t* ev) CO2_BEG(task<int>, (ev)) {
    CO2_AWAIT(ev->async_wait(aw::use_awaitable));
    CO2_RETURN(1);
}
CO2_END

// 改成接收 partial，用 as_tuple 接住取消，再看 cancelled() 和 throw_if_cancelled。
auto partial_then_continue(event_t* ev, ex_t ex)
    CO2_BEG(task<int>, (ev, ex), std::tuple<std::error_code> r{}; aw::cancellation_state st{};
            bool throwing{}; int v{};) {
    CO2_AWAIT(aw::this_coro::reset_cancellation_state(aw::enable_partial_cancellation{}));
    CO2_AWAIT_SET(r, ev->async_wait(aw::as_tuple(aw::use_awaitable)));
    if (std::get<0>(r) != std::error_code{aw::error::operation_aborted}) CO2_RETURN(-1);
    CO2_AWAIT_SET(st, aw::this_coro::cancellation_state);
    if (st.cancelled() != aw::cancellation_type::partial) CO2_RETURN(-2);
    CO2_AWAIT_SET(throwing, aw::this_coro::throw_if_cancelled());
    if (!throwing) CO2_RETURN(-3);
    CO2_AWAIT(aw::this_coro::throw_if_cancelled(false));
    CO2_AWAIT_SET(v, aw_test::async_value(ex, 7, aw::use_awaitable));
    CO2_RETURN(v);
}
CO2_END

// 已记录取消且 throw_if_cancelled 默认开：下一次等待在发起之前抛出。
auto partial_then_throw(event_t* ev, ex_t ex, int* started)
    CO2_BEG(task<int>, (ev, ex, started), std::tuple<std::error_code> r{}; int v{};) {
    CO2_AWAIT(aw::this_coro::reset_cancellation_state(aw::enable_partial_cancellation{}));
    CO2_AWAIT_SET(r, ev->async_wait(aw::as_tuple(aw::use_awaitable)));
    ++*started;
    CO2_AWAIT_SET(v, aw_test::async_value(ex, 7, aw::use_awaitable));
    ++*started;
    CO2_RETURN(v);
}
CO2_END

// reset_cancellation_state 清除已记录的取消。
auto reset_clears(event_t* ev, ex_t ex)
    CO2_BEG(task<int>, (ev, ex), std::tuple<std::error_code> r{}; aw::cancellation_state st{}; int v{};) {
    CO2_AWAIT(aw::this_coro::reset_cancellation_state(aw::enable_total_cancellation{}));
    CO2_AWAIT_SET(r, ev->async_wait(aw::as_tuple(aw::use_awaitable)));
    CO2_AWAIT(aw::this_coro::reset_cancellation_state());
    CO2_AWAIT_SET(st, aw::this_coro::cancellation_state);
    if (st.cancelled() != aw::cancellation_type::none) CO2_RETURN(-1);
    CO2_AWAIT_SET(v, aw_test::async_value(ex, 9, aw::use_awaitable));
    CO2_RETURN(v);
}
CO2_END

// stop_token 适配器：取消转成 request_stop（§6.4）。
auto token_observer(event_t* ev, bool* stopped)
    CO2_BEG(task<int>, (ev, stopped), aw::stop_token tok{}; std::tuple<std::error_code> r{};) {
    CO2_AWAIT_SET(tok, aw::get_stop_token());
    if (tok.stop_requested()) CO2_RETURN(-1);
    CO2_AWAIT_SET(r, ev->async_wait(aw::as_tuple(aw::use_awaitable)));
    *stopped = tok.stop_requested();
    CO2_RETURN(std::get<0>(r) == std::error_code{aw::error::operation_aborted} ? 1 : -2);
}
CO2_END

void terminal_cancel_reaches_operation() {
    aw::manual_executor ctx;
    event_t ev{ctx.get_executor()};
    aw::cancellation_signal sig;
    record r;
    aw::co_spawn(ctx, wait_once(&ev), aw::bind_cancellation_slot(sig.slot(), record_int{&r}));
    ctx.run();
    AW_CHECK(ev.waiting());
    sig.emit(aw::cancellation_type::terminal);
    ctx.run();
    AW_CHECK(ev.last_cancel() == aw::cancellation_type::terminal);
    AW_CHECK(r.called && is_aborted(r.e));
}

void default_filter_drops_partial() {
    aw::manual_executor ctx;
    event_t ev{ctx.get_executor()};
    aw::cancellation_signal sig;
    record r;
    aw::co_spawn(ctx, wait_once(&ev), aw::bind_cancellation_slot(sig.slot(), record_int{&r}));
    ctx.run();
    sig.emit(aw::cancellation_type::partial);
    ctx.run();
    AW_CHECK(ev.waiting());
    AW_CHECK(ev.last_cancel() == aw::cancellation_type::none);
    ev.set();
    ctx.run();
    AW_CHECK(r.called && !r.e && r.value == 1);
}

void partial_with_throw_if_cancelled_off() {
    aw::manual_executor ctx;
    event_t ev{ctx.get_executor()};
    aw::cancellation_signal sig;
    record r;
    aw::co_spawn(ctx, partial_then_continue(&ev, ctx.get_executor()),
                 aw::bind_cancellation_slot(sig.slot(), record_int{&r}));
    ctx.run();
    sig.emit(aw::cancellation_type::partial);
    ctx.run();
    AW_CHECK(ev.last_cancel() == aw::cancellation_type::partial);
    AW_CHECK(r.called && !r.e && r.value == 7);
}

void recorded_cancel_throws_on_next_await() {
    aw::manual_executor ctx;
    event_t ev{ctx.get_executor()};
    aw::cancellation_signal sig;
    record r;
    int started = 0;
    aw::co_spawn(ctx, partial_then_throw(&ev, ctx.get_executor(), &started),
                 aw::bind_cancellation_slot(sig.slot(), record_int{&r}));
    ctx.run();
    sig.emit(aw::cancellation_type::partial);
    ctx.run();
    AW_CHECK(started == 1);
    AW_CHECK(r.called && is_aborted(r.e));
}

void reset_clears_recorded_cancel() {
    aw::manual_executor ctx;
    event_t ev{ctx.get_executor()};
    aw::cancellation_signal sig;
    record r;
    aw::co_spawn(ctx, reset_clears(&ev, ctx.get_executor()),
                 aw::bind_cancellation_slot(sig.slot(), record_int{&r}));
    ctx.run();
    sig.emit(aw::cancellation_type::total);
    ctx.run();
    AW_CHECK(ev.last_cancel() == aw::cancellation_type::total);
    AW_CHECK(r.called && !r.e && r.value == 9);
}

void stop_token_follows_cancellation() {
    aw::manual_executor ctx;
    event_t ev{ctx.get_executor()};
    aw::cancellation_signal sig;
    record r;
    bool stopped = false;
    aw::co_spawn(ctx, token_observer(&ev, &stopped),
                 aw::bind_cancellation_slot(sig.slot(), record_int{&r}));
    ctx.run();
    sig.emit(aw::cancellation_type::terminal);
    ctx.run();
    AW_CHECK(stopped);
    AW_CHECK(r.called && !r.e && r.value == 1);
}

// 回调先完成、之后才 emit：处理器经引用计数桥安全地变成空操作。
void emit_after_completion_is_harmless() {
    aw::manual_executor ctx;
    event_t ev{ctx.get_executor()};
    aw::cancellation_signal sig;
    record r;
    aw::co_spawn(ctx, wait_once(&ev), aw::bind_cancellation_slot(sig.slot(), record_int{&r}));
    ctx.run();
    ev.set();
    ctx.run();
    AW_CHECK(r.called && !r.e && r.value == 1);
    sig.emit(aw::cancellation_type::terminal);
    ctx.run();
}

} // namespace

int main() {
    terminal_cancel_reaches_operation();
    default_filter_drops_partial();
    partial_with_throw_if_cancelled_off();
    recorded_cancel_throws_on_next_await();
    reset_clears_recorded_cancel();
    stop_token_follows_cancellation();
    emit_after_completion_is_harmless();
    return 0;
}
```

`tests/unit/CMakeLists.txt` 追加：

```cmake
aw_unit_test(awaitable_cancellation_test)
```

- [ ] **Step 2：运行，确认失败**

Run: `cmake --build build/debug --target aw-awaitable-cancellation-test 2>&1 | grep -m1 "error:"`
Expected：包含 `no member named 'reset_cancellation_state' in namespace 'aw::this_coro'`

- [ ] **Step 3：实现（整文件替换四个头文件）**

`include/awaitable/this_coro.hpp`（整文件替换）：

```cpp
#pragma once

#include "awaitable/cancel/cancellation_state.hpp"

namespace aw {
namespace this_coro {

// CO2_AWAIT_SET(ex, aw::this_coro::executor);
struct executor_t {
    using aw_this_coro_tag = void;
    constexpr executor_t() noexcept = default;
};
constexpr executor_t executor{};

// §4.5 / §6.2：CO2_AWAIT_SET(state, aw::this_coro::cancellation_state);
struct cancellation_state_t {
    using aw_this_coro_tag = void;
    constexpr cancellation_state_t() noexcept = default;
};
constexpr cancellation_state_t cancellation_state{};

// CO2_AWAIT(aw::this_coro::reset_cancellation_state(...))：用新过滤器重建取消状态并清除已记录的取消
// （asio this_coro.hpp:76-140）。
template <class In, class Out> struct reset_cancellation_state_t {
    using aw_this_coro_tag = void;
    In in;
    Out out;
};

inline reset_cancellation_state_t<enable_terminal_cancellation, enable_terminal_cancellation>
reset_cancellation_state() noexcept {
    return {enable_terminal_cancellation{}, enable_terminal_cancellation{}};
}
template <class Filter>
reset_cancellation_state_t<Filter, Filter> reset_cancellation_state(Filter filter) {
    return {filter, filter};
}
template <class In, class Out>
reset_cancellation_state_t<In, Out> reset_cancellation_state(In in, Out out) {
    return {in, out};
}

// CO2_AWAIT_SET(on, aw::this_coro::throw_if_cancelled()); CO2_AWAIT(aw::this_coro::throw_if_cancelled(false));
struct throw_if_cancelled_0_t {
    using aw_this_coro_tag = void;
};
struct throw_if_cancelled_1_t {
    using aw_this_coro_tag = void;
    bool value;
};
inline throw_if_cancelled_0_t throw_if_cancelled() noexcept { return {}; }
inline throw_if_cancelled_1_t throw_if_cancelled(bool value) noexcept { return {value}; }

} // namespace this_coro
} // namespace aw
```

`include/awaitable/impl/awaitable_frame.hpp`（整文件替换）：

```cpp
#pragma once

#include <cstddef>
#include <exception>
#include <system_error>
#include <type_traits>
#include <utility>

#include "awaitable/cancel/cancellation_state.hpp"
#include "awaitable/cancel/stop_token.hpp"
#include "awaitable/cancel/stop_token_adapter.hpp"
#include "awaitable/core/coroutine.hpp"
#include "awaitable/error.hpp"
#include "awaitable/exec/detail/recycling_allocator.hpp"
#include "awaitable/optional.hpp"
#include "awaitable/this_coro.hpp"

namespace aw {

template <class T, class Executor> class awaitable;

namespace detail {

template <class Executor> class awaitable_thread;
template <class Executor> class entry_frame_base;
template <class Executor, class T, class Handler> class entry_frame_impl;

// 帧 resume() 在栈上放一个，after_suspend 写进去（asio impl/awaitable.hpp:497-516）。
struct resume_context {
    void (*after_suspend_fn)(void*){nullptr};
    void* after_suspend_arg{nullptr};
};

template <class T> struct dependent_false : std::false_type {};
template <class...> struct make_void { using type = void; };

template <class T, class = void> struct has_aw_awaitable_tag : std::false_type {};
template <class T>
struct has_aw_awaitable_tag<T, typename make_void<typename T::aw_awaitable_tag>::type>
    : std::true_type {};

template <class T, class = void> struct has_aw_this_coro_tag : std::false_type {};
template <class T>
struct has_aw_this_coro_tag<T, typename make_void<typename T::aw_this_coro_tag>::type>
    : std::true_type {};

template <class T, class Executor> struct is_awaitable_of : std::false_type {};
template <class U, class Executor>
struct is_awaitable_of<awaitable<U, Executor>, Executor> : std::true_type {};

template <class Executor> class awaitable_frame_base {
  public:
    using executor_type = Executor;

    awaitable_frame_base() noexcept = default;
    awaitable_frame_base(awaitable_frame_base const&) = delete;
    awaitable_frame_base& operator=(awaitable_frame_base const&) = delete;

    // §9.4：core 的 FrameMemory 优先用 promise 的 operator new(size_t)。
    static void* operator new(std::size_t size) {
        return recycling_allocate(mem_tag::awaitable_frame, size);
    }
    static void operator delete(void* pointer, std::size_t size) noexcept {
        recycling_deallocate(mem_tag::awaitable_frame, pointer, size);
    }

    co2::suspend_always initial_suspend() const noexcept { return {}; }

    struct final_awaiter {
        awaitable_frame_base* self;
        bool await_ready() const noexcept { return false; }
        void await_suspend(co2::coroutine_handle<>) noexcept { self->pop_frame(); }
        void await_resume() const noexcept {}
    };
    final_awaiter final_suspend() noexcept { return final_awaiter{this}; }

    void unhandled_exception() noexcept { pending_exception_ = std::current_exception(); }

    void rethrow_exception() {
        if (pending_exception_) {
            std::exception_ptr e = std::move(pending_exception_);
            pending_exception_ = nullptr;
            std::rethrow_exception(e);
        }
    }

    // asio impl/awaitable.hpp:480-495。
    void push_frame(awaitable_frame_base* caller) noexcept {
        caller_ = caller;
        attached_thread_ = caller_->attached_thread_;
        attached_thread_->entry_point()->top_of_stack_ = this;
        caller_->attached_thread_ = nullptr;
    }

    void pop_frame() noexcept {
        if (caller_ != nullptr) caller_->attached_thread_ = attached_thread_;
        attached_thread_->entry_point()->top_of_stack_ = caller_;
        attached_thread_ = nullptr;
        caller_ = nullptr;
    }

    void attach_thread(awaitable_thread<Executor>* thread) noexcept { attached_thread_ = thread; }

    awaitable_thread<Executor>* detach_thread() noexcept {
        awaitable_thread<Executor>* const thread = attached_thread_;
        attached_thread_ = nullptr;
        return thread;
    }

    void resume() {
        resume_context context;
        resume_context_ = &context;
        coro_.resume();
        // 帧可能已在别的线程上继续运行：此后只读栈上的 context。
        if (context.after_suspend_fn != nullptr) context.after_suspend_fn(context.after_suspend_arg);
    }

    void after_suspend(void (*fn)(void*), void* arg) noexcept {
        resume_context_->after_suspend_fn = fn;
        resume_context_->after_suspend_arg = arg;
    }

    entry_frame_base<Executor>* entry() const noexcept { return attached_thread_->entry_point(); }

    void clear_cancellation_slot() { entry()->cancellation_state_.slot().clear(); }

    void destroy() noexcept { coro_.destroy(); }

    // ---- §4.3 await_transform ----

    template <class U>
    awaitable<U, Executor> await_transform(awaitable<U, Executor>&& child) const {
        throw_if_cancelled_now();
        return std::move(child);
    }

    template <class Op, typename std::enable_if<
                            has_aw_awaitable_tag<typename std::decay<Op>::type>::value, int>::type = 0>
    Op&& await_transform(Op&& op) const {
        throw_if_cancelled_now();
        return std::forward<Op>(op);
    }

    struct executor_awaiter {
        Executor ex;
        bool await_ready() const noexcept { return true; }
        void await_suspend(co2::coroutine_handle<>) const noexcept {}
        Executor await_resume() const noexcept { return ex; }
    };
    executor_awaiter await_transform(this_coro::executor_t) const noexcept {
        return executor_awaiter{entry()->executor_};
    }

    template <class Other,
              typename std::enable_if<
                  !is_awaitable_of<typename std::decay<Other>::type, Executor>::value &&
                      !has_aw_awaitable_tag<typename std::decay<Other>::type>::value &&
                      !has_aw_this_coro_tag<typename std::decay<Other>::type>::value,
                  int>::type = 0>
    co2::suspend_never await_transform(Other&&) const {
        static_assert(dependent_false<Other>::value,
                      "an aw::awaitable frame can only await aw::awaitable (same executor), "
                      "use_awaitable operations, this_coro queries and async_generator::next()");
        return {};
    }

    // ---- §4.5 / §6.2 / §6.4：取消相关的 this_coro 查询（Task 13）----

    template <class V> struct ready_value {
        V value;
        bool await_ready() const noexcept { return true; }
        void await_suspend(co2::coroutine_handle<>) const noexcept {}
        V await_resume() { return std::move(value); }
    };

    ready_value<cancellation_state> await_transform(this_coro::cancellation_state_t) const noexcept {
        return ready_value<cancellation_state>{entry()->cancellation_state_};
    }

    template <class In, class Out>
    co2::suspend_never await_transform(this_coro::reset_cancellation_state_t<In, Out> reset) const {
        entry()->reset_cancellation_state(reset.in, reset.out);
        return {};
    }

    ready_value<bool> await_transform(this_coro::throw_if_cancelled_0_t) const noexcept {
        return ready_value<bool>{entry()->throw_if_cancelled_};
    }

    co2::suspend_never await_transform(this_coro::throw_if_cancelled_1_t set) const noexcept {
        entry()->throw_if_cancelled_ = set.value;
        return {};
    }

    ready_value<stop_token> await_transform(get_stop_token_t) const {
        return ready_value<stop_token>{entry()->bridge().token()};
    }

    // coro 旧写法：inherit_stop_token(into, handle) 借用这里的 EnvStopToken（§6.4）。
    detail::EnvStopToken const& envStopToken() const { return entry()->bridge().env(); }

    void throw_if_cancelled_now() const {
        entry_frame_base<Executor>* const e = entry();
        if (e->throw_if_cancelled_ && !!e->cancellation_state_.cancelled())
            throw std::system_error{std::error_code{error::operation_aborted}, "co_await"};
    }

    // ---- 数据成员（asio :523-528）----
    co2::coroutine_handle<> coro_{};
    awaitable_thread<Executor>* attached_thread_{nullptr};
    awaitable_frame_base* caller_{nullptr};
    std::exception_ptr pending_exception_{};
    resume_context* resume_context_{nullptr};
};

template <class T> struct frame_result {
    aw::optional<T> value;
    template <class U> void set(U&& u) { value.emplace(std::forward<U>(u)); }
    T take() { return std::move(*value); }
};
template <class T> struct frame_result<T&> {
    T* value{nullptr};
    void set(T& u) noexcept { value = &u; }
    T& take() noexcept { return *value; }
};

template <class T, class Executor> class awaitable_frame : public awaitable_frame_base<Executor> {
  public:
    awaitable<T, Executor> get_return_object() noexcept;

    template <class U> void return_value(U&& v) { result_.set(std::forward<U>(v)); }

    T get() {
        this->caller_ = nullptr;
        this->rethrow_exception();
        return result_.take();
    }

  private:
    frame_result<T> result_;
};

template <class Executor>
class awaitable_frame<void, Executor> : public awaitable_frame_base<Executor> {
  public:
    awaitable<void, Executor> get_return_object() noexcept;

    void return_void() noexcept {}

    void get() {
        this->caller_ = nullptr;
        this->rethrow_exception();
    }
};

} // namespace detail
} // namespace aw
```

`include/awaitable/impl/awaitable_thread.hpp`（整文件替换）：

```cpp
#pragma once

#include <exception>
#include <utility>

#include "awaitable/cancel/cancellation_signal.hpp"
#include "awaitable/cancel/cancellation_state.hpp"
#include "awaitable/cancel/stop_token_adapter.hpp"
#include "awaitable/core/coroutine_handle.hpp"
#include "awaitable/exec/detail/thread_context.hpp"
#include "awaitable/impl/awaitable_frame.hpp"

namespace aw {
namespace detail {

// 入口帧（栈底）。帧头与 core 兼容，可被 pump 恢复；状态机由 entry_frame_impl 实现。
template <class Executor> class entry_frame_base : public awaitable_frame_base<Executor> {
  public:
    struct header_block {
        co2::detail::FrameHeader header;
        entry_frame_base* self;
    };

    explicit entry_frame_base(Executor const& ex) : executor_{ex} {
        block_.self = this;
        this->coro_ = co2::coroutine_handle<>::from_address(&block_.header);
        top_of_stack_ = this;
        parent_slot_ = internal_signal_.slot();
        cancellation_state_ = cancellation_state{parent_slot_};
    }
    entry_frame_base(entry_frame_base const&) = delete;
    entry_frame_base& operator=(entry_frame_base const&) = delete;
    ~entry_frame_base() {
        // 先拆掉父 slot 上的状态处理器（它的观察者指向 stop_bridge），再释放桥。
        parent_slot_.clear();
        if (stop_bridge_ != nullptr) stop_bridge::destroy(stop_bridge_);
    }

    // co_spawn 的 token 带关联 slot 时，父 slot 改为引用计数桥里的 signal（§8.2）。
    void use_parent_slot(cancellation_slot parent) {
        parent_slot_.clear();
        parent_slot_ = parent;
        cancellation_state_ = cancellation_state{parent_slot_};
    }

    template <class In, class Out> void reset_cancellation_state(In in, Out out) {
        cancellation_state_ = cancellation_state{parent_slot_, in, out};
        if (stop_bridge_ != nullptr) stop_bridge_->attach(cancellation_state_);
    }

    // §6.4：第一次取 stop_token 时惰性创建桥。
    stop_bridge& bridge() {
        if (stop_bridge_ == nullptr) {
            stop_bridge_ = stop_bridge::create();
            stop_bridge_->attach(cancellation_state_);
        }
        return *stop_bridge_;
    }

    header_block block_{};
    Executor executor_;
    awaitable_frame_base<Executor>* top_of_stack_{nullptr};
    cancellation_signal internal_signal_;
    cancellation_slot parent_slot_;
    cancellation_state cancellation_state_;
    bool throw_if_cancelled_{true};
    stop_bridge* stop_bridge_{nullptr};
};

// §4.4：执行线程。只持有栈底；所有权在 pump 和完成回调之间转移。
template <class Executor> class awaitable_thread {
  public:
    using executor_type = Executor;

    explicit awaitable_thread(entry_frame_base<Executor>* entry) noexcept : bottom_of_stack_{entry} {}
    awaitable_thread(awaitable_thread&& other) noexcept : bottom_of_stack_{other.bottom_of_stack_} {
        other.bottom_of_stack_ = nullptr;
    }
    awaitable_thread(awaitable_thread const&) = delete;
    awaitable_thread& operator=(awaitable_thread const&) = delete;
    awaitable_thread& operator=(awaitable_thread&&) = delete;
    ~awaitable_thread() {
        if (bottom_of_stack_ != nullptr) unwind_through_executor();
    }

    executor_type get_executor() const noexcept { return bottom_of_stack_->executor_; }
    cancellation_slot get_cancellation_slot() const noexcept {
        return bottom_of_stack_->cancellation_state_.slot();
    }
    entry_frame_base<Executor>* entry_point() const noexcept { return bottom_of_stack_; }
    awaitable_frame_base<Executor>* frame() const noexcept { return bottom_of_stack_->top_of_stack_; }

    // asio :753-757 与 impl/awaitable.ipp:31-43：在“启动中”标记下 pump。
    void launch() {
        launching_scope scope{static_cast<void const*>(bottom_of_stack_)};
        frame()->attach_thread(this);
        pump();
    }

    // asio :778-786。
    void pump() {
        do {
            bottom_of_stack_->top_of_stack_->resume();
        } while (bottom_of_stack_ != nullptr && bottom_of_stack_->top_of_stack_ != nullptr);
        if (bottom_of_stack_ != nullptr) complete();
    }

  private:
    // asio :794-799：销毁入口帧；交付回调时抛出的异常在这里重抛（§7.5）。
    void complete() {
        entry_frame_base<Executor>* const entry = bottom_of_stack_;
        bottom_of_stack_ = nullptr;
        std::exception_ptr e = std::move(entry->pending_exception_);
        entry->destroy();
        if (e) std::rethrow_exception(e);
    }

    struct entry_owner {
        entry_frame_base<Executor>* entry;
        explicit entry_owner(entry_frame_base<Executor>* e) noexcept : entry{e} {}
        entry_owner(entry_owner&& other) noexcept : entry{other.entry} { other.entry = nullptr; }
        entry_owner(entry_owner const&) = delete;
        entry_owner& operator=(entry_owner const&) = delete;
        ~entry_owner() {
            if (entry != nullptr) {
                unwinding_scope scope;
                entry->destroy();
            }
        }
        void operator()() noexcept {}
    };

    // asio :763-774：被丢弃的执行线程经 executor 展开（关闭期间 post 立即销毁，即内联展开）。
    void unwind_through_executor() noexcept {
        entry_frame_base<Executor>* const entry = bottom_of_stack_;
        bottom_of_stack_ = nullptr;
        Executor ex = entry->executor_;
        ex.post(entry_owner{entry});
    }

    entry_frame_base<Executor>* bottom_of_stack_;
};

// 投递一个执行线程：在 executor 上重新 attach 并 pump。
template <class Executor> struct resume_thread_op {
    awaitable_thread<Executor> thread;
    void operator()() {
        thread.frame()->attach_thread(&thread);
        thread.pump();
    }
};

} // namespace detail
} // namespace aw
```

`include/awaitable/impl/entry_point.hpp`（整文件替换）：

```cpp
#pragma once

#include <atomic>
#include <exception>
#include <new>
#include <type_traits>
#include <utility>

#include "awaitable/associated.hpp"
#include "awaitable/awaitable.hpp"
#include "awaitable/cancel/cancellation_signal.hpp"
#include "awaitable/core/contract.hpp"
#include "awaitable/exec/detail/recycling_allocator.hpp"
#include "awaitable/exec/detail/thread_context.hpp"
#include "awaitable/optional.hpp"

namespace aw {
namespace detail {

template <class T> struct spawn_value {
    using type = typename std::conditional<std::is_default_constructible<T>::value, T,
                                           aw::optional<T>>::type;
};
template <class T> using spawn_value_t = typename spawn_value<T>::type;

template <class T> struct spawn_signature { using type = void(std::exception_ptr, spawn_value_t<T>); };
template <> struct spawn_signature<void> { using type = void(std::exception_ptr); };

template <class T> struct spawn_result {
    using value_type = spawn_value_t<T>;
    aw::optional<T> value;
    template <class F> void capture(F&& get) { value.emplace(get()); }
    value_type take() {
        if (value) return value_type(std::move(*value));
        return value_type();
    }
};

template <class Handler, class V> struct deliver_op {
    Handler handler;
    std::exception_ptr e;
    V v;
    void operator()() { handler(e, std::move(v)); }
};
template <class Handler> struct deliver_void_op {
    Handler handler;
    std::exception_ptr e;
    void operator()() { handler(e); }
};

// §8.2：token 带关联 slot 时的取消桥。slot 处理器和入口帧各持一个引用；处理器可能在
// 执行线程结束后才运行，所以 signal 住在这个引用计数块里（asio impl/co_spawn.hpp:240-291）。
class spawn_cancellation_bridge {
  public:
    static spawn_cancellation_bridge* create() {
        void* const memory = recycling_allocate(mem_tag::cancellation_signal,
                                                sizeof(spawn_cancellation_bridge),
                                                alignof(spawn_cancellation_bridge));
        return ::new (memory) spawn_cancellation_bridge;
    }

    void add_ref() noexcept { refs_.fetch_add(1U, std::memory_order_relaxed); }
    void release() noexcept {
        if (refs_.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
            this->~spawn_cancellation_bridge();
            recycling_deallocate(mem_tag::cancellation_signal, this, sizeof(spawn_cancellation_bridge));
        }
    }

    cancellation_signal& signal() noexcept { return signal_; }

  private:
    spawn_cancellation_bridge() noexcept = default;
    spawn_cancellation_bridge(spawn_cancellation_bridge const&) = delete;
    spawn_cancellation_bridge& operator=(spawn_cancellation_bridge const&) = delete;
    ~spawn_cancellation_bridge() = default;

    std::atomic<unsigned> refs_{1U};
    cancellation_signal signal_;
};

struct bridge_ref {
    spawn_cancellation_bridge* bridge;
    explicit bridge_ref(spawn_cancellation_bridge* b) noexcept : bridge{b} {}
    bridge_ref(bridge_ref&& other) noexcept : bridge{other.bridge} { other.bridge = nullptr; }
    bridge_ref(bridge_ref const&) = delete;
    bridge_ref& operator=(bridge_ref const&) = delete;
    ~bridge_ref() {
        if (bridge != nullptr) bridge->release();
    }
};

// 在协程的 executor 上 emit（§6.2 第 7 条）。
struct spawn_emit_op {
    bridge_ref ref;
    cancellation_type type;
    void operator()() { ref.bridge->signal().emit(type); }
};

// 装在 token 的 slot 上：可以在任何线程被 emit，先 dispatch 到 ex。
template <class Executor> struct spawn_cancellation_handler {
    bridge_ref ref;
    Executor ex;
    spawn_cancellation_handler(spawn_cancellation_bridge* bridge, Executor const& e) noexcept
        : ref{bridge}, ex{e} {
        bridge->add_ref();
    }
    void operator()(cancellation_type type) {
        ref.bridge->add_ref();
        ex.dispatch(spawn_emit_op{bridge_ref{ref.bridge}, type});
    }
};

// §7.2：手写入口帧。状态机：开始（压入用户帧）→ 运行中 → [启动期完成：先 post] → 交付。
template <class Executor, class T, class Handler>
class entry_frame_impl final : public entry_frame_base<Executor> {
    using base = entry_frame_base<Executor>;

  public:
    template <class H>
    static entry_frame_impl* create(Executor const& ex, awaitable<T, Executor> user, H&& handler) {
        void* const memory = recycling_allocate(mem_tag::awaitable_frame, sizeof(entry_frame_impl),
                                                alignof(entry_frame_impl));
        try {
            return ::new (memory) entry_frame_impl(ex, std::move(user), std::forward<H>(handler));
        } catch (...) {
            recycling_deallocate(mem_tag::awaitable_frame, memory, sizeof(entry_frame_impl));
            throw;
        }
    }

    void hold_bridge(spawn_cancellation_bridge* bridge) {
        bridge_ = bridge;
        this->use_parent_slot(bridge->signal().slot());
    }

  private:
    enum class state : unsigned char { start, running, deliver };

    template <class H>
    entry_frame_impl(Executor const& ex, awaitable<T, Executor> user, H&& handler)
        : base{ex}, user_{std::move(user)}, handler_(std::forward<H>(handler)) {
        this->block_.header = co2::detail::FrameHeader{&destroy_entry, &step_entry, false, false};
    }
    ~entry_frame_impl() {
        if (bridge_ != nullptr) {
            this->parent_slot_.clear();
            this->parent_slot_ = cancellation_slot{};
            bridge_->release();
        }
    }

    static entry_frame_impl* from(co2::detail::FrameHeader* header) noexcept {
        return static_cast<entry_frame_impl*>(
            reinterpret_cast<typename base::header_block*>(header)->self);
    }

    static void destroy_entry(co2::detail::FrameHeader* header) noexcept {
        entry_frame_impl* const self = from(header);
        self->~entry_frame_impl();
        recycling_deallocate(mem_tag::awaitable_frame, self, sizeof(entry_frame_impl));
    }

    static co2::detail::FrameHeader* step_entry(co2::detail::FrameHeader* header) {
        from(header)->step();
        return nullptr;
    }

    void step() {
        switch (state_) {
        case state::start:
            CO2_CONTRACT_CHECK(user_.valid());
            state_ = state::running;
            user_.frame_->push_frame(this);
            return;
        case state::running:
            if (launching_marker() == static_cast<void const*>(static_cast<base*>(this))) {
                state_ = state::deliver;
                this->after_suspend(&reschedule_via_post, this);
                return;
            }
            deliver();
            return;
        case state::deliver:
            deliver();
            return;
        }
    }

    // §7.3 第 4 条：启动期同步完成，先 post 回 executor，再交付。
    static void reschedule_via_post(void* arg) {
        entry_frame_impl* const self = static_cast<entry_frame_impl*>(arg);
        awaitable_thread<Executor>* const thread = self->detach_thread();
        Executor ex = self->executor_;
        ex.post(resume_thread_op<Executor>{awaitable_thread<Executor>{std::move(*thread)}});
    }

    void deliver() { deliver(std::is_void<T>{}); }

    void deliver(std::false_type) {
        std::exception_ptr e;
        spawn_result<T> result;
        {
            awaitable<T, Executor> done{std::move(user_)};
            try {
                result.capture([&]() -> T { return done.frame_->get(); });
            } catch (...) {
                e = std::current_exception();
            }
        } // 用户帧在交付前归还缓存
        using op_type = deliver_op<Handler, spawn_value_t<T>>;
        dispatch_to_handler(op_type{std::move(handler_), e, result.take()});
    }

    void deliver(std::true_type) {
        std::exception_ptr e;
        {
            awaitable<T, Executor> done{std::move(user_)};
            try {
                done.frame_->get();
            } catch (...) {
                e = std::current_exception();
            }
        }
        dispatch_to_handler(deliver_void_op<Handler>{std::move(handler_), e});
    }

    // §7.3 第 5 条：交付到 token 的关联 executor；回调抛出的异常经 pump 的 complete 重抛。
    template <class Op> void dispatch_to_handler(Op&& op) {
        try {
            auto target = get_associated_executor(op.handler, this->executor_);
            target.dispatch(std::forward<Op>(op));
        } catch (...) {
            this->pending_exception_ = std::current_exception();
        }
        this->pop_frame();
    }

    awaitable<T, Executor> user_;
    Handler handler_;
    state state_{state::start};
    spawn_cancellation_bridge* bridge_{nullptr};
};

template <class Executor> struct launch_op {
    awaitable_thread<Executor> thread;
    void operator()() { thread.launch(); }
};

template <class Executor> struct initiate_co_spawn {
    Executor ex;

    template <class Handler, class T> void operator()(Handler&& handler, awaitable<T, Executor> a) const {
        using handler_type = typename std::decay<Handler>::type;
        cancellation_slot const slot = get_associated_cancellation_slot(handler);
        auto* const entry = entry_frame_impl<Executor, T, handler_type>::create(
            ex, std::move(a), std::forward<Handler>(handler));
        awaitable_thread<Executor> thread{entry};
        if (slot.is_connected()) {
            spawn_cancellation_bridge* const bridge = spawn_cancellation_bridge::create();
            entry->hold_bridge(bridge); // 入口帧的引用
            cancellation_slot{slot}.template emplace<spawn_cancellation_handler<Executor>>(bridge, ex);
        }
        // §7.3 第 3 条：先 dispatch 到 ex，再 launch。
        ex.dispatch(launch_op<Executor>{std::move(thread)});
    }
};

} // namespace detail
} // namespace aw
```

- [ ] **Step 4：运行，确认通过（含 ASan 与 TSan）**

Run: `cmake --build build/debug -j8 && ctest --test-dir build/debug -R '^aw-(awaitable|use-awaitable)' --output-on-failure && tools/run-matrix.sh asan-cxx14 tsan-cxx14`
Expected：先输出 `100% tests passed out of 3`，再输出 `asan-cxx14: 100% tests passed out of 44` 和 `tsan-cxx14: 100% tests passed out of 44`。

- [ ] **Step 5：提交**

```bash
git add include/awaitable tests/support tests/unit
git commit -m "feat(cancel): per-entry cancellation state, throw_if_cancelled, co_spawn slot bridge, stop_token"
```

### Task 14：`||` / `&&` 运算符

**交付物：** `aw::awaitable_operators::operator&&` 和 `operator||`（§2.1 组合层、§6.3、§12 第 25 项），基于两个子协程的并行组：
- 两个子协程在父协程的 executor 上 `co_spawn`，各有自己的取消信号；
- `&&` 用 `wait_for_one_error`：任一个失败就取消另一个；
- `||` 用 `wait_for_one_success`：任一个成功就取消另一个；
- 父协程的取消转发给两个子协程；
- 结果形状和异常规则与 asio 相同：`&&` 丢掉 void、左侧 tuple 展开；`||` 的 void 变成 `monostate`、左侧 variant 展开；两个都失败时抛 `multiple_exceptions`（§14.3 第 5 条）；
- 共享状态用 `mem_tag::parallel_group` 回收分配。

**Files:**
- Create: `include/awaitable/impl/parallel_group.hpp`, `include/awaitable/operators.hpp`
- Test: `tests/unit/operators_test.cpp`
- Modify: `tests/unit/CMakeLists.txt`（追加）

**Interfaces:**
- Consumes：
  - Task 2：`aw::optional`、`aw::variant`、`aw::monostate`、`multiple_exceptions`；
  - Task 7：`cancellation_signal`；
  - Task 10：`co_spawn`、`spawn_value_t`；
  - Task 11：`use_awaitable`；
  - Task 13：父槽转发。
- Produces：
  - `aw::detail::group_condition { one_error, one_success }`；
  - `aw::detail::child_value_t<T>`：T 为 void 时是 `no_value`，否则是 `aw::optional<spawn_value_t<T>>`；还有 `unwrap_child`；
  - `aw::detail::pair_outcome<T, U>`，字段：`first`（先完成的下标）、`e0` / `e1`、`v0` / `v1`；
  - `aw::detail::pair_group_state<Executor, T, U, Handler>`：
    - 一把互斥锁保护 `spawned` / `cancel_pending` / `cancel_claimed`；
    - 原子 `remaining = 3`（两个子协程加启动方）；
    - `forward_cancel` 挂在父槽上；`child_done<I>`；
    - 结果用 `dispatch` 交给 handler 的 executor；
  - `aw::detail::async_pair_group(cond, awaitable<T,E>, awaitable<U,E>, token)`，完成签名 `void(pair_outcome<T, U>)`；
  - `aw::detail::and_join<T, U>::type` 和 `aw::detail::or_join<T, U>::type`；
  - `aw::awaitable_operators::operator&&(awaitable<T,E>, awaitable<U,E>)` 和 `operator||(awaitable<T,E>, awaitable<U,E>)`。

- [ ] **Step 1：写失败的测试**

十个场景：
- `&&` 合并值；`&&` 一方失败时取消另一方；`&&` 只有一方失败时原样抛出；`&&` 两方都失败时抛 `multiple_exceptions`；
- `||` 的赢家取消输家；`||` 一方失败时等另一方；`||` 两方都失败时抛 `multiple_exceptions`；`||` 的结果形状；
- 父协程取消到达两个子协程；不可默认构造的值。

`tests/unit/operators_test.cpp`：

```cpp
#include <exception>
#include <future>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>

#include "awaitable/associated.hpp"
#include "awaitable/awaitable.hpp"
#include "awaitable/cancel/cancellation_signal.hpp"
#include "awaitable/co_spawn.hpp"
#include "awaitable/multiple_exceptions.hpp"
#include "awaitable/operators.hpp"
#include "awaitable/use_awaitable.hpp"
#include "awaitable/use_future.hpp"
#include "awaitable/variant.hpp"
#include "check.hpp"
#include "test_cancellable_ops.hpp"

namespace {

using namespace aw::awaitable_operators;
using ex_t = aw::manual_executor::executor_type;
template <class T> using task = aw::awaitable<T, ex_t>;
using event_t = aw_test::manual_event<ex_t>;

auto value_after_post(ex_t ex, int v) CO2_BEG(task<int>, (ex, v), int r{};) {
    CO2_AWAIT_SET(r, aw_test::async_value(ex, v, aw::use_awaitable));
    CO2_RETURN(r);
}
CO2_END

auto text(ex_t ex, std::string s) CO2_BEG(task<std::string>, (ex, s)) {
    CO2_AWAIT(aw_test::async_nothing(ex, aw::use_awaitable));
    CO2_RETURN(s);
}
CO2_END

auto nothing(ex_t ex) CO2_BEG(task<void>, (ex)) {
    CO2_AWAIT(aw_test::async_nothing(ex, aw::use_awaitable));
    CO2_RETURN();
}
CO2_END

auto fail_after_post(ex_t ex, char const* what) CO2_BEG(task<int>, (ex, what)) {
    CO2_AWAIT(aw_test::async_nothing(ex, aw::use_awaitable));
    throw std::runtime_error{what};
}
CO2_END

// 等事件；被取消时把 operation_aborted 作为异常抛出。
auto wait_event(event_t* ev, int v) CO2_BEG(task<int>, (ev, v)) {
    CO2_AWAIT(ev->async_wait(aw::use_awaitable));
    CO2_RETURN(v);
}
CO2_END

// 不可默认构造的值类型：co_spawn 交付 optional<T>，运算符要剥掉一层。
struct no_default {
    explicit no_default(int x) : v{x} {}
    int v;
};
auto make_no_default(ex_t ex, int v) CO2_BEG(task<no_default>, (ex, v)) {
    CO2_AWAIT(aw_test::async_nothing(ex, aw::use_awaitable));
    CO2_RETURN(no_default{v});
}
CO2_END

template <class T> T run(aw::manual_executor& ctx, task<T> a) {
    auto f = aw::co_spawn(ctx, std::move(a), aw::use_future);
    ctx.run();
    return f.get();
}

std::string message_of(std::exception_ptr e) {
    try {
        std::rethrow_exception(e);
    } catch (std::exception const& x) {
        return x.what();
    }
}

auto and_values(ex_t ex) CO2_BEG(task<int>, (ex), std::tuple<int, std::string> p{}; int n{};
                                 std::tuple<int, int, int> three{};) {
    CO2_AWAIT_SET(p, value_after_post(ex, 1) && text(ex, "two"));
    if (std::get<0>(p) != 1 || std::get<1>(p) != "two") CO2_RETURN(-1);
    CO2_AWAIT_SET(n, nothing(ex) && value_after_post(ex, 5));
    if (n != 5) CO2_RETURN(-2);
    CO2_AWAIT_SET(n, value_after_post(ex, 6) && nothing(ex));
    if (n != 6) CO2_RETURN(-3);
    CO2_AWAIT(nothing(ex) && nothing(ex));
    CO2_AWAIT_SET(three, value_after_post(ex, 1) && value_after_post(ex, 2) && value_after_post(ex, 3));
    CO2_RETURN(std::get<0>(three) + std::get<1>(three) + std::get<2>(three));
}
CO2_END

void and_combines_values() {
    aw::manual_executor ctx;
    AW_CHECK(run(ctx, and_values(ctx.get_executor())) == 6);
}

void and_failure_cancels_the_other() {
    aw::manual_executor ctx;
    event_t ev{ctx.get_executor()};
    auto f = aw::co_spawn(ctx, wait_event(&ev, 1) && fail_after_post(ctx.get_executor(), "boom"), aw::use_future);
    ctx.run();
    AW_CHECK(ev.last_cancel() == aw::cancellation_type::terminal);
    try {
        static_cast<void>(f.get());
        AW_CHECK(false);
    } catch (aw::multiple_exceptions const& m) {
        // 被取消的一边以 operation_aborted 失败，两边都失败：带第一个操作数的异常。
        AW_CHECK(message_of(m.first_exception()).find("aborted") != std::string::npos ||
                 message_of(m.first_exception()).find("canceled") != std::string::npos ||
                 message_of(m.first_exception()).find("cancelled") != std::string::npos);
    }
}

void and_single_failure_rethrows_it() {
    aw::manual_executor ctx;
    auto f = aw::co_spawn(ctx, fail_after_post(ctx.get_executor(), "left") && value_after_post(ctx.get_executor(), 2),
                          aw::use_future);
    ctx.run();
    try {
        static_cast<void>(f.get());
        AW_CHECK(false);
    } catch (std::runtime_error const& e) {
        AW_CHECK(std::string{e.what()} == "left");
    }
}

void and_both_fail_multiple_exceptions() {
    aw::manual_executor ctx;
    auto f = aw::co_spawn(ctx, fail_after_post(ctx.get_executor(), "a") && fail_after_post(ctx.get_executor(), "b"),
                          aw::use_future);
    ctx.run();
    try {
        static_cast<void>(f.get());
        AW_CHECK(false);
    } catch (aw::multiple_exceptions const& m) {
        AW_CHECK(message_of(m.first_exception()) == "a");
    }
}

void or_winner_cancels_loser() {
    aw::manual_executor ctx;
    event_t ev{ctx.get_executor()};
    auto f = aw::co_spawn(ctx, wait_event(&ev, 1) || value_after_post(ctx.get_executor(), 2), aw::use_future);
    ctx.run();
    AW_CHECK(ev.last_cancel() == aw::cancellation_type::terminal);
    auto r = f.get();
    AW_CHECK(r.index() == 1U && aw::get<1>(r) == 2);
}

void or_failure_falls_back_to_other() {
    aw::manual_executor ctx;
    event_t ev{ctx.get_executor()};
    auto f = aw::co_spawn(ctx, fail_after_post(ctx.get_executor(), "x") || wait_event(&ev, 7), aw::use_future);
    ctx.run();
    AW_CHECK(ev.waiting()); // 失败不触发取消
    ev.set();
    ctx.run();
    auto r = f.get();
    AW_CHECK(r.index() == 1U && aw::get<1>(r) == 7);
}

void or_both_fail_multiple_exceptions() {
    aw::manual_executor ctx;
    auto f = aw::co_spawn(ctx, fail_after_post(ctx.get_executor(), "a") || fail_after_post(ctx.get_executor(), "b"),
                          aw::use_future);
    ctx.run();
    try {
        static_cast<void>(f.get());
        AW_CHECK(false);
    } catch (aw::multiple_exceptions const& m) {
        AW_CHECK(message_of(m.first_exception()) == "a");
    }
}

auto or_shapes(ex_t ex) CO2_BEG(task<int>, (ex), aw::variant<aw::monostate, int> a{};
                                aw::variant<int, aw::monostate> b{}; aw::variant<int, std::string, int> c{};
                                aw::variant<aw::monostate, aw::monostate> d{};) {
    CO2_AWAIT_SET(a, nothing(ex) || value_after_post(ex, 3));
    if (a.index() != 0U) CO2_RETURN(-1);
    CO2_AWAIT_SET(b, value_after_post(ex, 4) || nothing(ex));
    if (b.index() != 0U || aw::get<0>(b) != 4) CO2_RETURN(-2);
    CO2_AWAIT_SET(c, fail_after_post(ex, "f") || text(ex, "t") || value_after_post(ex, 9));
    if (c.index() != 1U || aw::get<1>(c) != "t") CO2_RETURN(-3);
    CO2_AWAIT_SET(d, nothing(ex) || nothing(ex));
    CO2_RETURN(static_cast<int>(d.index()) + 10);
}
CO2_END

void or_result_shapes() {
    aw::manual_executor ctx;
    AW_CHECK(run(ctx, or_shapes(ctx.get_executor())) == 10);
}

void parent_cancellation_reaches_both_children() {
    aw::manual_executor ctx;
    event_t e1{ctx.get_executor()};
    event_t e2{ctx.get_executor()};
    aw::cancellation_signal sig;
    std::exception_ptr got;
    bool called = false;
    aw::co_spawn(ctx, wait_event(&e1, 1) || wait_event(&e2, 2),
                 aw::bind_cancellation_slot(sig.slot(), [&](std::exception_ptr e, aw::variant<int, int>) {
                     called = true;
                     got = e;
                 }));
    ctx.run();
    AW_CHECK(e1.waiting() && e2.waiting());
    sig.emit(aw::cancellation_type::terminal);
    ctx.run();
    AW_CHECK(e1.last_cancel() == aw::cancellation_type::terminal);
    AW_CHECK(e2.last_cancel() == aw::cancellation_type::terminal);
    AW_CHECK(called && got);
}

auto non_default(ex_t ex) CO2_BEG(task<int>, (ex), std::tuple<no_default, int> p{no_default{0}, 0};) {
    CO2_AWAIT_SET(p, make_no_default(ex, 4) && value_after_post(ex, 5));
    CO2_RETURN(std::get<0>(p).v + std::get<1>(p));
}
CO2_END

void non_default_constructible_values() {
    aw::manual_executor ctx;
    AW_CHECK(run(ctx, non_default(ctx.get_executor())) == 9);
}

} // namespace

int main() {
    and_combines_values();
    and_failure_cancels_the_other();
    and_single_failure_rethrows_it();
    and_both_fail_multiple_exceptions();
    or_winner_cancels_loser();
    or_failure_falls_back_to_other();
    or_both_fail_multiple_exceptions();
    or_result_shapes();
    parent_cancellation_reaches_both_children();
    non_default_constructible_values();
    return 0;
}
```

`tests/unit/CMakeLists.txt` 追加：

```cmake
aw_unit_test(operators_test)
```

- [ ] **Step 2：运行，确认失败**

Run: `cmake --build build/debug --target aw-operators-test 2>&1 | grep -m1 "error:"`
Expected：包含 `'awaitable/operators.hpp' file not found`

- [ ] **Step 3：最小实现**

`include/awaitable/impl/parallel_group.hpp`：

```cpp
#pragma once

#include <atomic>
#include <exception>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>

#include "awaitable/associated.hpp"
#include "awaitable/awaitable.hpp"
#include "awaitable/cancel/cancellation_signal.hpp"
#include "awaitable/cancel/cancellation_type.hpp"
#include "awaitable/co_spawn.hpp"
#include "awaitable/exec/detail/recycling_allocator.hpp"
#include "awaitable/impl/entry_point.hpp"
#include "awaitable/optional.hpp"

namespace aw {
namespace detail {

// §12 第 25、26 行：两个操作数各开一个子执行线程，加一个手写组状态（mem_tag::parallel_group）。
enum class group_condition : unsigned char {
    one_error,   // wait_for_one_error：有一个失败就取消另一个（operator&&）
    one_success, // wait_for_one_success：有一个成功就取消另一个（operator||）
};

struct no_value {};

template <class T> struct child_value { using type = aw::optional<spawn_value_t<T>>; };
template <> struct child_value<void> { using type = no_value; };
template <class T> using child_value_t = typename child_value<T>::type;

// co_spawn 交付 spawn_value_t<T>；T 不可默认构造时那是 optional<T>，这里剥掉一层。
template <class T> T&& unwrap_child(aw::optional<T>& v) { return std::move(*v); }
template <class T> T&& unwrap_child(aw::optional<aw::optional<T>>& v) { return std::move(**v); }

// 组的完成值：先完成者的下标、两个异常、两个值。
template <class T, class U> struct pair_outcome {
    unsigned first{0U};
    std::exception_ptr e0{};
    std::exception_ptr e1{};
    child_value_t<T> v0{};
    child_value_t<U> v1{};
};

template <class Executor, class T, class U, class Handler> class pair_group_state {
  public:
    using outcome_type = pair_outcome<T, U>;

    template <class H> static pair_group_state* create(H&& handler, Executor const& ex, group_condition cond) {
        void* const memory = recycling_allocate(mem_tag::parallel_group, sizeof(pair_group_state),
                                                alignof(pair_group_state));
        try {
            return ::new (memory) pair_group_state(std::forward<H>(handler), ex, cond);
        } catch (...) {
            recycling_deallocate(mem_tag::parallel_group, memory, sizeof(pair_group_state));
            throw;
        }
    }

    // 在执行线程的 executor 上调用（initiation 里）。
    void start(awaitable<T, Executor> t, awaitable<U, Executor> u) {
        co_spawn(ex_, std::move(t), bind_cancellation_slot(signals_[0].slot(), child_done<0U, T>{this}));
        co_spawn(ex_, std::move(u), bind_cancellation_slot(signals_[1].slot(), child_done<1U, U>{this}));
        bool emit_now = false;
        {
            std::lock_guard<std::mutex> lock{mutex_};
            spawned_ = true;
            emit_now = cancel_pending_;
        }
        if (emit_now) emit_others(pending_target_);
        // §6.3：父执行线程收到的取消转给两个子执行线程。
        parent_slot_ = get_associated_cancellation_slot(handler_);
        if (parent_slot_.is_connected()) parent_slot_.template emplace<forward_cancel>(forward_cancel{this});
        finish_one(); // initiation 自己持有的那一份
    }

  private:
    pair_group_state(Handler&& handler, Executor const& ex, group_condition cond)
        : handler_(std::move(handler)), ex_{ex}, cond_{cond} {}

    struct forward_cancel {
        pair_group_state* self;
        void operator()(cancellation_type type) {
            self->signals_[0].emit(type);
            self->signals_[1].emit(type);
        }
    };

    template <unsigned I, class V> struct child_done {
        pair_group_state* self;
        template <class... Vs> void operator()(std::exception_ptr e, Vs&&... v) {
            self->store(std::integral_constant<unsigned, I>{}, std::move(e), std::forward<Vs>(v)...);
            self->decide(I, I == 0U ? !!self->outcome_.e0 : !!self->outcome_.e1);
            self->finish_one();
        }
    };

    void store(std::integral_constant<unsigned, 0U>, std::exception_ptr e) { outcome_.e0 = std::move(e); }
    void store(std::integral_constant<unsigned, 1U>, std::exception_ptr e) { outcome_.e1 = std::move(e); }
    template <class V> void store(std::integral_constant<unsigned, 0U>, std::exception_ptr e, V&& v) {
        outcome_.e0 = std::move(e);
        if (!outcome_.e0) outcome_.v0.emplace(std::forward<V>(v));
    }
    template <class V> void store(std::integral_constant<unsigned, 1U>, std::exception_ptr e, V&& v) {
        outcome_.e1 = std::move(e);
        if (!outcome_.e1) outcome_.v1.emplace(std::forward<V>(v));
    }

    // 第一个满足条件的完成者取消另一个（cancellation_type::all，asio cancellation_condition.hpp:41-69）。
    void decide(unsigned index, bool failed) {
        bool const trigger = cond_ == group_condition::one_error ? failed : !failed;
        bool emit_now = false;
        {
            std::lock_guard<std::mutex> lock{mutex_};
            if (completed_++ == 0U) outcome_.first = index;
            if (trigger && !cancel_claimed_) {
                cancel_claimed_ = true;
                if (spawned_) {
                    emit_now = true;
                } else {
                    cancel_pending_ = true;
                    pending_target_ = index;
                }
            }
        }
        if (emit_now) emit_others(index);
    }

    void emit_others(unsigned winner) { signals_[winner == 0U ? 1U : 0U].emit(cancellation_type::all); }

    void finish_one() {
        if (remaining_.fetch_sub(1U, std::memory_order_acq_rel) != 1U) return;
        // 经回调的关联 executor 交付（§8.1 操作作者契约第 2 条）。
        auto target = get_associated_executor(handler_, ex_);
        target.dispatch(deliver{this});
    }

    struct deliver {
        pair_group_state* self;
        void operator()() {
            if (self->parent_slot_.is_connected()) self->parent_slot_.clear();
            Handler handler(std::move(self->handler_));
            outcome_type outcome(std::move(self->outcome_));
            self->~pair_group_state();
            recycling_deallocate(mem_tag::parallel_group, self, sizeof(pair_group_state));
            handler(std::move(outcome));
        }
    };

    Handler handler_;
    Executor ex_;
    group_condition cond_;
    cancellation_signal signals_[2];
    cancellation_slot parent_slot_{};
    outcome_type outcome_{};
    std::atomic<unsigned> remaining_{3U}; // 两个子执行线程 + initiation
    std::mutex mutex_;
    unsigned completed_{0U};
    bool spawned_{false};
    bool cancel_claimed_{false};
    bool cancel_pending_{false};
    unsigned pending_target_{0U};
};

template <class Executor> struct initiate_pair_group {
    group_condition cond;
    template <class Handler, class T, class U>
    void operator()(Handler&& handler, awaitable<T, Executor> t, awaitable<U, Executor> u) const {
        using handler_type = typename std::decay<Handler>::type;
        using state_type = pair_group_state<Executor, T, U, handler_type>;
        Executor const ex = handler.get_executor(); // 只配合 use_awaitable：回调就是执行线程
        state_type* const state = state_type::create(std::forward<Handler>(handler), ex, cond);
        state->start(std::move(t), std::move(u));
    }
};

template <class Executor, class T, class U, class Token>
auto async_pair_group(group_condition cond, awaitable<T, Executor> t, awaitable<U, Executor> u, Token&& token) {
    return async_initiate<Token, void(pair_outcome<T, U>)>(initiate_pair_group<Executor>{cond}, token,
                                                           std::move(t), std::move(u));
}

} // namespace detail
} // namespace aw
```

`include/awaitable/operators.hpp`：

```cpp
#pragma once

#include <cstddef>
#include <exception>
#include <tuple>
#include <type_traits>
#include <utility>

#include "awaitable/awaitable.hpp"
#include "awaitable/core/dsl.hpp"
#include "awaitable/impl/parallel_group.hpp"
#include "awaitable/multiple_exceptions.hpp"
#include "awaitable/use_awaitable.hpp"
#include "awaitable/variant.hpp"

// §12 第 25 行：a && b 是 wait_for_one_error，a || b 是 wait_for_one_success（asio
// experimental/awaitable_operators.hpp:75-300）。用法：using namespace aw::awaitable_operators;
namespace aw {
namespace detail {

// ---- && 的结果类型：void 消失；左边是 tuple 时展平 ----
template <class T, class U> struct and_join {
    using type = std::tuple<T, U>;
    template <class O> static type make(O& o) { return type(unwrap_child(o.v0), unwrap_child(o.v1)); }
};
template <> struct and_join<void, void> {
    using type = void;
};
template <class T> struct and_join<T, void> {
    using type = T;
    template <class O> static type make(O& o) { return unwrap_child(o.v0); }
};
template <class U> struct and_join<void, U> {
    using type = U;
    template <class O> static type make(O& o) { return unwrap_child(o.v1); }
};
template <class... Ts> struct and_join<std::tuple<Ts...>, void> {
    using type = std::tuple<Ts...>;
    template <class O> static type make(O& o) { return unwrap_child(o.v0); }
};
template <class... Ts, class U> struct and_join<std::tuple<Ts...>, U> {
    using type = std::tuple<Ts..., U>;
    template <class O> static type make(O& o) {
        return std::tuple_cat(unwrap_child(o.v0), std::tuple<U>(unwrap_child(o.v1)));
    }
};

// ---- || 的结果类型：void 变成 monostate；左边是 variant 时展平 ----
template <class T> struct or_alt { using type = T; };
template <> struct or_alt<void> { using type = aw::monostate; };

template <class R, std::size_t Offset, class O> R or_take_right(O&, std::true_type /*void*/) {
    return R(aw::in_place_index<Offset>());
}
template <class R, std::size_t Offset, class O> R or_take_right(O& o, std::false_type) {
    return R(aw::in_place_index<Offset>(), unwrap_child(o.v1));
}

template <class T, class U> struct or_join {
    using type = aw::variant<typename or_alt<T>::type, typename or_alt<U>::type>;
    template <class O> static type left(O& o) { return left(o, std::is_void<T>{}); }
    template <class O> static type right(O& o) { return or_take_right<type, 1U>(o, std::is_void<U>{}); }

  private:
    template <class O> static type left(O&, std::true_type) { return type(aw::in_place_index<0U>()); }
    template <class O> static type left(O& o, std::false_type) {
        return type(aw::in_place_index<0U>(), unwrap_child(o.v0));
    }
};

template <class R, class Src> struct variant_rewrap;
template <class R, class... Ts> struct variant_rewrap<R, aw::variant<Ts...>> {
    template <std::size_t I> static R go(aw::variant<Ts...>& v, std::integral_constant<std::size_t, I>) {
        if (v.index() == I) return R(aw::in_place_index<I>(), aw::get<I>(std::move(v)));
        return go(v, std::integral_constant<std::size_t, I + 1U>{});
    }
    static R go(aw::variant<Ts...>& v, std::integral_constant<std::size_t, sizeof...(Ts) - 1U>) {
        return R(aw::in_place_index<sizeof...(Ts) - 1U>(), aw::get<sizeof...(Ts) - 1U>(std::move(v)));
    }
};

template <class... Ts, class U> struct or_join<aw::variant<Ts...>, U> {
    using type = aw::variant<Ts..., typename or_alt<U>::type>;
    template <class O> static type left(O& o) {
        return variant_rewrap<type, aw::variant<Ts...>>::go(unwrap_child_ref(o.v0),
                                                            std::integral_constant<std::size_t, 0U>{});
    }
    template <class O> static type right(O& o) {
        return or_take_right<type, sizeof...(Ts)>(o, std::is_void<U>{});
    }

  private:
    template <class V> static V& unwrap_child_ref(aw::optional<V>& v) { return *v; }
    template <class V> static V& unwrap_child_ref(aw::optional<aw::optional<V>>& v) { return **v; }
};

inline void rethrow_and(std::exception_ptr const& e0, std::exception_ptr const& e1) {
    if (e0 && e1) throw multiple_exceptions(e0);
    if (e0) std::rethrow_exception(e0);
    if (e1) std::rethrow_exception(e1);
}

template <class T, class U, class Executor>
auto and_impl(awaitable<T, Executor> t, awaitable<U, Executor> u)
    CO2_BEG((awaitable<typename and_join<T, U>::type, Executor>), (t, u), pair_outcome<T, U> out{};) {
    CO2_AWAIT_SET(out, async_pair_group(group_condition::one_error, std::move(t), std::move(u), use_awaitable));
    rethrow_and(out.e0, out.e1);
    CO2_RETURN(and_join<T, U>::make(out));
}
CO2_END

template <class T, class U, class Executor>
auto and_void_impl(awaitable<T, Executor> t, awaitable<U, Executor> u)
    CO2_BEG((awaitable<void, Executor>), (t, u), pair_outcome<T, U> out{};) {
    CO2_AWAIT_SET(out, async_pair_group(group_condition::one_error, std::move(t), std::move(u), use_awaitable));
    rethrow_and(out.e0, out.e1);
    CO2_RETURN();
}
CO2_END

// asio：先看先完成者；它失败再看另一个；都失败抛 multiple_exceptions(先完成者的异常)。
template <class T, class U, class Executor>
auto or_impl(awaitable<T, Executor> t, awaitable<U, Executor> u)
    CO2_BEG((awaitable<typename or_join<T, U>::type, Executor>), (t, u), pair_outcome<T, U> out{};) {
    CO2_AWAIT_SET(out, async_pair_group(group_condition::one_success, std::move(t), std::move(u), use_awaitable));
    if (out.first == 0U) {
        if (!out.e0) CO2_RETURN(or_join<T, U>::left(out));
        if (!out.e1) CO2_RETURN(or_join<T, U>::right(out));
        throw multiple_exceptions(out.e0);
    }
    if (!out.e1) CO2_RETURN(or_join<T, U>::right(out));
    if (!out.e0) CO2_RETURN(or_join<T, U>::left(out));
    throw multiple_exceptions(out.e1);
}
CO2_END

template <class T, class U> struct and_dispatch {
    template <class E> static auto run(awaitable<T, E> t, awaitable<U, E> u) {
        return and_impl(std::move(t), std::move(u));
    }
};
template <> struct and_dispatch<void, void> {
    template <class E> static auto run(awaitable<void, E> t, awaitable<void, E> u) {
        return and_void_impl(std::move(t), std::move(u));
    }
};

} // namespace detail

namespace awaitable_operators {

template <class T, class U, class Executor>
awaitable<typename detail::and_join<T, U>::type, Executor> operator&&(awaitable<T, Executor> t,
                                                                     awaitable<U, Executor> u) {
    return detail::and_dispatch<T, U>::run(std::move(t), std::move(u));
}

template <class T, class U, class Executor>
awaitable<typename detail::or_join<T, U>::type, Executor> operator||(awaitable<T, Executor> t,
                                                                    awaitable<U, Executor> u) {
    return detail::or_impl(std::move(t), std::move(u));
}

} // namespace awaitable_operators
} // namespace aw
```

- [ ] **Step 4：运行，确认通过（含 ASan 与 TSan）**

Run: `cmake --build build/debug -j8 && ctest --test-dir build/debug -R '^aw-operators' --output-on-failure && tools/run-matrix.sh asan-cxx14 tsan-cxx14`
Expected：先输出 `100% tests passed out of 1`，再输出 `asan-cxx14: 100% tests passed out of 45` 和 `tsan-cxx14: 100% tests passed out of 45`。

- [ ] **Step 5：提交**

```bash
git add include/awaitable tests/unit
git commit -m "feat(operators): awaitable || and && over a two-child parallel group"
```

### Task 15：async_generator

**交付物：** §10 的 `aw::async_generator<T, Executor>`。生成器在消费者的执行线程上运行：`next()` 把生成器帧压到消费者帧之上，`CO2_YIELD` 时出栈并把值交给消费者（§10.2）。结束时 `next()` 得到空的 `aw::optional<T>`。需要满足：
- 稳态 yield 零分配；
- 生成器体内的异常在消费者的 `next()` 处抛出；
- 销毁停在 yield 处或尚未启动的生成器时，帧被正确销毁；
- 同一时刻有两个未完成的 `next()`，或者销毁正在运行的生成器，都属于契约违反（§10.2 第 6、7 条）；
- 左值 yield 拷贝出去，右值 yield 移动出去。规格 §10.2 第 2 条写的是从指针 move，这里对左值改成拷贝，避免掏空生成器体内仍可见的变量（见计划头部“与规格的已知偏差”）。

**Files:**
- Create: `include/awaitable/impl/generator_frame.hpp`, `include/awaitable/async_generator.hpp`
- Test: `tests/unit/async_generator_test.cpp`, `tests/unit/async_generator_two_next_contract_test.cpp`, `tests/unit/async_generator_destroy_active_contract_test.cpp`
- Modify: `tests/unit/CMakeLists.txt`（追加）

**Interfaces:**
- Consumes：
  - Task 2：`aw::optional`；
  - Task 3：`tests/support/counting_new.hpp`；
  - Task 10：`awaitable_frame_base<E>` 的 `push_frame` / `pop_frame` / `rethrow_exception`、`aw_awaitable_tag`；
  - co2 core：`CO2_CONTRACT_CHECK`、`co2::setContractViolationHandler`。
- Produces：
  - `template <class T, class Executor> class aw::async_generator`：
    - 只可移动，`promise_type = detail::generator_frame<T, Executor>`；
    - `next_awaiter next() noexcept`；`next_awaiter` 带 `aw_awaitable_tag`，`await_resume()` 返回 `aw::optional<T>`；
    - `reset()` / 析构：契约要求生成器没有在运行（`!active_`）。
  - `aw::detail::generator_frame<T, Executor>`，派生自 `awaitable_frame_base<Executor>`：
    - `get_return_object`、`yield_value(T const&)`（保存指针）、`yield_value(T&&)`（保存到 optional）、`return_void`、`final_suspend`、`aw::optional<T> take()`；
    - 成员 `finished_`、`active_`。

- [ ] **Step 1：写失败的测试**

两个契约测试都设置 `co2::setContractViolationHandler`，让它以 `_Exit(86)` 退出，由 `aw_contract_test` 检查退出码。

`tests/unit/async_generator_test.cpp`：

```cpp
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "awaitable/async_generator.hpp"
#include "awaitable/awaitable.hpp"
#include "awaitable/cancel/stop_token_adapter.hpp"
#include "awaitable/co_spawn.hpp"
#include "awaitable/this_coro.hpp"
#include "awaitable/use_awaitable.hpp"
#include "awaitable/use_future.hpp"
#include "check.hpp"
#include "counting_new.hpp"
#include "test_ops.hpp"

namespace {

using ex_t = aw::manual_executor::executor_type;
template <class T> using task = aw::awaitable<T, ex_t>;
template <class T> using gen = aw::async_generator<T, ex_t>;

auto numbers(int n) CO2_BEG((gen<int>), (n), int i{};) {
    for (i = 0; i != n; ++i) CO2_YIELD(i);
    CO2_RETURN();
}
CO2_END

// 生成器体内等待异步操作：走与 awaitable 相同的桥接。
auto async_numbers(ex_t ex, int n) CO2_BEG((gen<int>), (ex, n), int i{}; int v{};) {
    for (i = 0; i != n; ++i) {
        CO2_AWAIT_SET(v, aw_test::async_value(ex, i * 10, aw::use_awaitable));
        CO2_YIELD(v);
    }
    CO2_RETURN();
}
CO2_END

auto words() CO2_BEG((gen<std::string>), ()) {
    CO2_YIELD(std::string{"a"});
    CO2_YIELD(std::string{"b"});
    CO2_RETURN();
}
CO2_END

auto throws_after_one() CO2_BEG((gen<int>), ()) {
    CO2_YIELD(1);
    throw std::runtime_error{"gen"};
}
CO2_END

// 生成器看到的 executor 是消费者执行线程的 executor。
auto executor_probe(ex_t expected, bool* same) CO2_BEG((gen<int>), (expected, same), ex_t ex{};) {
    CO2_AWAIT_SET(ex, aw::this_coro::executor);
    *same = ex == expected;
    CO2_YIELD(0);
    CO2_RETURN();
}
CO2_END

auto sum(gen<int> g) CO2_BEG(task<int>, (g), aw::optional<int> v{}; int total{};) {
    for (;;) {
        CO2_AWAIT_SET(v, g.next());
        if (!v) break;
        total += *v;
    }
    CO2_RETURN(total);
}
CO2_END

auto concat(gen<std::string> g) CO2_BEG(task<std::string>, (g), aw::optional<std::string> v{}; std::string s{};) {
    for (;;) {
        CO2_AWAIT_SET(v, g.next());
        if (!v) break;
        s += *v;
    }
    CO2_RETURN(s);
}
CO2_END

auto first_then_error(gen<int> g) CO2_BEG(task<int>, (g), aw::optional<int> v{};) {
    CO2_AWAIT_SET(v, g.next());
    if (!v || *v != 1) CO2_RETURN(-1);
    CO2_AWAIT_SET(v, g.next()); // 重抛 "gen"
    CO2_RETURN(-2);
}
CO2_END

// 只取前两个，然后丢弃停在 yield 处的生成器。
auto take_two(gen<int> g) CO2_BEG(task<int>, (g), aw::optional<int> a{}; aw::optional<int> b{};) {
    CO2_AWAIT_SET(a, g.next());
    CO2_AWAIT_SET(b, g.next());
    CO2_RETURN(*a + *b);
}
CO2_END

template <class T> T run(aw::manual_executor& ctx, task<T> a) {
    auto f = aw::co_spawn(ctx, std::move(a), aw::use_future);
    ctx.run();
    return f.get();
}

void sums() {
    aw::manual_executor ctx;
    AW_CHECK(run(ctx, sum(numbers(5))) == 10);
    AW_CHECK(run(ctx, sum(async_numbers(ctx.get_executor(), 4))) == 60);
    AW_CHECK(run(ctx, sum(numbers(0))) == 0);
}

void rvalues_are_moved() { 
    aw::manual_executor ctx;
    AW_CHECK(run(ctx, concat(words())) == "ab");
}

void body_exception_reaches_consumer() {
    aw::manual_executor ctx;
    auto f = aw::co_spawn(ctx, first_then_error(throws_after_one()), aw::use_future);
    ctx.run();
    try {
        static_cast<void>(f.get());
        AW_CHECK(false);
    } catch (std::runtime_error const& e) {
        AW_CHECK(std::string{e.what()} == "gen");
    }
}

void destroy_at_yield_and_unstarted() {
    aw::manual_executor ctx;
    AW_CHECK(run(ctx, take_two(numbers(100))) == 1);
    { gen<int> unstarted = numbers(3); }
}

void generator_shares_consumer_executor() {
    aw::manual_executor ctx;
    bool same = false;
    AW_CHECK(run(ctx, sum(executor_probe(ctx.get_executor(), &same))) == 0);
    AW_CHECK(same);
}

// §9.3：稳态下一次 yield 零分配。
auto count_yields(gen<int> g, long* news) CO2_BEG(task<int>, (g, news), aw::optional<int> v{}; int n{}; long before{};) {
    CO2_AWAIT_SET(v, g.next());
    before = aw_test::new_calls().load();
    for (;;) {
        CO2_AWAIT_SET(v, g.next());
        if (!v) break;
        ++n;
    }
    *news = aw_test::new_calls().load() - before;
    CO2_RETURN(n);
}
CO2_END

void yield_does_not_allocate() {
    aw::manual_executor ctx;
    long news = -1;
    AW_CHECK(run(ctx, count_yields(numbers(1001), &news)) == 1000);
    AW_CHECK(news == 0);
}

} // namespace

int main() {
    sums();
    rvalues_are_moved();
    body_exception_reaches_consumer();
    destroy_at_yield_and_unstarted();
    generator_shares_consumer_executor();
    yield_does_not_allocate();
    return 0;
}
```

`tests/unit/async_generator_two_next_contract_test.cpp`：

```cpp
// §10.2 第 7 条：同一个生成器同时有两个 next() 是契约违规（退出码 86）。
#include <cstdlib>

#include "awaitable/async_generator.hpp"
#include "awaitable/awaitable.hpp"
#include "awaitable/co_spawn.hpp"
#include "awaitable/core/contract.hpp"
#include "awaitable/detached.hpp"
#include "awaitable/use_awaitable.hpp"
#include "test_cancellable_ops.hpp"

namespace {

using ex_t = aw::manual_executor::executor_type;
using gen_t = aw::async_generator<int, ex_t>;
using event_t = aw_test::manual_event<ex_t>;

auto slow(event_t* ev) CO2_BEG((gen_t), (ev)) {
    CO2_AWAIT(ev->async_wait(aw::use_awaitable));
    CO2_YIELD(1);
    CO2_RETURN();
}
CO2_END

auto pull(gen_t* g) CO2_BEG((aw::awaitable<void, ex_t>), (g), aw::optional<int> v{};) {
    CO2_AWAIT_SET(v, g->next());
    CO2_RETURN();
}
CO2_END

} // namespace

int main() {
    co2::setContractViolationHandler([](co2::ContractViolation const&) { std::_Exit(86); });
    aw::manual_executor ctx;
    event_t ev{ctx.get_executor()};
    gen_t g = slow(&ev);
    aw::co_spawn(ctx, pull(&g), aw::detached);
    aw::co_spawn(ctx, pull(&g), aw::detached);
    ctx.run();
    return 0;
}
```

`tests/unit/async_generator_destroy_active_contract_test.cpp`：

```cpp
// §10.2 第 6 条：销毁正在帧栈中（挂起在异步操作上）的生成器是契约违规（退出码 86）。
#include <cstdlib>

#include "awaitable/async_generator.hpp"
#include "awaitable/awaitable.hpp"
#include "awaitable/co_spawn.hpp"
#include "awaitable/core/contract.hpp"
#include "awaitable/detached.hpp"
#include "awaitable/use_awaitable.hpp"
#include "test_cancellable_ops.hpp"

namespace {

using ex_t = aw::manual_executor::executor_type;
using gen_t = aw::async_generator<int, ex_t>;
using event_t = aw_test::manual_event<ex_t>;

auto slow(event_t* ev) CO2_BEG((gen_t), (ev)) {
    CO2_AWAIT(ev->async_wait(aw::use_awaitable));
    CO2_YIELD(1);
    CO2_RETURN();
}
CO2_END

auto pull(gen_t* g) CO2_BEG((aw::awaitable<void, ex_t>), (g), aw::optional<int> v{};) {
    CO2_AWAIT_SET(v, g->next());
    CO2_RETURN();
}
CO2_END

} // namespace

int main() {
    co2::setContractViolationHandler([](co2::ContractViolation const&) { std::_Exit(86); });
    aw::manual_executor ctx;
    event_t ev{ctx.get_executor()};
    aw::optional<gen_t> holder{slow(&ev)};
    aw::co_spawn(ctx, pull(&*holder), aw::detached);
    ctx.run();
    holder.reset();
    return 0;
}
```

`tests/unit/CMakeLists.txt` 追加：

```cmake
aw_unit_test(async_generator_test)
aw_contract_test(async_generator_two_next_contract_test)
aw_contract_test(async_generator_destroy_active_contract_test)
```

- [ ] **Step 2：运行，确认失败**

Run: `cmake --build build/debug --target aw-async-generator-test 2>&1 | grep -m1 "error:"`
Expected：包含 `'awaitable/async_generator.hpp' file not found`

- [ ] **Step 3：最小实现**

`include/awaitable/impl/generator_frame.hpp`：

```cpp
#pragma once

#include <exception>
#include <type_traits>
#include <utility>

#include "awaitable/core/contract.hpp"
#include "awaitable/core/coroutine.hpp"
#include "awaitable/impl/awaitable_frame.hpp"
#include "awaitable/optional.hpp"

namespace aw {

template <class T, class Executor> class async_generator;

namespace detail {

// §10.2：async_generator 的 promise。与 awaitable 共用 awaitable_frame_base（压栈 / 出栈、
// await_transform、取消、stop_token 适配器、回收缓存）。
template <class T, class Executor> class generator_frame : public awaitable_frame_base<Executor> {
    static_assert(std::is_object<T>::value && !std::is_const<T>::value,
                  "async_generator<T, E>: T must be a non-const object type");

  public:
    async_generator<T, Executor> get_return_object() noexcept;

    // yield：记下值的地址，然后出栈回到消费者（§10.2 第 2 条）。
    struct yield_awaiter {
        generator_frame* self;
        bool await_ready() const noexcept { return false; }
        void await_suspend(co2::coroutine_handle<>) noexcept {
            self->active_ = false;
            self->pop_frame();
        }
        void await_resume() const noexcept {}
    };

    // 左值：只取地址，消费者拷贝构造；右值：移进帧内存储，消费者再移出。
    yield_awaiter yield_value(T const& value) noexcept {
        yielded_ = &value;
        stored_.reset();
        return yield_awaiter{this};
    }
    yield_awaiter yield_value(T&& value) {
        stored_.emplace(std::move(value));
        yielded_ = nullptr;
        return yield_awaiter{this};
    }

    void return_void() noexcept {}

    struct final_awaiter {
        generator_frame* self;
        bool await_ready() const noexcept { return false; }
        void await_suspend(co2::coroutine_handle<>) noexcept {
            self->finished_ = true;
            self->active_ = false;
            self->pop_frame();
        }
        void await_resume() const noexcept {}
    };
    final_awaiter final_suspend() noexcept { return final_awaiter{this}; }

    // 消费者的 await_resume 调用：取出本次的值，或空（结束），或重抛生成器体的异常。
    aw::optional<T> take() {
        if (finished_) {
            this->rethrow_exception();
            return aw::optional<T>{};
        }
        if (yielded_ != nullptr) return aw::optional<T>{*yielded_};
        aw::optional<T> out{std::move(*stored_)};
        stored_.reset();
        return out;
    }

    bool active_{false};
    bool finished_{false};

  private:
    T const* yielded_{nullptr};
    aw::optional<T> stored_{};
};

} // namespace detail
} // namespace aw
```

`include/awaitable/async_generator.hpp`：

```cpp
#pragma once

#include <type_traits>
#include <utility>

#include "awaitable/awaitable.hpp"
#include "awaitable/core/contract.hpp"
#include "awaitable/core/coroutine.hpp"
#include "awaitable/core/dsl.hpp"
#include "awaitable/impl/generator_frame.hpp"
#include "awaitable/optional.hpp"

namespace aw {

// §10.1：CO2_AWAIT_SET(item, gen.next()); item 是 aw::optional<T>，空表示结束。
// 生成器在消费者的执行线程上运行：next() 把生成器帧压到消费者之上，yield 时出栈。
template <class T, class Executor> class async_generator {
  public:
    using value_type = T;
    using executor_type = Executor;
    using promise_type = detail::generator_frame<T, Executor>;

    async_generator() noexcept = default;
    async_generator(async_generator&& other) noexcept : frame_{other.frame_} { other.frame_ = nullptr; }
    async_generator& operator=(async_generator&& other) noexcept {
        if (this != &other) {
            reset();
            frame_ = other.frame_;
            other.frame_ = nullptr;
        }
        return *this;
    }
    async_generator(async_generator const&) = delete;
    async_generator& operator=(async_generator const&) = delete;
    ~async_generator() { reset(); }

    class next_awaiter {
      public:
        using aw_awaitable_tag = void; // 经 await_transform：取消检查同 awaitable（§4.3 第 4 条）

        explicit next_awaiter(promise_type* frame) noexcept : frame_{frame} {}

        bool await_ready() const noexcept { return frame_ == nullptr || frame_->finished_; }

        template <class P> void await_suspend(co2::coroutine_handle<P> consumer) noexcept {
            static_assert(std::is_same<typename P::executor_type, Executor>::value,
                          "async_generator<T, E>::next() must be awaited in a frame with executor E");
            // §10.2 第 7 条：同一时刻最多一个未完成的 next()。
            CO2_CONTRACT_CHECK(!frame_->active_);
            frame_->active_ = true;
            frame_->push_frame(&consumer.promise());
        }

        aw::optional<T> await_resume() {
            if (frame_ == nullptr) return aw::optional<T>{};
            return frame_->take();
        }

      private:
        promise_type* frame_;
    };

    next_awaiter next() noexcept { return next_awaiter{frame_}; }

  private:
    friend class detail::generator_frame<T, Executor>;
    explicit async_generator(promise_type* frame) noexcept : frame_{frame} {}

    void reset() noexcept {
        if (frame_ == nullptr) return;
        // §10.2 第 6 条：生成器正在执行或挂起在异步操作上（在帧栈里）时销毁是契约违规。
        CO2_CONTRACT_CHECK(!frame_->active_);
        frame_->destroy();
        frame_ = nullptr;
    }

    promise_type* frame_{nullptr};
};

namespace detail {
template <class T, class Executor>
async_generator<T, Executor> generator_frame<T, Executor>::get_return_object() noexcept {
    this->coro_ = co2::coroutine_handle<generator_frame>::from_promise(*this);
    return async_generator<T, Executor>{this};
}
} // namespace detail

} // namespace aw
```

- [ ] **Step 4：运行，确认通过（含 ASan）**

Run: `cmake --build build/debug -j8 && ctest --test-dir build/debug -R '^aw-async-generator' --output-on-failure && tools/run-matrix.sh asan-cxx14`
Expected：先输出 `100% tests passed out of 3`，再输出 `asan-cxx14: 100% tests passed out of 48`。

- [ ] **Step 5：提交**

```bash
git add include/awaitable tests/unit
git commit -m "feat(generator): async_generator running on the consumer's thread"
```

### Task 16：sync_wait、总头文件、头文件自包含检查

**交付物：**
- §10.3 的 `aw::sync_wait`，有两种形式：
  - 在 executor 上运行 awaitable，阻塞调用线程直到完成，用补丁 03 的三态 phase 协议：完成方只有看到 parked 才加锁；等待方先做快速 load，再在锁内用 CAS 把 running 改成 parked；
  - `manual_executor&` 形式由调用线程自己 `run_one` 驱动，空闲时 yield；
  - 在 executor 自己的线程上调用属于契约违反。
- §2.2 的总头文件 `include/awaitable.hpp`。
- 头文件自包含检查：每个公共头生成一个只包含它两次的 TU，编译成 OBJECT 库，再作为一个 ctest 项运行。

**Files:**
- Create: `include/awaitable/sync_wait.hpp`, `include/awaitable.hpp`
- Test: `tests/unit/sync_wait_test.cpp`（只包含 `<awaitable.hpp>`）、`tests/unit/sync_wait_on_own_thread_contract_test.cpp`
- Modify: `tests/unit/CMakeLists.txt`（追加）

**Interfaces:**
- Consumes：
  - Task 4 / 5：`manual_executor::run_one`、`running_in_this_thread()`、`thread_pool`；
  - Task 10：`co_spawn`、`spawn_value_t`；
  - Tasks 2–15：全部公共头（总头文件包含它们）。
- Produces：
  - `aw::detail::sync_wait_event`：`set()` / `is_set()` / `wait()`，phase 取值 `running` / `parked` / `finished`；
  - `aw::detail::sync_wait_state<T>` 和 `sync_wait_handler<T>`（含 void 特化）；
  - `template <class T, class Executor> T aw::sync_wait(Executor const& ex, awaitable<T, Executor> a)`：契约 `!ex.running_in_this_thread()`；
  - `template <class T> T aw::sync_wait(manual_executor& ctx, awaitable<T, manual_executor::executor_type> a)`：契约 `!ctx.running_in_this_thread()`；
  - `#include <awaitable.hpp>`：包含全部公共头，不包含测试夹具 `tests/legacy/include/co2/`；
  - CMake 目标 `aw-header-self-containment`（OBJECT 库），以及同名 ctest 项（`RESOURCE_LOCK aw-build-tree`）。

- [ ] **Step 1：写失败的测试**

`tests/unit/sync_wait_test.cpp`：

```cpp
// 只包含总头文件：同时验证 <awaitable.hpp> 可用（§2.2）。
#include <awaitable.hpp>

#include <stdexcept>
#include <string>

#include "check.hpp"
#include "test_ops.hpp"

namespace {

using pool_ex = aw::thread_pool::executor_type;
using manual_ex = aw::manual_executor::executor_type;

auto ready(int v) CO2_BEG((aw::pool_awaitable<int>), (v)) { CO2_RETURN(v); }
CO2_END

auto later(aw_test::foreign_threads* threads, int v) CO2_BEG((aw::pool_awaitable<int>), (threads, v), int r{};) {
    CO2_AWAIT_SET(r, aw_test::async_from_foreign_thread(*threads, v, aw::use_awaitable));
    CO2_RETURN(r);
}
CO2_END

auto fails() CO2_BEG((aw::pool_awaitable<void>), ()) {
    throw std::runtime_error{"sync"};
}
CO2_END

auto on_manual(manual_ex ex, int v) CO2_BEG((aw::manual_awaitable<int>), (ex, v), int r{};) {
    CO2_AWAIT_SET(r, aw_test::async_value(ex, v, aw::use_awaitable));
    CO2_RETURN(r);
}
CO2_END

auto manual_from_foreign(aw_test::foreign_threads* threads, int v)
    CO2_BEG((aw::manual_awaitable<int>), (threads, v), int r{};) {
    CO2_AWAIT_SET(r, aw_test::async_from_foreign_thread(*threads, v, aw::use_awaitable));
    CO2_RETURN(r);
}
CO2_END

struct no_default {
    explicit no_default(int x) : v{x} {}
    int v;
};
auto make_no_default(int v) CO2_BEG((aw::pool_awaitable<no_default>), (v)) { CO2_RETURN(no_default{v}); }
CO2_END

void pool_finish_before_or_after_wait() {
    aw::thread_pool pool{2U};
    int total = 0;
    for (int i = 0; i != 200; ++i) total += aw::sync_wait(pool.get_executor(), ready(i)); // 多数先完成后等待
    AW_CHECK(total == 19900);
    aw_test::foreign_threads threads;
    AW_CHECK(aw::sync_wait(pool.get_executor(), later(&threads, 7)) == 7); // 先等待后完成
    AW_CHECK(aw::sync_wait(pool.get_executor(), make_no_default(5)).v == 5);
}

void pool_exception_is_rethrown() {
    aw::thread_pool pool{1U};
    try {
        aw::sync_wait(pool.get_executor(), fails());
        AW_CHECK(false);
    } catch (std::runtime_error const& e) {
        AW_CHECK(std::string{e.what()} == "sync");
    }
}

void manual_form_drives_run_one() {
    aw::manual_executor ctx;
    AW_CHECK(aw::sync_wait(ctx, on_manual(ctx.get_executor(), 11)) == 11);
    aw_test::foreign_threads threads;
    AW_CHECK(aw::sync_wait(ctx, manual_from_foreign(&threads, 12)) == 12);
}

} // namespace

int main() {
    pool_finish_before_or_after_wait();
    pool_exception_is_rethrown();
    manual_form_drives_run_one();
    return 0;
}
```

`tests/unit/sync_wait_on_own_thread_contract_test.cpp`：

```cpp
// §10.3 / §14.4：在 ex 自己的线程上调 sync_wait(ex, …) 是契约违规（退出码 86）。
#include <cstdlib>

#include "awaitable/awaitable.hpp"
#include "awaitable/core/contract.hpp"
#include "awaitable/sync_wait.hpp"

namespace {

using ex_t = aw::manual_executor::executor_type;

auto one() CO2_BEG((aw::awaitable<int, ex_t>), ()) { CO2_RETURN(1); }
CO2_END

struct nested_wait {
    ex_t ex;
    void operator()() const { static_cast<void>(aw::sync_wait(ex, one())); }
};

} // namespace

int main() {
    co2::setContractViolationHandler([](co2::ContractViolation const&) { std::_Exit(86); });
    aw::manual_executor ctx;
    ctx.get_executor().post(nested_wait{ctx.get_executor()});
    ctx.run();
    return 0;
}
```

`tests/unit/CMakeLists.txt` 追加：

```cmake
aw_unit_test(sync_wait_test)
aw_contract_test(sync_wait_on_own_thread_contract_test)

# §2.2：每个公共头都能单独包含（每个头生成一个只包含它两次的 TU）。
file(GLOB_RECURSE AW_PUBLIC_HEADERS RELATIVE ${PROJECT_SOURCE_DIR}/include CONFIGURE_DEPENDS
     ${PROJECT_SOURCE_DIR}/include/*.hpp)
set(AW_SELF_CONTAINED_SOURCES)
foreach(header IN LISTS AW_PUBLIC_HEADERS)
    string(MAKE_C_IDENTIFIER "${header}" stem)
    set(source ${CMAKE_CURRENT_BINARY_DIR}/self_contained/${stem}.cpp)
    file(CONFIGURE OUTPUT ${source} CONTENT "#include \"${header}\"\n#include \"${header}\"\n")
    list(APPEND AW_SELF_CONTAINED_SOURCES ${source})
endforeach()
add_library(aw-header-self-containment OBJECT ${AW_SELF_CONTAINED_SOURCES})
aw_configure_test(aw-header-self-containment)
add_test(NAME aw-header-self-containment
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target aw-header-self-containment)
set_tests_properties(aw-header-self-containment PROPERTIES RESOURCE_LOCK aw-build-tree)
```

- [ ] **Step 2：运行，确认失败**

Run: `cmake --preset debug >/dev/null && cmake --build build/debug --target aw-sync-wait-test 2>&1 | grep -m1 "error:"`
Expected：包含 `'awaitable.hpp' file not found`

- [ ] **Step 3：最小实现**

`include/awaitable/sync_wait.hpp`：

```cpp
#pragma once

#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

#include "awaitable/awaitable.hpp"
#include "awaitable/co_spawn.hpp"
#include "awaitable/core/contract.hpp"
#include "awaitable/exec/executor.hpp"
#include "awaitable/exec/manual_executor.hpp"
#include "awaitable/impl/entry_point.hpp"
#include "awaitable/optional.hpp"

namespace aw {
namespace detail {

// §10.3：补丁 03 的三态协议。完成方只有看到 parked 才加锁；等待方先快速 load，
// 再在锁内用 CAS 把 running 改成 parked。
class sync_wait_event {
  public:
    void set() noexcept {
        if (phase_.exchange(finished, std::memory_order_acq_rel) == parked) {
            std::lock_guard<std::mutex> lock{mutex_};
            notified_ = true;
            cv_.notify_one();
        }
    }

    bool is_set() const noexcept { return phase_.load(std::memory_order_acquire) == finished; }

    void wait() {
        if (is_set()) return;
        std::unique_lock<std::mutex> lock{mutex_};
        unsigned char expected = running;
        if (!phase_.compare_exchange_strong(expected, parked, std::memory_order_acq_rel)) return;
        cv_.wait(lock, [this] { return notified_; });
    }

  private:
    static constexpr unsigned char running = 0U;
    static constexpr unsigned char finished = 1U;
    static constexpr unsigned char parked = 2U;

    std::atomic<unsigned char> phase_{running};
    std::mutex mutex_;
    std::condition_variable cv_;
    bool notified_{false};
};

template <class T> struct sync_wait_state {
    sync_wait_event event;
    std::exception_ptr error{};
    aw::optional<spawn_value_t<T>> value{};

    T take() {
        if (error) std::rethrow_exception(error);
        return unwrap(*value);
    }

  private:
    template <class V> static T unwrap(V& v) { return std::move(v); }
    template <class V> static T unwrap(aw::optional<V>& v) { return std::move(*v); }
};
template <> struct sync_wait_state<void> {
    sync_wait_event event;
    std::exception_ptr error{};
    void take() {
        if (error) std::rethrow_exception(error);
    }
};

template <class T> struct sync_wait_handler {
    sync_wait_state<T>* state;
    template <class V> void operator()(std::exception_ptr e, V&& v) {
        state->error = std::move(e);
        if (!state->error) state->value.emplace(std::forward<V>(v));
        state->event.set(); // 之后不再访问 state
    }
};
template <> struct sync_wait_handler<void> {
    sync_wait_state<void>* state;
    void operator()(std::exception_ptr e) {
        state->error = std::move(e);
        state->event.set();
    }
};

} // namespace detail

// 形式一：co_spawn 到 ex，阻塞调用线程直到交付。前置条件：!ex.running_in_this_thread()。
template <class T, class Executor, typename std::enable_if<is_executor<Executor>::value, int>::type = 0>
T sync_wait(Executor const& ex, awaitable<T, Executor> a) {
    // 在 ex 自己的线程上等待会死锁（§14.4 契约违规）。
    CO2_CONTRACT_CHECK(!ex.running_in_this_thread());
    detail::sync_wait_state<T> state;
    co_spawn(ex, std::move(a), detail::sync_wait_handler<T>{&state});
    state.event.wait();
    return state.take();
}

// 形式二：在调用线程上驱动 ctx.run_one()，直到交付完成。外部线程完成的操作会投递回 ctx。
template <class T> T sync_wait(manual_executor& ctx, awaitable<T, manual_executor::executor_type> a) {
    CO2_CONTRACT_CHECK(!ctx.running_in_this_thread());
    detail::sync_wait_state<T> state;
    co_spawn(ctx.get_executor(), std::move(a), detail::sync_wait_handler<T>{&state});
    while (!state.event.is_set()) {
        if (!ctx.run_one()) std::this_thread::yield();
    }
    return state.take();
}

} // namespace aw
```

`include/awaitable.hpp`：

```cpp
#pragma once

// awaitable 总头文件（规格 §2.2）：包含全部公共头。不包含测试夹具 tests/legacy/include/co2/。

#include "awaitable/core/coroutine.hpp"
#include "awaitable/core/dsl.hpp"
#include "awaitable/core/generator.hpp"

#include "awaitable/exec/executor.hpp"
#include "awaitable/exec/manual_executor.hpp"
#include "awaitable/exec/scheduler.hpp"
#include "awaitable/exec/strand.hpp"
#include "awaitable/exec/thread_pool.hpp"

#include "awaitable/cancel/cancellation_signal.hpp"
#include "awaitable/cancel/cancellation_state.hpp"
#include "awaitable/cancel/cancellation_type.hpp"
#include "awaitable/cancel/stop_token.hpp"
#include "awaitable/cancel/stop_token_adapter.hpp"

#include "awaitable/as_tuple.hpp"
#include "awaitable/associated.hpp"
#include "awaitable/async_generator.hpp"
#include "awaitable/async_result.hpp"
#include "awaitable/awaitable.hpp"
#include "awaitable/co_spawn.hpp"
#include "awaitable/detached.hpp"
#include "awaitable/dispatch.hpp"
#include "awaitable/error.hpp"
#include "awaitable/multiple_exceptions.hpp"
#include "awaitable/operators.hpp"
#include "awaitable/optional.hpp"
#include "awaitable/post.hpp"
#include "awaitable/redirect_error.hpp"
#include "awaitable/sync_wait.hpp"
#include "awaitable/this_coro.hpp"
#include "awaitable/use_awaitable.hpp"
#include "awaitable/use_future.hpp"
#include "awaitable/variant.hpp"
```

- [ ] **Step 4：运行，确认通过（含 TSan 与全矩阵）**

`file(GLOB ... CONFIGURE_DEPENDS)` 会在构建时发现新头文件；保险起见，这里先重新 configure 一次。

Run: `cmake --preset debug >/dev/null && cmake --build build/debug -j8 && ctest --test-dir build/debug -R '^aw-(sync-wait|header)' --output-on-failure && tools/run-matrix.sh`
Expected：先输出 `100% tests passed out of 3`，再输出 12 行，每行都是 `<配置>: 100% tests passed out of 51`。

- [ ] **Step 5：提交**

```bash
git add include tests/unit
git commit -m "feat(sync): sync_wait with the three-state phase protocol, umbrella header, header self-containment check"
```

### Task 17：帧栈原语差分测试

**交付物：** §14.2 的差分测试，固定用 C++20。测试里有一个只依赖协程协议的最小执行线程：push / pop、返回 void 的 `await_suspend`、`after_suspend`、final 时 pop、pump。promise 和 awaiter 代码用 `Lib` 参数化，一份由 co2 宏协程驱动，一份由原生 `co_await` 协程驱动，然后逐事件比对两边的追踪序列。共四个场景：
- 嵌套；
- 异常传播；
- `after_suspend`；
- 销毁挂起中的协程（局部对象析构时打点，用来比对析构顺序）。

这一步只加测试，不改库代码。它验证的是 Task 10 帧栈模型所依赖的协议行为，所以 Step 2 应当直接通过。如果没有通过，说明 co2 core 与原生协程的语义有差异，需要停下来查。

**Files:**
- Create: `tests/differential/CMakeLists.txt`, `tests/differential/frame_stack_differential_test.cpp`
- Modify: `tests/CMakeLists.txt`（追加）

**Interfaces:**
- Consumes：co2 core 的 `CO2_BEG` / `CO2_END` / `CO2_AWAIT` / `CO2_RETURN`、`co2::coroutine_handle`；Task 1 的 `aw_configure_test`。
- Produces：ctest 项 `aw-frame-stack-differential-test`。设置环境变量 `AW_DIFF_VERBOSE=1` 时打印每个场景的追踪序列；不一致时打印两边并失败。

- [ ] **Step 1：写测试**

`tests/differential/frame_stack_differential_test.cpp`：

```cpp
// §14.2：帧栈原语的差分测试（C++20）。一个只依赖协程协议的最小执行线程（push / pop、
// await_suspend 返回 void、after_suspend、final 时 pop、pump），promise 与 awaiter 代码以 Lib
// 参数化，分别由 co2 宏协程和原生 co_await 协程驱动，逐事件比对追踪序列。

#include <coroutine>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "awaitable/core/coroutine.hpp"
#include "awaitable/core/dsl.hpp"

namespace {

struct log_t {
    std::vector<std::string> events;
    void add(std::string e) { events.push_back(std::move(e)); }
};

struct Co2Lib {
    template <class P = void> using Handle = co2::coroutine_handle<P>;
    using SuspendAlways = co2::suspend_always;
};
struct StdLib {
    template <class P = void> using Handle = std::coroutine_handle<P>;
    using SuspendAlways = std::suspend_always;
};

// 局部对象：析构时打点，用来比对销毁顺序。
struct noisy {
    log_t* log{nullptr};
    std::string name;
    void bind(log_t& l, std::string n) {
        log = &l;
        name = std::move(n);
    }
    ~noisy() {
        if (log != nullptr) log->add(name + " destroyed");
    }
};

template <class Lib> struct frame_base;

template <class Lib> struct mini_thread {
    frame_base<Lib>* top{nullptr};
    bool detached{false};
    log_t* log{nullptr};
    // asio pump：反复恢复栈顶，直到栈空或执行线程被移走。
    void pump() {
        detached = false;
        while (top != nullptr && !detached) top->resume();
        log->add(top == nullptr ? "pump: stack empty" : "pump: detached");
    }
};

struct resume_context {
    void (*fn)(void*){nullptr};
    void* arg{nullptr};
};

template <class Lib> struct frame_base {
    typename Lib::template Handle<> coro{};
    frame_base* caller{nullptr};
    mini_thread<Lib>* thread{nullptr};
    resume_context* rc{nullptr};
    std::exception_ptr pending{};
    std::string name;

    void resume() {
        resume_context context;
        rc = &context;
        thread->log->add("resume " + name);
        coro.resume();
        if (context.fn != nullptr) context.fn(context.arg);
    }
    void push_frame(frame_base* parent) {
        caller = parent;
        thread = parent->thread;
        thread->top = this;
        thread->log->add("push " + name + " over " + parent->name);
    }
    void pop_frame() {
        thread->log->add("pop " + name);
        thread->top = caller;
    }
    void after_suspend(void (*fn)(void*), void* arg) {
        rc->fn = fn;
        rc->arg = arg;
    }
};

template <class Lib> struct task {
    struct promise_type : frame_base<Lib> {
        int value{0};
        explicit promise_type() = default;
        template <class... Args> explicit promise_type(log_t&, std::string n, Args&&...) { this->name = std::move(n); }
        task get_return_object() {
            this->coro = Lib::template Handle<promise_type>::from_promise(*this);
            return task{this};
        }
        typename Lib::SuspendAlways initial_suspend() const noexcept { return {}; }
        struct final_awaiter {
            promise_type* self;
            bool await_ready() const noexcept { return false; }
            void await_suspend(typename Lib::template Handle<>) noexcept { self->pop_frame(); }
            void await_resume() const noexcept {}
        };
        final_awaiter final_suspend() noexcept { return final_awaiter{this}; }
        void return_value(int v) { value = v; }
        void unhandled_exception() { this->pending = std::current_exception(); }
    };

    explicit task(promise_type* p) : frame{p} {}
    task(task&& other) noexcept : frame{other.frame} { other.frame = nullptr; }
    task(task const&) = delete;
    ~task() {
        if (frame != nullptr) frame->coro.destroy();
    }

    // 等待子 task：压栈，返回 void 交回 pump。
    bool await_ready() const noexcept { return false; }
    template <class P> void await_suspend(typename Lib::template Handle<P> h) { frame->push_frame(&h.promise()); }
    int await_resume() {
        if (frame->pending) std::rethrow_exception(frame->pending);
        return frame->value;
    }

    promise_type* frame;
};

// 异步操作：await_suspend 只登记 after_suspend；挂起完成后执行线程被“移走”，由驱动方完成。
template <class Lib> struct pending_ops {
    std::vector<frame_base<Lib>*> waiting;
    int next_value{100};
};

template <class Lib> struct async_op {
    pending_ops<Lib>* ops;
    frame_base<Lib>* frame{nullptr};
    int result{0};
    bool await_ready() const noexcept { return false; }
    template <class P> void await_suspend(typename Lib::template Handle<P> h) {
        frame = &h.promise();
        frame->thread->log->add("op await_suspend in " + frame->name);
        frame->after_suspend(&on_suspend, this);
    }
    static void on_suspend(void* arg) {
        auto* self = static_cast<async_op*>(arg);
        self->frame->thread->log->add("after_suspend: initiate");
        self->frame->thread->detached = true;
        self->ops->waiting.push_back(self->frame);
        self->result = self->ops->next_value++;
    }
    int await_resume() { return result; }
};

template <class Lib> void complete_all(mini_thread<Lib>& thread, pending_ops<Lib>& ops) {
    while (!ops.waiting.empty()) {
        ops.waiting.erase(ops.waiting.begin());
        thread.log->add("complete");
        thread.pump();
    }
}

using C = task<Co2Lib>;
using S = task<StdLib>;

// ---- 场景 1：三层嵌套，压栈出栈顺序 ----
auto co2_leaf(log_t& log, std::string n, int v) CO2_BEG(C, (log, n, v)) {
    log.add(n + " body");
    CO2_RETURN(v);
}
CO2_END
auto co2_mid(log_t& log, std::string n, int v) CO2_BEG(C, (log, n, v), int r{};) {
    log.add(n + " before");
    CO2_AWAIT_SET(r, co2_leaf(log, n + ".leaf", v));
    log.add(n + " after " + std::to_string(r));
    CO2_RETURN(r + 1);
}
CO2_END
auto co2_root(log_t& log, std::string n, int v) CO2_BEG(C, (log, n, v), int r{};) {
    log.add(n + " before");
    CO2_AWAIT_SET(r, co2_mid(log, n + ".mid", v));
    log.add(n + " after " + std::to_string(r));
    CO2_RETURN(r + 1);
}
CO2_END

S std_leaf(log_t& log, std::string n, int v) {
    log.add(n + " body");
    co_return v;
}
S std_mid(log_t& log, std::string n, int v) {
    log.add(n + " before");
    int r = co_await std_leaf(log, n + ".leaf", v);
    log.add(n + " after " + std::to_string(r));
    co_return r + 1;
}
S std_root(log_t& log, std::string n, int v) {
    log.add(n + " before");
    int r = co_await std_mid(log, n + ".mid", v);
    log.add(n + " after " + std::to_string(r));
    co_return r + 1;
}

// ---- 场景 2：叶子抛出，逐层经 await_resume 重抛到根 ----
auto co2_thrower(log_t& log, std::string n, int) CO2_BEG(C, (log, n)) {
    log.add(n + " throws");
    throw std::runtime_error{"leaf"};
}
CO2_END
auto co2_propagate(log_t& log, std::string n, int) CO2_BEG(C, (log, n), int r{};) {
    CO2_AWAIT_SET(r, co2_thrower(log, n + ".leaf", 0));
    log.add("unreachable");
    CO2_RETURN(r);
}
CO2_END
S std_thrower(log_t& log, std::string n, int) {
    log.add(n + " throws");
    throw std::runtime_error{"leaf"};
    co_return 0;
}
S std_propagate(log_t& log, std::string n, int) {
    int r = co_await std_thrower(log, n + ".leaf", 0);
    log.add("unreachable");
    co_return r;
}

// ---- 场景 3：中间层等待异步操作：after_suspend、pump 退出、完成后重新 pump ----
template <class Lib> pending_ops<Lib>* g_ops = nullptr;

auto co2_waits(log_t& log, std::string n, int) CO2_BEG(C, (log, n), int a{}; int b{};) {
    CO2_AWAIT_SET(a, (async_op<Co2Lib>{g_ops<Co2Lib>}));
    log.add(n + " got " + std::to_string(a));
    CO2_AWAIT_SET(b, (async_op<Co2Lib>{g_ops<Co2Lib>}));
    log.add(n + " got " + std::to_string(b));
    CO2_RETURN(a + b);
}
CO2_END
auto co2_outer(log_t& log, std::string n, int) CO2_BEG(C, (log, n), int r{};) {
    CO2_AWAIT_SET(r, co2_waits(log, n + ".waits", 0));
    log.add(n + " sum " + std::to_string(r));
    CO2_RETURN(r);
}
CO2_END
S std_waits(log_t& log, std::string n, int) {
    int a = co_await async_op<StdLib>{g_ops<StdLib>};
    log.add(n + " got " + std::to_string(a));
    int b = co_await async_op<StdLib>{g_ops<StdLib>};
    log.add(n + " got " + std::to_string(b));
    co_return a + b;
}
S std_outer(log_t& log, std::string n, int) {
    int r = co_await std_waits(log, n + ".waits", 0);
    log.add(n + " sum " + std::to_string(r));
    co_return r;
}

// ---- 场景 4：挂起在异步操作上时销毁根帧：子帧随父帧的 awaiter 一起销毁 ----
auto co2_local_leaf(log_t& log, std::string n, int) CO2_BEG(C, (log, n), noisy guard{}; int a{};) {
    guard.bind(log, n + ".guard");
    CO2_AWAIT_SET(a, (async_op<Co2Lib>{g_ops<Co2Lib>}));
    CO2_RETURN(a);
}
CO2_END
auto co2_local_root(log_t& log, std::string n, int) CO2_BEG(C, (log, n), noisy guard{}; int r{};) {
    guard.bind(log, n + ".guard");
    CO2_AWAIT_SET(r, co2_local_leaf(log, n + ".leaf", 0));
    CO2_RETURN(r);
}
CO2_END
S std_local_leaf(log_t& log, std::string n, int) {
    noisy guard;
    guard.bind(log, n + ".guard");
    int a = co_await async_op<StdLib>{g_ops<StdLib>};
    co_return a;
}
S std_local_root(log_t& log, std::string n, int) {
    noisy guard;
    guard.bind(log, n + ".guard");
    int r = co_await std_local_leaf(log, n + ".leaf", 0);
    co_return r;
}

// 驱动：根帧放到栈底，pump；有挂起的操作就逐个完成。
template <class Lib, class Make> log_t drive(Make make, bool destroy_while_suspended) {
    log_t log;
    pending_ops<Lib> ops;
    g_ops<Lib> = &ops;
    mini_thread<Lib> thread;
    thread.log = &log;
    {
        task<Lib> root = make(log, std::string{"root"}, 3);
        root.frame->thread = &thread;
        thread.top = root.frame;
        thread.pump();
        if (destroy_while_suspended) {
            log.add("-- destroy root while suspended");
        } else {
            complete_all(thread, ops);
            if (root.frame->pending) {
                try {
                    std::rethrow_exception(root.frame->pending);
                } catch (std::exception const& e) {
                    log.add(std::string{"root exception: "} + e.what());
                }
            } else {
                log.add("root value " + std::to_string(root.frame->value));
            }
        }
    }
    log.add("-- root destroyed");
    return log;
}

void compare(char const* name, log_t const& a, log_t const& b) {
    if (std::getenv("AW_DIFF_VERBOSE") != nullptr) {
        std::cout << "== " << name << '\n';
        for (auto const& e : a.events) std::cout << "    " << e << '\n';
    }
    if (a.events == b.events) return;
    std::cerr << "differential mismatch in scenario " << name << "\n  co2:\n";
    for (auto const& e : a.events) std::cerr << "    " << e << '\n';
    std::cerr << "  std:\n";
    for (auto const& e : b.events) std::cerr << "    " << e << '\n';
    std::exit(EXIT_FAILURE);
}

} // namespace

int main() {
    compare("nesting", drive<Co2Lib>(co2_root, false), drive<StdLib>(std_root, false));
    compare("exception", drive<Co2Lib>(co2_propagate, false), drive<StdLib>(std_propagate, false));
    compare("after_suspend", drive<Co2Lib>(co2_outer, false), drive<StdLib>(std_outer, false));
    compare("destroy_suspended", drive<Co2Lib>(co2_local_root, true), drive<StdLib>(std_local_root, true));
    // 防止“两边都什么也没记”的空比对。
    if (drive<Co2Lib>(co2_root, false).events.size() < 10U) return EXIT_FAILURE;
    return 0;
}
```

`tests/differential/CMakeLists.txt`：

```cmake
# §14.2：帧栈原语的差分测试，固定 C++20（同一编译器的原生协程作对照）。
add_executable(aw-frame-stack-differential-test frame_stack_differential_test.cpp)
aw_configure_test(aw-frame-stack-differential-test)
set_target_properties(aw-frame-stack-differential-test PROPERTIES CXX_STANDARD 20)
add_test(NAME aw-frame-stack-differential-test COMMAND aw-frame-stack-differential-test)
```

`tests/CMakeLists.txt` 追加：

```cmake
add_subdirectory(differential)
```

- [ ] **Step 2：运行，确认通过并检查追踪序列**

Run: `cmake --preset debug >/dev/null && cmake --build build/debug -j8 && ctest --test-dir build/debug -R '^aw-frame-stack' --output-on-failure && AW_DIFF_VERBOSE=1 build/debug/tests/differential/aw-frame-stack-differential-test 2>&1 | head -4`
Expected：先输出 `100% tests passed out of 1`；再输出（两边追踪相同，只打印一份）：

```text
== nesting
    resume root
    root before
    push root.mid over root
```

- [ ] **Step 3：全矩阵**

Run: `tools/run-matrix.sh`
Expected：12 行，每行都是 `<配置>: 100% tests passed out of 52`。

- [ ] **Step 4：提交**

```bash
git add tests/CMakeLists.txt tests/differential
git commit -m "test(differential): frame-stack primitives, co2 macro coroutines vs native coroutines"
```

### Task 18：与 asio 的行为对照

**交付物：** §14.3 的对照测试。同一个场景写两份，一份用 asio 190a8d8 的原生协程（C++20），一份用本库的宏协程。两边各自记录事件，然后断言事件序列相同。共七个场景：
- 嵌套；
- 异常；
- 取消：3 种取消类型 × 3 种过滤器 × `throw_if_cancelled` 开 / 关；
- strand 轮转顺序；
- `||` / `&&`；
- executor 亲和性；
- 启动时同步完成。

asio 只用于测试和基准，库本身不依赖它。`AW_ASIO_ROOT` 为空时跳过这个目录。

**Files:**
- Create: `tests/asio_parity/CMakeLists.txt`, `tests/asio_parity/asio_parity_test.cpp`
- Modify（整文件替换）：`CMakeLists.txt`（新增缓存变量 `AW_ASIO_ROOT`）、`tools/run-matrix.sh`（向每个配置传入 `-DAW_ASIO_ROOT`，默认 `/workspace/asio`）
- Modify: `tests/CMakeLists.txt`（追加）

**Interfaces:**
- Consumes：
  - `<awaitable.hpp>`（Task 16）；
  - `tests/support/test_ops.hpp`（Task 11）、`tests/support/test_cancellable_ops.hpp`（Task 13）；
  - asio 的 `awaitable`、`co_spawn`、`experimental::awaitable_operators`、`strand`、`io_context`。
- Produces：
  - CMake 缓存变量 `AW_ASIO_ROOT`（PATH，默认空）；
  - ctest 项 `aw-asio-parity-test`；成功时打印 `asio parity: all scenarios match`；
  - `tools/run-matrix.sh` 的环境变量 `AW_ASIO_ROOT`（设为空字符串可以跳过对照）。

- [ ] **Step 1：写测试**

说明：asio 的 `io.poll()` 不会运行 poll 期间新投递的 handler，所以测试里用 `drain(io)` 循环 poll，直到没有可运行的 handler。

`tests/asio_parity/asio_parity_test.cpp`：

```cpp
// §14.3：同一场景写两份——asio 原生协程（C++20）与本库的宏协程——各自记录事件，断言相同。

#include <asio.hpp>
#include <asio/experimental/awaitable_operators.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <awaitable.hpp>

#include "test_cancellable_ops.hpp"
#include "test_ops.hpp"

namespace {

struct trace_t {
    std::vector<std::string> events;
    void add(std::string e) { events.push_back(std::move(e)); }
};

void compare(std::string const& name, trace_t const& ours, trace_t const& theirs) {
    if (ours.events == theirs.events && !ours.events.empty()) return;
    std::cerr << "asio parity mismatch in scenario " << name << "\n  aw:\n";
    for (auto const& e : ours.events) std::cerr << "    " << e << '\n';
    std::cerr << "  asio:\n";
    for (auto const& e : theirs.events) std::cerr << "    " << e << '\n';
    std::exit(EXIT_FAILURE);
}

std::string describe(std::exception_ptr e) {
    if (!e) return "none";
    try {
        std::rethrow_exception(e);
    } catch (asio::multiple_exceptions const& m) {
        return "multiple_exceptions(" + describe(m.first_exception()) + ")";
    } catch (aw::multiple_exceptions const& m) {
        return "multiple_exceptions(" + describe(m.first_exception()) + ")";
    } catch (std::system_error const& s) {
        return s.code() == std::error_code{asio::error::operation_aborted} ||
                       s.code() == std::error_code{aw::error::operation_aborted}
                   ? std::string{"operation_aborted"}
                   : std::string{"system_error"};
    } catch (std::runtime_error const& r) {
        return std::string{"runtime_error "} + r.what();
    } catch (...) {
        return "unknown";
    }
}

// io_context::poll 不一定执行它自己运行期间新投递的项：反复 poll 直到没有就绪项。
void drain(asio::io_context& io) {
    for (;;) {
        io.restart();
        if (io.poll() == 0U) break;
    }
}

using aw_ex = aw::manual_executor::executor_type;
template <class T> using aw_task = aw::awaitable<T, aw_ex>;

// ============================================================================
// 场景 1：三层嵌套，每层前后打点
// ============================================================================
asio::awaitable<int> asio_level(trace_t& t, int depth) {
    t.add("enter " + std::to_string(depth));
    int r = depth;
    if (depth < 3) r += co_await asio_level(t, depth + 1);
    t.add("leave " + std::to_string(depth));
    co_return r;
}
auto aw_level(trace_t& t, int depth) CO2_BEG(aw_task<int>, (t, depth), int r{};) {
    t.add("enter " + std::to_string(depth));
    r = depth;
    if (depth < 3) {
        CO2_AWAIT_SET(r, aw_level(t, depth + 1));
        r += depth;
    }
    t.add("leave " + std::to_string(depth));
    CO2_RETURN(r);
}
CO2_END

void scenario_nesting() {
    trace_t a, b;
    {
        aw::manual_executor ctx;
        aw::co_spawn(ctx, aw_level(a, 1), [&](std::exception_ptr e, int v) {
            a.add("cb " + describe(e) + " " + std::to_string(v));
        });
        ctx.run();
    }
    {
        asio::io_context io;
        asio::co_spawn(io, asio_level(b, 1), [&](std::exception_ptr e, int v) {
            b.add("cb " + describe(e) + " " + std::to_string(v));
        });
        io.run();
    }
    compare("nesting", a, b);
}

// ============================================================================
// 场景 2：子帧抛出，父帧不处理，一路传到 co_spawn 回调
// ============================================================================
asio::awaitable<int> asio_thrower(trace_t& t) {
    t.add("throwing");
    throw std::runtime_error{"leaf"};
    co_return 0;
}
asio::awaitable<int> asio_middle(trace_t& t) {
    int v = co_await asio_thrower(t);
    t.add("unreachable");
    co_return v;
}
auto aw_thrower(trace_t& t) CO2_BEG(aw_task<int>, (t)) {
    t.add("throwing");
    throw std::runtime_error{"leaf"};
}
CO2_END
auto aw_middle(trace_t& t) CO2_BEG(aw_task<int>, (t), int v{};) {
    CO2_AWAIT_SET(v, aw_thrower(t));
    t.add("unreachable");
    CO2_RETURN(v);
}
CO2_END

void scenario_exception() {
    trace_t a, b;
    {
        aw::manual_executor ctx;
        aw::co_spawn(ctx, aw_middle(a), [&](std::exception_ptr e, int) { a.add("cb " + describe(e)); });
        ctx.run();
    }
    {
        asio::io_context io;
        asio::co_spawn(io, asio_middle(b), [&](std::exception_ptr e, int) { b.add("cb " + describe(e)); });
        io.run();
    }
    compare("exception", a, b);
}

// ============================================================================
// 场景 3：三级取消 × 三种过滤器 × throw_if_cancelled 开 / 关，以及 reset 之后
// ============================================================================
enum class filter_kind { keep_default, partial, total };

asio::awaitable<void> asio_cancel_body(trace_t& t, filter_kind f, bool tic, asio::steady_timer& timer) {
    if (f == filter_kind::partial)
        co_await asio::this_coro::reset_cancellation_state(asio::enable_partial_cancellation());
    if (f == filter_kind::total)
        co_await asio::this_coro::reset_cancellation_state(asio::enable_total_cancellation());
    if (!tic) co_await asio::this_coro::throw_if_cancelled(false);
    auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
    t.add(ec == asio::error::operation_aborted ? "wait aborted" : "wait ok");
    asio::cancellation_state cs = co_await asio::this_coro::cancellation_state;
    t.add("cancelled " + std::to_string(static_cast<unsigned>(cs.cancelled())));
    co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
    t.add("post done");
    co_await asio::this_coro::reset_cancellation_state();
    cs = co_await asio::this_coro::cancellation_state;
    t.add("after reset " + std::to_string(static_cast<unsigned>(cs.cancelled())));
    co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
    t.add("post after reset done");
}

using aw_event = aw_test::manual_event<aw_ex>;
auto aw_cancel_body(trace_t& t, filter_kind f, bool tic, aw_event* ev, aw_ex ex)
    CO2_BEG(aw_task<void>, (t, f, tic, ev, ex), std::tuple<std::error_code> r{}; aw::cancellation_state cs{};) {
    if (f == filter_kind::partial)
        CO2_AWAIT(aw::this_coro::reset_cancellation_state(aw::enable_partial_cancellation{}));
    if (f == filter_kind::total)
        CO2_AWAIT(aw::this_coro::reset_cancellation_state(aw::enable_total_cancellation{}));
    if (!tic) CO2_AWAIT(aw::this_coro::throw_if_cancelled(false));
    CO2_AWAIT_SET(r, ev->async_wait(aw::as_tuple(aw::use_awaitable)));
    t.add(std::get<0>(r) == std::error_code{aw::error::operation_aborted} ? "wait aborted" : "wait ok");
    CO2_AWAIT_SET(cs, aw::this_coro::cancellation_state);
    t.add("cancelled " + std::to_string(static_cast<unsigned>(cs.cancelled())));
    CO2_AWAIT(aw::post(ex, aw::use_awaitable));
    t.add("post done");
    CO2_AWAIT(aw::this_coro::reset_cancellation_state());
    CO2_AWAIT_SET(cs, aw::this_coro::cancellation_state);
    t.add("after reset " + std::to_string(static_cast<unsigned>(cs.cancelled())));
    CO2_AWAIT(aw::post(ex, aw::use_awaitable));
    t.add("post after reset done");
    CO2_RETURN();
}
CO2_END

void scenario_cancellation() {
    aw::cancellation_type const aw_types[] = {aw::cancellation_type::terminal, aw::cancellation_type::partial,
                                              aw::cancellation_type::total};
    asio::cancellation_type const asio_types[] = {asio::cancellation_type::terminal,
                                                  asio::cancellation_type::partial,
                                                  asio::cancellation_type::total};
    filter_kind const filters[] = {filter_kind::keep_default, filter_kind::partial, filter_kind::total};
    for (int ti = 0; ti != 3; ++ti) {
        for (filter_kind f : filters) {
            for (bool tic : {true, false}) {
                std::string const name = "cancel type=" + std::to_string(ti) + " filter=" +
                                         std::to_string(static_cast<int>(f)) + " tic=" + std::to_string(tic);
                trace_t a, b;
                {
                    aw::manual_executor ctx;
                    aw_event ev{ctx.get_executor()};
                    aw::cancellation_signal sig;
                    aw::co_spawn(ctx, aw_cancel_body(a, f, tic, &ev, ctx.get_executor()),
                                 aw::bind_cancellation_slot(sig.slot(), [&](std::exception_ptr e) {
                                     a.add("cb " + describe(e));
                                 }));
                    ctx.run();
                    sig.emit(aw_types[ti]);
                    a.add("emitted");
                    ctx.run();
                    if (ev.waiting()) {
                        a.add("still waiting");
                        sig.emit(aw::cancellation_type::terminal);
                        ctx.run();
                    }
                }
                {
                    asio::io_context io;
                    asio::steady_timer timer{io, asio::steady_timer::time_point::max()};
                    asio::cancellation_signal sig;
                    bool waited = false;
                    asio::co_spawn(io, asio_cancel_body(b, f, tic, timer),
                                   asio::bind_cancellation_slot(sig.slot(), [&](std::exception_ptr e) {
                                       b.add("cb " + describe(e));
                                   }));
                    drain(io);
                    sig.emit(asio_types[ti]);
                    b.add("emitted");
                    drain(io);
                    waited = std::find(b.events.begin(), b.events.end(), "wait aborted") != b.events.end() ||
                             std::find(b.events.begin(), b.events.end(), "wait ok") != b.events.end();
                    if (!waited) {
                        b.add("still waiting");
                        sig.emit(asio::cancellation_type::terminal);
                        drain(io);
                    }
                }
                compare(name, a, b);
            }
        }
    }
}

// ============================================================================
// 场景 4：N 个执行线程绑定同一个 strand，交替等待异步操作：互不重叠，顺序一致
// ============================================================================
constexpr int strand_rounds = 50;

asio::awaitable<void> asio_strand_worker(int id, asio::strand<asio::thread_pool::executor_type> s,
                                         std::vector<std::string>* order, std::atomic<int>* inside,
                                         std::atomic<bool>* overlap) {
    for (int k = 0; k != strand_rounds; ++k) {
        if (inside->fetch_add(1) != 0) overlap->store(true);
        order->push_back(std::to_string(id) + ":" + std::to_string(k));
        inside->fetch_sub(1);
        co_await asio::post(s, asio::use_awaitable);
    }
}

using pool_strand = aw::strand<aw::thread_pool::executor_type>;
auto aw_strand_worker(int id, pool_strand s, std::vector<std::string>* order, std::atomic<int>* inside,
                      std::atomic<bool>* overlap)
    CO2_BEG((aw::awaitable<void, pool_strand>), (id, s, order, inside, overlap), int k{};) {
    for (k = 0; k != strand_rounds; ++k) {
        if (inside->fetch_add(1) != 0) overlap->store(true);
        order->push_back(std::to_string(id) + ":" + std::to_string(k));
        inside->fetch_sub(1);
        CO2_AWAIT(aw::post(s, aw::use_awaitable));
    }
    CO2_RETURN();
}
CO2_END

void scenario_strand() {
    trace_t a, b;
    {
        aw::thread_pool pool{4U};
        pool_strand s = aw::make_strand(pool.get_executor());
        std::vector<std::string> order;
        std::atomic<int> inside{0};
        std::atomic<bool> overlap{false};
        std::promise<void> all_done;
        std::atomic<int> remaining{3};
        // 在 strand 内发起三个执行线程：co_spawn 先 dispatch，所以启动在 strand 内内联进行。
        s.post([&] {
            for (int id = 0; id != 3; ++id)
                aw::co_spawn(s, aw_strand_worker(id, s, &order, &inside, &overlap), [&](std::exception_ptr) {
                    if (remaining.fetch_sub(1) == 1) all_done.set_value();
                });
        });
        all_done.get_future().wait();
        for (auto const& e : order) a.add(e);
        a.add(overlap.load() ? "overlap" : "no overlap");
    }
    {
        asio::thread_pool pool{4};
        auto s = asio::make_strand(pool.get_executor());
        std::vector<std::string> order;
        std::atomic<int> inside{0};
        std::atomic<bool> overlap{false};
        asio::post(s, [&] {
            for (int id = 0; id != 3; ++id)
                asio::co_spawn(s, asio_strand_worker(id, s, &order, &inside, &overlap), asio::detached);
        });
        pool.join();
        for (auto const& e : order) b.add(e);
        b.add(overlap.load() ? "overlap" : "no overlap");
    }
    compare("strand", a, b);
}

// ============================================================================
// 场景 5：|| / &&：先完成者胜出、败者被取消；单边与双边失败；void 与非 void；三元链式
// ============================================================================
asio::awaitable<int> asio_delay(int n) {
    for (int i = 0; i != n; ++i) co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
    co_return n;
}
asio::awaitable<void> asio_void_delay(int n) {
    for (int i = 0; i != n; ++i) co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
}
asio::awaitable<int> asio_fail(int n) {
    for (int i = 0; i != n; ++i) co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
    throw std::runtime_error{"fail " + std::to_string(n)};
    co_return 0;
}

asio::awaitable<void> asio_operators(trace_t& t) {
    using namespace asio::experimental::awaitable_operators;
    auto r1 = co_await (asio_delay(1) || asio_delay(3));
    t.add("or index " + std::to_string(r1.index()) + " value " + std::to_string(std::get<0>(r1)));
    auto r2 = co_await (asio_delay(1) && asio_delay(3));
    t.add("and " + std::to_string(std::get<0>(r2)) + "," + std::to_string(std::get<1>(r2)));
    int r3 = co_await (asio_void_delay(1) && asio_delay(2));
    t.add("void and int " + std::to_string(r3));
    auto r4 = co_await (asio_delay(1) && asio_delay(2) && asio_delay(3));
    t.add("and3 " + std::to_string(std::get<0>(r4) + std::get<1>(r4) + std::get<2>(r4)));
    auto r5 = co_await (asio_fail(1) || asio_delay(2) || asio_delay(3));
    t.add("or3 index " + std::to_string(r5.index()) + " value " + std::to_string(std::get<1>(r5)));
    auto r6 = co_await (asio_void_delay(2) || asio_delay(1));
    t.add("void or int index " + std::to_string(r6.index()));
}
asio::awaitable<void> asio_and_one_fails() {
    using namespace asio::experimental::awaitable_operators;
    co_await (asio_fail(1) && asio_delay(3));
}
asio::awaitable<void> asio_and_both_fail() {
    using namespace asio::experimental::awaitable_operators;
    co_await (asio_fail(1) && asio_fail(2));
}
asio::awaitable<void> asio_or_both_fail() {
    using namespace asio::experimental::awaitable_operators;
    co_await (asio_fail(2) || asio_fail(1));
}

auto aw_delay(int n, aw_ex ex) CO2_BEG(aw_task<int>, (n, ex), int i{};) {
    for (i = 0; i != n; ++i) CO2_AWAIT(aw::post(ex, aw::use_awaitable));
    CO2_RETURN(n);
}
CO2_END
auto aw_void_delay(int n, aw_ex ex) CO2_BEG(aw_task<void>, (n, ex), int i{};) {
    for (i = 0; i != n; ++i) CO2_AWAIT(aw::post(ex, aw::use_awaitable));
    CO2_RETURN();
}
CO2_END
auto aw_fail(int n, aw_ex ex) CO2_BEG(aw_task<int>, (n, ex), int i{};) {
    for (i = 0; i != n; ++i) CO2_AWAIT(aw::post(ex, aw::use_awaitable));
    throw std::runtime_error{"fail " + std::to_string(n)};
}
CO2_END

using namespace aw::awaitable_operators;

auto aw_operators(trace_t& t, aw_ex ex)
    CO2_BEG(aw_task<void>, (t, ex), aw::variant<int, int> r1{}; std::tuple<int, int> r2{}; int r3{};
            std::tuple<int, int, int> r4{}; aw::variant<int, int, int> r5{}; aw::variant<aw::monostate, int> r6{};) {
    CO2_AWAIT_SET(r1, aw_delay(1, ex) || aw_delay(3, ex));
    t.add("or index " + std::to_string(r1.index()) + " value " + std::to_string(aw::get<0>(r1)));
    CO2_AWAIT_SET(r2, aw_delay(1, ex) && aw_delay(3, ex));
    t.add("and " + std::to_string(std::get<0>(r2)) + "," + std::to_string(std::get<1>(r2)));
    CO2_AWAIT_SET(r3, aw_void_delay(1, ex) && aw_delay(2, ex));
    t.add("void and int " + std::to_string(r3));
    CO2_AWAIT_SET(r4, aw_delay(1, ex) && aw_delay(2, ex) && aw_delay(3, ex));
    t.add("and3 " + std::to_string(std::get<0>(r4) + std::get<1>(r4) + std::get<2>(r4)));
    CO2_AWAIT_SET(r5, aw_fail(1, ex) || aw_delay(2, ex) || aw_delay(3, ex));
    t.add("or3 index " + std::to_string(r5.index()) + " value " + std::to_string(aw::get<1>(r5)));
    CO2_AWAIT_SET(r6, aw_void_delay(2, ex) || aw_delay(1, ex));
    t.add("void or int index " + std::to_string(r6.index()));
    CO2_RETURN();
}
CO2_END
auto aw_and_one_fails(aw_ex ex) CO2_BEG(aw_task<void>, (ex)) {
    CO2_AWAIT(aw_fail(1, ex) && aw_delay(3, ex));
    CO2_RETURN();
}
CO2_END
auto aw_and_both_fail(aw_ex ex) CO2_BEG(aw_task<void>, (ex)) {
    CO2_AWAIT(aw_fail(1, ex) && aw_fail(2, ex));
    CO2_RETURN();
}
CO2_END
auto aw_or_both_fail(aw_ex ex) CO2_BEG(aw_task<void>, (ex)) {
    CO2_AWAIT(aw_fail(2, ex) || aw_fail(1, ex));
    CO2_RETURN();
}
CO2_END

void scenario_operators() {
    trace_t a, b;
    {
        aw::manual_executor ctx;
        aw_ex ex = ctx.get_executor();
        auto cb = [&](std::exception_ptr e) { a.add("cb " + describe(e)); };
        aw::co_spawn(ctx, aw_operators(a, ex), cb);
        ctx.run();
        aw::co_spawn(ctx, aw_and_one_fails(ex), cb);
        ctx.run();
        aw::co_spawn(ctx, aw_and_both_fail(ex), cb);
        ctx.run();
        aw::co_spawn(ctx, aw_or_both_fail(ex), cb);
        ctx.run();
    }
    {
        asio::io_context io;
        auto cb = [&](std::exception_ptr e) { b.add("cb " + describe(e)); };
        asio::co_spawn(io, asio_operators(b), cb);
        io.run();
        io.restart();
        asio::co_spawn(io, asio_and_one_fails(), cb);
        io.run();
        io.restart();
        asio::co_spawn(io, asio_and_both_fail(), cb);
        io.run();
        io.restart();
        asio::co_spawn(io, asio_or_both_fail(), cb);
        io.run();
    }
    compare("operators", a, b);
}

// ============================================================================
// 场景 6：异步操作在别的线程完成，恢复时 running_in_this_thread() 为真
// ============================================================================
template <class Token> auto asio_from_foreign_thread(aw_test::foreign_threads& threads, int v, Token&& token) {
    return asio::async_initiate<Token, void(std::error_code, int)>(
        [&threads, v](auto handler) {
            auto ex = asio::get_associated_executor(handler);
            threads.start([ex, v, h = std::move(handler)]() mutable {
                asio::dispatch(ex, [h = std::move(h), v]() mutable { std::move(h)(std::error_code{}, v); });
            });
        },
        token);
}

asio::awaitable<void> asio_affinity(trace_t& t, asio::thread_pool::executor_type ex,
                                    aw_test::foreign_threads& threads) {
    int v = co_await asio_from_foreign_thread(threads, 5, asio::use_awaitable);
    t.add("value " + std::to_string(v) + " on own executor " + std::to_string(ex.running_in_this_thread()));
}

using pool_ex = aw::thread_pool::executor_type;
auto aw_affinity(trace_t& t, pool_ex ex, aw_test::foreign_threads* threads)
    CO2_BEG((aw::awaitable<void, pool_ex>), (t, ex, threads), int v{};) {
    CO2_AWAIT_SET(v, aw_test::async_from_foreign_thread(*threads, 5, aw::use_awaitable));
    t.add("value " + std::to_string(v) + " on own executor " + std::to_string(ex.running_in_this_thread()));
    CO2_RETURN();
}
CO2_END

void scenario_affinity() {
    trace_t a, b;
    {
        aw_test::foreign_threads threads;
        aw::thread_pool pool{2U};
        aw::co_spawn(pool, aw_affinity(a, pool.get_executor(), &threads), aw::use_future).get();
    }
    {
        asio::thread_pool pool{2};
        aw_test::foreign_threads threads; // 在 pool 之前析构：先 join 外部线程，再销毁 pool
        asio::co_spawn(pool, asio_affinity(b, pool.get_executor(), threads), asio::use_future).get();
        pool.join();
    }
    compare("affinity", a, b);
}

// ============================================================================
// 场景 7：co_spawn 启动期同步完成时，完成回调不在 co_spawn 的调用栈内执行
// ============================================================================
asio::awaitable<int> asio_ready() { co_return 1; }
auto aw_ready() CO2_BEG(aw_task<int>, ()) { CO2_RETURN(1); }
CO2_END

void scenario_launch_completion() {
    trace_t a, b;
    {
        aw::manual_executor ctx;
        ctx.get_executor().post([&] {
            aw::co_spawn(ctx, aw_ready(), [&](std::exception_ptr, int v) { a.add("cb " + std::to_string(v)); });
            a.add("after co_spawn");
        });
        ctx.run();
    }
    {
        asio::io_context io;
        asio::post(io, [&] {
            asio::co_spawn(io, asio_ready(), [&](std::exception_ptr, int v) { b.add("cb " + std::to_string(v)); });
            b.add("after co_spawn");
        });
        io.run();
    }
    compare("launch completion", a, b);
}

} // namespace

int main() {
    scenario_nesting();
    scenario_exception();
    scenario_cancellation();
    scenario_strand();
    scenario_operators();
    scenario_affinity();
    scenario_launch_completion();
    std::cout << "asio parity: all scenarios match\n";
    return 0;
}
```

`tests/asio_parity/CMakeLists.txt`：

```cmake
# §14.3：与 asio 190a8d8 原生协程的行为对照，固定 C++20。asio 只用于测试和基准。
if(NOT AW_ASIO_ROOT)
    message(STATUS "AW_ASIO_ROOT not set: tests/asio_parity skipped")
    return()
endif()
add_executable(aw-asio-parity-test asio_parity_test.cpp)
aw_configure_test(aw-asio-parity-test)
set_target_properties(aw-asio-parity-test PROPERTIES CXX_STANDARD 20)
target_include_directories(aw-asio-parity-test SYSTEM PRIVATE ${AW_ASIO_ROOT}/asio/include)
target_compile_definitions(aw-asio-parity-test PRIVATE ASIO_STANDALONE ASIO_NO_DEPRECATED)
add_test(NAME aw-asio-parity-test COMMAND aw-asio-parity-test)
```

`tests/CMakeLists.txt` 追加：

```cmake
add_subdirectory(asio_parity)
```

`CMakeLists.txt`（整文件替换）：

```cmake
cmake_minimum_required(VERSION 3.21)

project(awaitable VERSION 0.1.0 LANGUAGES CXX)

find_package(Threads REQUIRED)

add_library(awaitable INTERFACE)
add_library(awaitable::awaitable ALIAS awaitable)
target_compile_features(awaitable INTERFACE cxx_std_14)
target_link_libraries(awaitable INTERFACE Threads::Threads)
# core 的预处理器元编程需要标准一致的 __VA_ARGS__ 展开（沿用 coro）。
target_compile_options(awaitable INTERFACE $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>)
target_include_directories(
    awaitable INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                        $<INSTALL_INTERFACE:include>)

set(AW_TEST_CXX_STANDARD 14 CACHE STRING "C++ standard used to build the awaitable tests (14, 17 or 20)")
set_property(CACHE AW_TEST_CXX_STANDARD PROPERTY STRINGS 14 17 20)

option(AW_BUILD_TESTS "Build awaitable tests" ${PROJECT_IS_TOP_LEVEL})
set(AW_ASIO_ROOT "" CACHE PATH "asio source tree (contains asio/include); enables tests/asio_parity and the asio benchmarks")

if(AW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
```

`tools/run-matrix.sh`（整文件替换）：

```bash
#!/usr/bin/env bash
# 用法：tools/run-matrix.sh [配置...]   （在仓库根目录运行）
# 配置名 = <preset>-cxx<14|17|20>，preset ∈ debug release asan tsan；默认跑全部 12 个。
# 编译器：AW_CXX（默认 /workspace/zigbin/zig-c++）；ASan+UBSan 用 AW_CXX_ASAN
# （默认 /workspace/zigbin/clang20++，带 compiler-rt 的 clang 20 + libc++）。
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
AW_CXX=${AW_CXX:-/workspace/zigbin/zig-c++}
AW_CXX_ASAN=${AW_CXX_ASAN:-/workspace/zigbin/clang20++}
# asio 对照组（tests/asio_parity）；设为空字符串可跳过。
AW_ASIO_ROOT=${AW_ASIO_ROOT-/workspace/asio}
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1}
configs=("$@")
if [ ${#configs[@]} -eq 0 ]; then
    for p in debug release asan tsan; do for s in 14 17 20; do configs+=("$p-cxx$s"); done; done
fi
status=0
for c in "${configs[@]}"; do
    preset=${c%-cxx*}; std=${c##*-cxx}
    cxx=$AW_CXX; [ "$preset" = asan ] && cxx=$AW_CXX_ASAN
    dir=$root/build/matrix/$c
    log=$root/build/matrix/$c.log
    mkdir -p "$root/build/matrix"
    if ! (cd "$root" && CXX=$cxx cmake --preset "$preset" -B "$dir" -DAW_TEST_CXX_STANDARD="$std" -DAW_ASIO_ROOT="$AW_ASIO_ROOT") > "$log" 2>&1; then
        echo "$c: CONFIGURE FAILED (see $log)"; status=1; continue
    fi
    if ! cmake --build "$dir" -j8 >> "$log" 2>&1; then
        echo "$c: BUILD FAILED (see $log)"; status=1; continue
    fi
    summary=$(cd "$dir" && ctest -j4 --timeout 300 --output-on-failure 2>&1 | tee -a "$log" | grep -E "tests passed")
    echo "$c: $summary"
    case $summary in "100% tests passed"*) ;; *) status=1;; esac
done
exit $status
```

- [ ] **Step 2：运行**

Run: `git -C /workspace/asio log -1 --format=%h && cmake --preset debug -DAW_ASIO_ROOT=/workspace/asio >/dev/null && cmake --build build/debug -j8 && ctest --test-dir build/debug -R '^aw-asio-parity' --output-on-failure`
Expected：先输出 `190a8d8`，再输出 `100% tests passed out of 1`。

如果某个场景不一致，测试会把两边的事件序列打到 stderr，并以 `asio parity mismatch in scenario <名称>` 开头。这时按 §14.3 改库代码，不要改 asio 一侧的期望。

- [ ] **Step 3：全矩阵（含 TSan）**

Run: `tools/run-matrix.sh`
Expected：12 行，每行都是 `<配置>: 100% tests passed out of 53`。

- [ ] **Step 4：提交**

```bash
git add CMakeLists.txt tools/run-matrix.sh tests/CMakeLists.txt tests/asio_parity
git commit -m "test(asio-parity): behaviour parity with asio 190a8d8 native coroutines"
```

### Task 19：并发压测

**交付物：** §14.6 的压测，共六项，每项跑若干轮：
- thread_pool 上大量 `co_spawn`；
- 跨线程完成；
- strand 串行性；
- strand 上的 `||` / `&&`；
- 取消和完成竞争（racy op：取消和完成从两个线程同时到达）；
- 多线程并发 `sync_wait`。

轮数：Release 30 轮，其余配置 10 轮。重点在 TSan 下没有报告。

**Files:**
- Create: `tests/stress/CMakeLists.txt`, `tests/stress/stress_test.cpp`
- Modify: `tests/CMakeLists.txt`（追加）

**Interfaces:**
- Consumes：
  - `<awaitable.hpp>`（Task 16）；
  - `tests/support/test_ops.hpp`、`tests/support/test_cancellable_ops.hpp`；
  - Task 1 搬入的 `awaitable/core/detail/move_only_function.hpp`（racy op 用它保存 handler），以及 Task 9 的 `get_associated_executor(handler)`。
- Produces：ctest 项 `aw-stress-test <rounds>`，`TIMEOUT 600`。每项输出一行 `<项>: <N> rounds ok`。

- [ ] **Step 1：写测试**

`tests/stress/stress_test.cpp`：

```cpp
// §14.6：thread_pool、co_spawn 跨线程完成、strand、|| / && 的取消竞争、取消与完成同时到达、
// sync_wait 两种顺序。用法：aw-stress-test <轮数>。

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <awaitable.hpp>

#include "awaitable/core/detail/move_only_function.hpp"
#include "check.hpp"
#include "test_ops.hpp"

namespace {

using namespace aw::awaitable_operators;
using pool_ex = aw::thread_pool::executor_type;
using strand_ex = aw::strand<pool_ex>;
template <class T> using pool_task = aw::awaitable<T, pool_ex>;
template <class T> using strand_task = aw::awaitable<T, strand_ex>;

// 等 n 个回调。
class latch {
  public:
    explicit latch(int n) : remaining_{n} {}
    void count_down() {
        if (remaining_.fetch_sub(1) == 1) done_.set_value();
    }
    void wait() { done_.get_future().wait(); }

  private:
    std::atomic<int> remaining_;
    std::promise<void> done_;
};

// ---- 1. thread_pool：大量执行线程，每个让出两次 ----
auto hop_twice(pool_ex ex, std::atomic<int>* sum) CO2_BEG(pool_task<void>, (ex, sum)) {
    CO2_AWAIT(aw::post(ex, aw::use_awaitable));
    CO2_AWAIT(aw::post(ex, aw::use_awaitable));
    sum->fetch_add(1);
    CO2_RETURN();
}
CO2_END

void thread_pool_round() {
    aw::thread_pool pool{4U};
    std::atomic<int> sum{0};
    constexpr int n = 2000;
    latch done{n};
    for (int i = 0; i != n; ++i)
        aw::co_spawn(pool, hop_twice(pool.get_executor(), &sum), [&](std::exception_ptr e) {
            AW_CHECK(!e);
            done.count_down();
        });
    done.wait();
    AW_CHECK(sum.load() == n);
}

// ---- 2. co_spawn 跨线程完成：操作在外部线程完成，回到池 ----
auto foreign(aw_test::foreign_threads* threads, pool_ex ex, int v) CO2_BEG(pool_task<int>, (threads, ex, v), int r{};) {
    CO2_AWAIT_SET(r, aw_test::async_from_foreign_thread(*threads, v, aw::use_awaitable));
    AW_CHECK(ex.running_in_this_thread());
    CO2_RETURN(r);
}
CO2_END

void cross_thread_round() {
    aw::thread_pool pool{4U};
    std::atomic<int> sum{0};
    constexpr int n = 64;
    latch done{n};
    {
        aw_test::foreign_threads threads;
        for (int i = 0; i != n; ++i)
            aw::co_spawn(pool, foreign(&threads, pool.get_executor(), i), [&](std::exception_ptr e, int v) {
                AW_CHECK(!e);
                sum.fetch_add(v);
                done.count_down();
            });
        done.wait();
    }
    AW_CHECK(sum.load() == n * (n - 1) / 2);
}

// ---- 3. strand：多个执行线程交替等待，互不重叠 ----
auto strand_worker(strand_ex s, int rounds, std::atomic<int>* inside, int* counter)
    CO2_BEG(strand_task<void>, (s, rounds, inside, counter), int k{};) {
    for (k = 0; k != rounds; ++k) {
        AW_CHECK(inside->fetch_add(1) == 0);
        ++*counter; // 只在 strand 上访问：TSan 能抓到违背串行化的情况
        inside->fetch_sub(1);
        CO2_AWAIT(aw::post(s, aw::use_awaitable));
    }
    CO2_RETURN();
}
CO2_END

void strand_round() {
    aw::thread_pool pool{4U};
    strand_ex s = aw::make_strand(pool.get_executor());
    std::atomic<int> inside{0};
    int counter = 0;
    constexpr int workers = 6;
    latch done{workers};
    for (int i = 0; i != workers; ++i)
        aw::co_spawn(s, strand_worker(s, 200, &inside, &counter), [&](std::exception_ptr e) {
            AW_CHECK(!e);
            done.count_down();
        });
    done.wait();
    std::promise<int> seen;
    s.post([&] { seen.set_value(counter); });
    AW_CHECK(seen.get_future().get() == workers * 200);
}

// ---- 4. || / && 的取消竞争（组合跑在 strand 上，§6.2 第 7 条）----
auto strand_delay(strand_ex s, int n) CO2_BEG(strand_task<int>, (s, n), int i{};) {
    for (i = 0; i != n; ++i) CO2_AWAIT(aw::post(s, aw::use_awaitable));
    CO2_RETURN(n);
}
CO2_END

auto strand_fail(strand_ex s, int n) CO2_BEG(strand_task<int>, (s, n), int i{};) {
    for (i = 0; i != n; ++i) CO2_AWAIT(aw::post(s, aw::use_awaitable));
    throw std::runtime_error{"fail"};
}
CO2_END

auto race(strand_ex s, int i) CO2_BEG(strand_task<int>, (s, i), aw::variant<int, int> v{}; std::tuple<int, int> t{};) {
    CO2_AWAIT_SET(v, strand_delay(s, i % 3) || strand_delay(s, (i + 1) % 3));
    CO2_AWAIT_SET(t, strand_delay(s, 1) && strand_delay(s, i % 2));
    CO2_RETURN(static_cast<int>(v.index()) + std::get<0>(t));
}
CO2_END

auto race_fail(strand_ex s, int i) CO2_BEG(strand_task<int>, (s, i), std::tuple<int, int> t{};) {
    CO2_AWAIT_SET(t, strand_fail(s, i % 2) && strand_delay(s, 2));
    CO2_RETURN(-1);
}
CO2_END

void operators_round() {
    aw::thread_pool pool{4U};
    constexpr int n = 300;
    latch done{2 * n};
    std::atomic<int> failures{0};
    for (int i = 0; i != n; ++i) {
        strand_ex s = aw::make_strand(pool.get_executor());
        aw::co_spawn(s, race(s, i), [&](std::exception_ptr e, int v) {
            AW_CHECK(!e && v >= 1 && v <= 2);
            done.count_down();
        });
        aw::co_spawn(s, race_fail(s, i), [&](std::exception_ptr e, int) {
            if (e) failures.fetch_add(1);
            done.count_down();
        });
    }
    done.wait();
    AW_CHECK(failures.load() == n);
}

// ---- 5. 取消与完成同时到达：先到者生效，回调恰好一次 ----
struct racy_state {
    explicit racy_state(strand_ex ex) : s{std::move(ex)} {}
    strand_ex s;
    co2::detail::MoveOnlyFunction<void(std::error_code, int)> handler;
    aw::cancellation_slot slot;
};

struct racy_cancel {
    std::shared_ptr<racy_state> st;
    void operator()(aw::cancellation_type) {
        // 在 strand 上（co_spawn 的父 slot 处理器先 dispatch 过来）。处理器正在运行，不能清 slot。
        if (!st->handler) return;
        auto h = std::move(st->handler);
        st->slot = aw::cancellation_slot{};
        st->s.post(aw_test::bind_completion(std::move(h), std::error_code{aw::error::operation_aborted}, 0));
    }
};

struct racy_complete {
    std::shared_ptr<racy_state> st;
    void operator()() {
        if (!st->handler) return; // 已被取消
        if (st->slot.is_connected()) st->slot.clear();
        auto h = std::move(st->handler);
        h(std::error_code{}, 1);
    }
};

struct initiate_racy {
    aw_test::foreign_threads* threads;
    template <class Handler> void operator()(Handler&& handler) const {
        using handler_type = typename std::decay<Handler>::type;
        strand_ex target = handler.get_executor(); // 只配合 use_awaitable：回调就是执行线程
        auto st = std::make_shared<racy_state>(target);
        aw::cancellation_slot slot = aw::get_associated_cancellation_slot(handler);
        st->handler = co2::detail::MoveOnlyFunction<void(std::error_code, int)>(handler_type(std::forward<Handler>(handler)));
        if (slot.is_connected()) {
            st->slot = slot;
            slot.template emplace<racy_cancel>(racy_cancel{st});
        }
        threads->start([st] { st->s.dispatch(racy_complete{st}); });
    }
};

template <class Token> auto async_racy(aw_test::foreign_threads& threads, Token&& token) {
    return aw::async_initiate<Token, void(std::error_code, int)>(initiate_racy{&threads}, token);
}

auto racy_body(aw_test::foreign_threads* threads) CO2_BEG(strand_task<int>, (threads), int v{};) {
    CO2_AWAIT_SET(v, async_racy(*threads, aw::use_awaitable));
    CO2_RETURN(v);
}
CO2_END

void cancel_vs_complete_round() {
    aw::thread_pool pool{4U};
    constexpr int n = 64;
    std::atomic<int> calls{0};
    std::atomic<int> aborted{0};
    latch done{n};
    {
        aw_test::foreign_threads threads;
        std::vector<std::unique_ptr<aw::cancellation_signal>> signals;
        for (int i = 0; i != n; ++i) {
            strand_ex s = aw::make_strand(pool.get_executor());
            signals.emplace_back(new aw::cancellation_signal);
            aw::co_spawn(s, racy_body(&threads),
                         aw::bind_cancellation_slot(signals.back()->slot(), [&](std::exception_ptr e, int) {
                             calls.fetch_add(1);
                             if (e) aborted.fetch_add(1);
                             done.count_down();
                         }));
        }
        // co_spawn 装的父 slot 处理器可以在任何线程 emit（§11）。
        for (auto& sig : signals) sig->emit(aw::cancellation_type::terminal);
        done.wait();
    }
    AW_CHECK(calls.load() == n);
    AW_CHECK(aborted.load() >= 0 && aborted.load() <= n);
}

// ---- 6. sync_wait 两种顺序 ----
auto ready(int v) CO2_BEG(pool_task<int>, (v)) { CO2_RETURN(v); }
CO2_END

auto later(aw_test::foreign_threads* threads, int v) CO2_BEG(pool_task<int>, (threads, v), int r{};) {
    CO2_AWAIT_SET(r, aw_test::async_from_foreign_thread(*threads, v, aw::use_awaitable));
    CO2_RETURN(r);
}
CO2_END

void sync_wait_round() {
    aw::thread_pool pool{2U};
    long sum = 0;
    for (int i = 0; i != 500; ++i) sum += aw::sync_wait(pool.get_executor(), ready(i));
    AW_CHECK(sum == 500L * 499L / 2L);
    aw_test::foreign_threads threads;
    for (int i = 0; i != 20; ++i) AW_CHECK(aw::sync_wait(pool.get_executor(), later(&threads, i)) == i);
}

} // namespace

int main(int argc, char** argv) {
    int const rounds = argc > 1 ? std::atoi(argv[1]) : 10;
    AW_CHECK(rounds > 0);
    struct item {
        char const* name;
        void (*fn)();
    };
    item const items[] = {{"thread_pool", &thread_pool_round},       {"cross_thread", &cross_thread_round},
                          {"strand", &strand_round},                 {"operators", &operators_round},
                          {"cancel_vs_complete", &cancel_vs_complete_round}, {"sync_wait", &sync_wait_round}};
    for (item const& it : items) {
        for (int r = 0; r != rounds; ++r) it.fn();
        std::printf("%s: %d rounds ok\n", it.name, rounds);
    }
    return 0;
}
```

`tests/stress/CMakeLists.txt`：

```cmake
# §14.6：并发压测。每项的轮数：Release 30，其余（Debug / ASan / TSan）10。
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(AW_STRESS_ROUNDS 30)
else()
    set(AW_STRESS_ROUNDS 10)
endif()
add_executable(aw-stress-test stress_test.cpp)
aw_configure_test(aw-stress-test)
add_test(NAME aw-stress-test COMMAND aw-stress-test ${AW_STRESS_ROUNDS})
set_tests_properties(aw-stress-test PROPERTIES TIMEOUT 600)
```

`tests/CMakeLists.txt` 追加：

```cmake
add_subdirectory(stress)
```

- [ ] **Step 2：运行**

Run: `cmake --preset debug -DAW_ASIO_ROOT=/workspace/asio >/dev/null && cmake --build build/debug -j8 && build/debug/tests/stress/aw-stress-test 2`
Expected：

```text
thread_pool: 2 rounds ok
cross_thread: 2 rounds ok
strand: 2 rounds ok
operators: 2 rounds ok
cancel_vs_complete: 2 rounds ok
sync_wait: 2 rounds ok
```

- [ ] **Step 3：TSan 下连跑三遍，然后跑全矩阵**

Run: `tools/run-matrix.sh tsan-cxx14 && for i in 1 2 3; do build/matrix/tsan-cxx14/tests/stress/aw-stress-test 10 > /dev/null || echo FAIL; done; tools/run-matrix.sh`
Expected：
- 先输出 `tsan-cxx14: 100% tests passed out of 54`；
- 三遍压测都不输出 `FAIL`，也没有 `WARNING: ThreadSanitizer`；
- 最后 12 行，每行都是 `<配置>: 100% tests passed out of 54`。

- [ ] **Step 4：提交**

```bash
git add tests/CMakeLists.txt tests/stress
git commit -m "test(stress): concurrent spawn, cross-thread completion, strand, operators, cancel-vs-complete, sync_wait"
```

### Task 20：基准与验收检查

**交付物：** §15 的基准和 §15.4 第 2–4 条的自动验收检查。

基准项：
- `create_destroy`、`await_child`、`await_child_nested_1000`；
- `bridge_same_thread`、`bridge_cross_thread`；
- `co_spawn`、`strand_post`、`generator_yield_1000`；
- `or_ready`、`and_ready`。

对照组：aw（缓存开）、aw（缓存关，`AW_DISABLE_RECYCLING_ALLOCATOR`）、asio 190a8d8、coro（补丁后的测试夹具）、原生 C++20。每一项都记录中位耗时、每次的全局 `operator new` 次数和字节数，同时打印帧大小探针（§9.1）。

这一步只测量、只记录，不调优。验收结果不达标时，在 Step 5 停下，把数字交给用户决定。调优方案要依据实测数据，无法在计划里预先写成确切代码。

**Files:**
- Create:
  - `benchmarks/CMakeLists.txt`、`benchmarks/bench_common.hpp`；
  - `benchmarks/aw_bench.cpp`、`benchmarks/asio_bench.cpp`、`benchmarks/coro_bench.cpp`、`benchmarks/native_bench.cpp`；
  - `tools/run-benchmarks.sh`、`tools/check-acceptance.py`。
- Modify（整文件替换）：`CMakeLists.txt`（新增 `option(AW_BUILD_BENCHMARKS ... OFF)` 和 `add_subdirectory(benchmarks)`）。
- 产物（运行时生成，Step 5 提交）：`benchmarks/results/<日期>.md`、`benchmarks/results/<日期>.json`。

**Interfaces:**
- Consumes：
  - `<awaitable.hpp>`；
  - `tests/legacy/include` 夹具（coro 对照）；
  - `AW_ASIO_ROOT`（Task 18）；
  - `AW_DISABLE_RECYCLING_ALLOCATOR`（Task 3）。
- Produces：
  - `bench_common.hpp`：
    - 计数的全局 `operator new` / `delete`，在定义了 `__cpp_aligned_new` 时还包括对齐版本；
    - `bench::window(name)`：`start()` / `stop(ops)`，输出 `bench <项> ns=<每次纳秒> news=<每次 new 次数> bytes=<每次字节>`；
    - `bench::size_probe(name, size)`：输出 `size <名称> <字节>`，名称里可以有空格（解析时取最后一个字段作字节数）；
    - `completer` 线程：256 B 内联缓冲，job 在锁内运行；
  - 目标 `aw-bench`、`aw-bench-nocache`、`coro-bench`、`native-bench`；`AW_ASIO_ROOT` 非空时还有 `asio-bench`，它定义 `ASIO_DISABLE_STD_ALIGNED_ALLOC`，让 asio 的分配经过计数器；
  - `tools/run-benchmarks.sh`：
    - 环境变量 `AW_CXX`、`AW_ASIO_ROOT`、`AW_BENCH_CPU`（默认 2）、`AW_BENCH_RUNS`（默认 5）；
    - 用 taskset 绑核，每个二进制跑 N 次取中位数；
    - 输出 `.md` 和 `.json` 的路径；
  - `tools/check-acceptance.py [json]`：逐条输出 `PASS` / `FAIL` / `n/a`，最后一行是 `ACCEPTANCE OK` 或 `ACCEPTANCE FAILED (<n> failure(s))`；全部通过时退出码 0，否则 1。

- [ ] **Step 1：写基准公共件与各对照组**

`benchmarks/bench_common.hpp`：

```cpp
#pragma once

// 基准公共部分（§15.3）：替换全局 operator new / delete 统计次数与字节数；每项先预热 1000 次，
// 再测 N 次，输出一行：bench <项> ns=<每次耗时> news=<每次 new 次数> bytes=<每次字节数>。
// 本头文件只能被一个 TU 包含（它定义全局 operator new）。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>

namespace bench {

inline std::atomic<long>& news() noexcept {
    static std::atomic<long> n{0};
    return n;
}
inline std::atomic<long>& bytes() noexcept {
    static std::atomic<long> n{0};
    return n;
}

constexpr long warmup = 1000;

// 一次测量窗口：start() 记下时间和计数，stop(n) 输出每次操作的均值。
class window {
  public:
    explicit window(char const* name) noexcept : name_{name} {}
    void start() noexcept {
        news0_ = news().load();
        bytes0_ = bytes().load();
        t0_ = std::chrono::steady_clock::now();
    }
    void stop(long ops) const noexcept {
        auto const t1 = std::chrono::steady_clock::now();
        double const ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0_).count());
        double const n = static_cast<double>(ops);
        std::printf("bench %s ns=%.2f news=%.3f bytes=%.1f\n", name_, ns / n,
                    static_cast<double>(news().load() - news0_) / n, static_cast<double>(bytes().load() - bytes0_) / n);
        std::fflush(stdout);
    }

  private:
    char const* name_;
    long news0_{0};
    long bytes0_{0};
    std::chrono::steady_clock::time_point t0_{};
};

inline void size_probe(char const* name, std::size_t size) {
    std::printf("size %s %zu\n", name, size);
}

// 跨线程完成方：一个常驻线程，一次只处理一个任务（任务对象放在定长缓冲区里，不分配）。
class completer {
  public:
    completer() : thread_{[this] { loop(); }} {}
    completer(completer const&) = delete;
    completer& operator=(completer const&) = delete;
    ~completer() {
        {
            std::lock_guard<std::mutex> lock{mutex_};
            stop_ = true;
        }
        cv_.notify_one();
        thread_.join();
    }

    template <class F> void submit(F&& f) {
        using job_type = typename std::decay<F>::type;
        static_assert(sizeof(job_type) <= sizeof(buffer_), "job too large");
        std::lock_guard<std::mutex> lock{mutex_};
        ::new (static_cast<void*>(buffer_)) job_type(std::forward<F>(f));
        run_ = [](void* p) {
            job_type* job = static_cast<job_type*>(p);
            (*job)();
            job->~job_type();
        };
        cv_.notify_one();
    }

  private:
    void loop() {
        std::unique_lock<std::mutex> lock{mutex_};
        for (;;) {
            cv_.wait(lock, [this] { return stop_ || run_ != nullptr; });
            if (run_ == nullptr) return;
            // 持锁执行：任务派发出去的协程可能立即在别的线程再次 submit，必须等本任务析构完。
            void (*run)(void*) = run_;
            run_ = nullptr;
            run(buffer_);
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    alignas(std::max_align_t) unsigned char buffer_[256];
    void (*run_)(void*){nullptr};
    bool stop_{false};
    std::thread thread_;
};

} // namespace bench

void* operator new(std::size_t size) {
    bench::news().fetch_add(1, std::memory_order_relaxed);
    bench::bytes().fetch_add(static_cast<long>(size), std::memory_order_relaxed);
    if (void* p = std::malloc(size == 0U ? 1U : size)) return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

#if defined(__cpp_aligned_new)
// asio 在 C++17 及以上用对齐版 new 分配帧：同样计数。
void* operator new(std::size_t size, std::align_val_t align) {
    bench::news().fetch_add(1, std::memory_order_relaxed);
    bench::bytes().fetch_add(static_cast<long>(size), std::memory_order_relaxed);
    std::size_t const a = static_cast<std::size_t>(align);
    std::size_t const rounded = (size + a - 1U) / a * a;
    if (void* p = std::aligned_alloc(a, rounded == 0U ? a : rounded)) return p;
    throw std::bad_alloc{};
}
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
#endif
```

`benchmarks/aw_bench.cpp`：

```cpp
// §15.1 的本库基准（C++14）。aw-bench 打开回收缓存，aw-bench-nocache 定义 AW_DISABLE_RECYCLING_ALLOCATOR。
// 全部测量都在库线程上进行（manual_executor::run 期间或 thread_pool 工作线程上）。

#include <awaitable.hpp>

#include <cstdio>
#include <system_error>
#include <tuple>
#include <utility>

#include "bench_common.hpp"

namespace {

using namespace aw::awaitable_operators;
using ex_t = aw::manual_executor::executor_type;
template <class T> using task = aw::awaitable<T, ex_t>;
using pool_ex = aw::thread_pool::executor_type;

volatile long sink = 0;

auto leaf(int v) CO2_BEG(task<int>, (v)) { CO2_RETURN(v); }
CO2_END

auto chain(int depth) CO2_BEG(task<int>, (depth), int v{};) {
    if (depth == 0) CO2_RETURN(0);
    CO2_AWAIT_SET(v, chain(depth - 1));
    CO2_RETURN(v + 1);
}
CO2_END

auto numbers(int n) CO2_BEG((aw::async_generator<int, ex_t>), (n), int i{};) {
    for (i = 0; i != n; ++i) CO2_YIELD(i);
    CO2_RETURN();
}
CO2_END

// 1. 创建与销毁（不运行）
void create_destroy(long n) {
    for (long i = 0; i != bench::warmup; ++i) { task<int> a = leaf(1); }
    bench::window w{"create_destroy"};
    w.start();
    for (long i = 0; i != n; ++i) { task<int> a = leaf(static_cast<int>(i)); }
    w.stop(n);
}

// 2. 等待子协程：单层与 1000 层
auto await_child(long n) CO2_BEG(task<void>, (n), long i{}; int v{}; bench::window w{"await_child"};) {
    for (i = 0; i != bench::warmup; ++i) CO2_AWAIT_SET(v, leaf(1));
    w.start();
    for (i = 0; i != n; ++i) {
        CO2_AWAIT_SET(v, leaf(static_cast<int>(i)));
        sink = sink + v;
    }
    w.stop(n);
    CO2_RETURN();
}
CO2_END

auto await_nested(long n) CO2_BEG(task<void>, (n), long i{}; int v{}; bench::window w{"await_child_nested_1000"};) {
    for (i = 0; i != 10; ++i) CO2_AWAIT_SET(v, chain(1000));
    w.start();
    for (i = 0; i != n; ++i) CO2_AWAIT_SET(v, chain(1000));
    w.stop(n);
    sink = v;
    CO2_RETURN();
}
CO2_END

// 3. 异步桥接：同线程（完成方在同一个 manual_executor 上 post）
auto bridge_same(ex_t ex, long n) CO2_BEG(task<void>, (ex, n), long i{}; bench::window w{"bridge_same_thread"};) {
    for (i = 0; i != bench::warmup; ++i) CO2_AWAIT(aw::post(ex, aw::use_awaitable));
    w.start();
    for (i = 0; i != n; ++i) CO2_AWAIT(aw::post(ex, aw::use_awaitable));
    w.stop(n);
    CO2_RETURN();
}
CO2_END

// 3'. 异步桥接：外部线程完成，回到 thread_pool
template <class Handler> struct cross_job {
    Handler handler;
    void operator()() {
        auto target = aw::get_associated_executor(handler, 0);
        target.dispatch(std::move(handler));
    }
};
struct initiate_cross {
    bench::completer* c;
    template <class Handler> void operator()(Handler&& h) const {
        c->submit(cross_job<typename std::decay<Handler>::type>{std::forward<Handler>(h)});
    }
};
template <class Token> auto async_cross(bench::completer& c, Token&& token) {
    return aw::async_initiate<Token, void()>(initiate_cross{&c}, token);
}

auto bridge_cross(bench::completer* c, long n)
    CO2_BEG((aw::awaitable<void, pool_ex>), (c, n), long i{}; bench::window w{"bridge_cross_thread"};) {
    for (i = 0; i != bench::warmup; ++i) CO2_AWAIT(async_cross(*c, aw::use_awaitable));
    w.start();
    for (i = 0; i != n; ++i) CO2_AWAIT(async_cross(*c, aw::use_awaitable));
    w.stop(n);
    CO2_RETURN();
}
CO2_END

// 4. co_spawn + 完成回调（回调里发起下一次，同一时刻只有一个执行线程）
struct spawn_chain {
    aw::manual_executor* ctx;
    long* left;
    bench::window* w;
    long total;
    void operator()(std::exception_ptr, int v) const {
        sink = sink + v;
        if (--*left == 0) {
            if (w != nullptr) w->stop(total);
            return;
        }
        aw::co_spawn(*ctx, leaf(1), *this);
    }
};

// 5. strand 投递（任务里投递下一个）
struct strand_chain {
    aw::strand<ex_t> s;
    long* left;
    bench::window* w;
    long total;
    void operator()() const {
        if (--*left == 0) {
            if (w != nullptr) w->stop(total);
            return;
        }
        s.post(*this);
    }
};

// 6. async_generator yield 1000 次
auto generator_1000(long n)
    CO2_BEG(task<void>, (n), long i{}; aw::async_generator<int, ex_t> g{}; aw::optional<int> v{};
            bench::window w{"generator_yield_1000"};) {
    for (i = -10; i != n; ++i) {
        if (i == 0) w.start();
        g = numbers(1000);
        for (;;) {
            CO2_AWAIT_SET(v, g.next());
            if (!v) break;
            sink = sink + *v;
        }
    }
    w.stop(n);
    CO2_RETURN();
}
CO2_END

// 7. || 与 &&（两个就绪的操作数）
auto or_ready(long n) CO2_BEG(task<void>, (n), long i{}; aw::variant<int, int> r{}; bench::window w{"or_ready"};) {
    for (i = 0; i != bench::warmup; ++i) CO2_AWAIT_SET(r, leaf(1) || leaf(2));
    w.start();
    for (i = 0; i != n; ++i) CO2_AWAIT_SET(r, leaf(1) || leaf(2));
    w.stop(n);
    CO2_RETURN();
}
CO2_END

auto and_ready(long n) CO2_BEG(task<void>, (n), long i{}; std::tuple<int, int> r{}; bench::window w{"and_ready"};) {
    for (i = 0; i != bench::warmup; ++i) CO2_AWAIT_SET(r, leaf(1) && leaf(2));
    w.start();
    for (i = 0; i != n; ++i) CO2_AWAIT_SET(r, leaf(1) && leaf(2));
    w.stop(n);
    CO2_RETURN();
}
CO2_END

template <class A> void run_on(aw::manual_executor& ctx, A a) {
    aw::co_spawn(ctx, std::move(a), aw::detached);
    ctx.run();
}

} // namespace

int main() {
    bench::size_probe("awaitable_frame_base<manual>", sizeof(aw::detail::awaitable_frame_base<ex_t>));
    bench::size_probe("awaitable_frame<int,manual>", sizeof(aw::detail::awaitable_frame<int, ex_t>));
    bench::size_probe("entry_frame_impl<manual,int,spawn_chain>",
                      sizeof(aw::detail::entry_frame_impl<ex_t, int, spawn_chain>));
    bench::size_probe("async_generator next_awaiter", sizeof(aw::async_generator<int, ex_t>::next_awaiter));

    aw::manual_executor ctx;
    ctx.get_executor().post([] { create_destroy(2000000); });
    ctx.run();
    run_on(ctx, await_child(2000000));
    run_on(ctx, await_nested(2000));
    run_on(ctx, bridge_same(ctx.get_executor(), 2000000));
    {
        aw::thread_pool pool{1U};
        bench::completer c;
        aw::co_spawn(pool, bridge_cross(&c, 100000), aw::use_future).get();
    }
    {
        long left = bench::warmup;
        aw::co_spawn(ctx, leaf(1), spawn_chain{&ctx, &left, nullptr, 0});
        ctx.run();
        long const n = 1000000;
        bench::window w{"co_spawn"};
        left = n;
        ctx.get_executor().post([&] {
            w.start();
            aw::co_spawn(ctx, leaf(1), spawn_chain{&ctx, &left, &w, n});
        });
        ctx.run();
    }
    {
        aw::strand<ex_t> s = aw::make_strand(ctx.get_executor());
        long left = bench::warmup;
        s.post(strand_chain{s, &left, nullptr, 0});
        ctx.run();
        long const n = 2000000;
        bench::window w{"strand_post"};
        left = n;
        ctx.get_executor().post([&] {
            w.start();
            s.post(strand_chain{s, &left, &w, n});
        });
        ctx.run();
    }
    run_on(ctx, generator_1000(2000));
    run_on(ctx, or_ready(300000));
    run_on(ctx, and_ready(300000));
    return 0;
}
```

`benchmarks/asio_bench.cpp`：

```cpp
// §15.1 的 asio 190a8d8 对照（C++20 原生协程）。同线程项用单线程 io_context，跨线程项用
// asio::thread_pool(1) 加一个外部完成线程。项名与 aw_bench.cpp 相同。

#include <asio.hpp>
#include <asio/experimental/awaitable_operators.hpp>

#include <cstdio>
#include <exception>
#include <tuple>
#include <utility>
#include <variant>

#include "bench_common.hpp"

namespace {

using namespace asio::experimental::awaitable_operators;
volatile long sink = 0;

asio::awaitable<int> leaf(int v) { co_return v; }

asio::awaitable<int> chain(int depth) {
    if (depth == 0) co_return 0;
    int v = co_await chain(depth - 1);
    co_return v + 1;
}

void create_destroy(long n) {
    for (long i = 0; i != bench::warmup; ++i) { auto a = leaf(1); }
    bench::window w{"create_destroy"};
    w.start();
    for (long i = 0; i != n; ++i) { auto a = leaf(static_cast<int>(i)); }
    w.stop(n);
}

asio::awaitable<void> await_child(long n) {
    for (long i = 0; i != bench::warmup; ++i) sink = sink + co_await leaf(1);
    bench::window w{"await_child"};
    w.start();
    for (long i = 0; i != n; ++i) sink = sink + co_await leaf(static_cast<int>(i));
    w.stop(n);
}

asio::awaitable<void> await_nested(long n) {
    for (long i = 0; i != 10; ++i) sink = co_await chain(1000);
    bench::window w{"await_child_nested_1000"};
    w.start();
    for (long i = 0; i != n; ++i) sink = co_await chain(1000);
    w.stop(n);
}

asio::awaitable<void> bridge_same(long n) {
    auto ex = co_await asio::this_coro::executor;
    for (long i = 0; i != bench::warmup; ++i) co_await asio::post(ex, asio::use_awaitable);
    bench::window w{"bridge_same_thread"};
    w.start();
    for (long i = 0; i != n; ++i) co_await asio::post(ex, asio::use_awaitable);
    w.stop(n);
}

template <class Handler> struct cross_job {
    Handler handler;
    void operator()() {
        auto ex = asio::get_associated_executor(handler);
        asio::dispatch(ex, std::move(handler));
    }
};
template <class Token> auto async_cross(bench::completer& c, Token&& token) {
    return asio::async_initiate<Token, void()>(
        [&c](auto handler) { c.submit(cross_job<decltype(handler)>{std::move(handler)}); }, token);
}

asio::awaitable<void> bridge_cross(bench::completer& c, long n) {
    for (long i = 0; i != bench::warmup; ++i) co_await async_cross(c, asio::use_awaitable);
    bench::window w{"bridge_cross_thread"};
    w.start();
    for (long i = 0; i != n; ++i) co_await async_cross(c, asio::use_awaitable);
    w.stop(n);
}

struct spawn_chain {
    asio::io_context* io;
    long* left;
    bench::window* w;
    long total;
    void operator()(std::exception_ptr, int v) const {
        sink = sink + v;
        if (--*left == 0) {
            if (w != nullptr) w->stop(total);
            return;
        }
        asio::co_spawn(*io, leaf(1), *this);
    }
};

struct strand_chain {
    asio::strand<asio::io_context::executor_type> s;
    long* left;
    bench::window* w;
    long total;
    void operator()() const {
        if (--*left == 0) {
            if (w != nullptr) w->stop(total);
            return;
        }
        asio::post(s, *this);
    }
};

asio::awaitable<void> or_ready(long n) {
    for (long i = 0; i != bench::warmup; ++i) sink = static_cast<long>((co_await (leaf(1) || leaf(2))).index());
    bench::window w{"or_ready"};
    w.start();
    for (long i = 0; i != n; ++i) sink = static_cast<long>((co_await (leaf(1) || leaf(2))).index());
    w.stop(n);
}

asio::awaitable<void> and_ready(long n) {
    for (long i = 0; i != bench::warmup; ++i) sink = std::get<0>(co_await (leaf(1) && leaf(2)));
    bench::window w{"and_ready"};
    w.start();
    for (long i = 0; i != n; ++i) sink = std::get<0>(co_await (leaf(1) && leaf(2)));
    w.stop(n);
}

template <class A> void run_on(asio::io_context& io, A a) {
    asio::co_spawn(io, std::move(a), asio::detached);
    io.restart();
    io.run();
}

} // namespace

int main() {
    asio::io_context io{1};
    asio::post(io, [] { create_destroy(2000000); });
    io.run();
    run_on(io, await_child(2000000));
    run_on(io, await_nested(2000));
    run_on(io, bridge_same(2000000));
    {
        asio::thread_pool pool{1};
        bench::completer c;
        asio::co_spawn(pool, bridge_cross(c, 100000), asio::use_future).get();
        pool.join();
    }
    {
        long left = bench::warmup;
        asio::co_spawn(io, leaf(1), spawn_chain{&io, &left, nullptr, 0});
        io.restart();
        io.run();
        long const n = 1000000;
        bench::window w{"co_spawn"};
        left = n;
        asio::post(io, [&] {
            w.start();
            asio::co_spawn(io, leaf(1), spawn_chain{&io, &left, &w, n});
        });
        io.restart();
        io.run();
    }
    {
        auto s = asio::make_strand(io.get_executor());
        long left = bench::warmup;
        asio::post(s, strand_chain{s, &left, nullptr, 0});
        io.restart();
        io.run();
        long const n = 2000000;
        bench::window w{"strand_post"};
        left = n;
        asio::post(io, [&] {
            w.start();
            asio::post(s, strand_chain{s, &left, &w, n});
        });
        io.restart();
        io.run();
    }
    run_on(io, or_ready(300000));
    run_on(io, and_ready(300000));
    return 0;
}
```

`benchmarks/coro_bench.cpp`：

```cpp
// §15.2：coro（补丁后的测试夹具 co2::Task / spawn / whenAll / AsyncGenerator）对照，C++14。

#include <cstdio>
#include <tuple>

#include "co2/async_generator.hpp"
#include "co2/manual_executor.hpp"
#include "co2/spawn.hpp"
#include "co2/sync_wait.hpp"
#include "co2/task.hpp"
#include "co2/when_all.hpp"

#include "bench_common.hpp"

namespace {

volatile long sink = 0;

auto leaf(int v) CO2_BEG(co2::Task<int>, (v)) { CO2_RETURN(v); }
CO2_END

auto chain(int depth) CO2_BEG(co2::Task<int>, (depth), int v{};) {
    if (depth == 0) CO2_RETURN(0);
    CO2_AWAIT_SET(v, chain(depth - 1));
    CO2_RETURN(v + 1);
}
CO2_END

auto numbers(int n) CO2_BEG(co2::AsyncGenerator<int>, (n), int i{};) {
    for (i = 0; i != n; ++i) CO2_YIELD(i);
    CO2_RETURN();
}
CO2_END

auto await_child(long n) CO2_BEG(co2::Task<int>, (n), long i{}; int v{}; bench::window w{"await_child"};) {
    for (i = 0; i != bench::warmup; ++i) CO2_AWAIT_SET(v, leaf(1));
    w.start();
    for (i = 0; i != n; ++i) {
        CO2_AWAIT_SET(v, leaf(static_cast<int>(i)));
        sink = sink + v;
    }
    w.stop(n);
    CO2_RETURN(0);
}
CO2_END

auto await_nested(long n) CO2_BEG(co2::Task<int>, (n), long i{}; int v{}; bench::window w{"await_child_nested_1000"};) {
    for (i = 0; i != 10; ++i) CO2_AWAIT_SET(v, chain(1000));
    w.start();
    for (i = 0; i != n; ++i) CO2_AWAIT_SET(v, chain(1000));
    w.stop(n);
    CO2_RETURN(v);
}
CO2_END

auto generator_1000(long n) CO2_BEG(co2::Task<int>, (n), long i{}; co2::AsyncGenerator<int> g{numbers(0)}; bool has{};
                                    bench::window w{"generator_yield_1000"};) {
    for (i = -10; i != n; ++i) {
        if (i == 0) w.start();
        g = numbers(1000);
        for (;;) {
            CO2_AWAIT_SET(has, g.next());
            if (!has) break;
            sink = sink + g.value();
        }
    }
    w.stop(n);
    CO2_RETURN(0);
}
CO2_END

auto and_ready(long n) CO2_BEG(co2::Task<int>, (n), long i{}; std::tuple<int, int> r{}; bench::window w{"and_ready"};) {
    for (i = 0; i != bench::warmup; ++i) CO2_AWAIT_SET(r, co2::whenAll(leaf(1), leaf(2)));
    w.start();
    for (i = 0; i != n; ++i) CO2_AWAIT_SET(r, co2::whenAll(leaf(1), leaf(2)));
    w.stop(n);
    CO2_RETURN(std::get<0>(r));
}
CO2_END

} // namespace

int main() {
    for (long i = 0; i != bench::warmup; ++i) { auto a = leaf(1); }
    {
        long const n = 2000000;
        bench::window w{"create_destroy"};
        w.start();
        for (long i = 0; i != n; ++i) { auto a = leaf(static_cast<int>(i)); }
        w.stop(n);
    }
    sink = co2::syncWait(await_child(2000000));
    sink = co2::syncWait(await_nested(2000));
    {
        co2::ManualExecutor executor;
        for (long i = 0; i != bench::warmup; ++i) {
            auto h = co2::spawn(executor, leaf(1));
            executor.run();
            sink = h.join();
        }
        long const n = 1000000;
        bench::window w{"co_spawn"};
        w.start();
        for (long i = 0; i != n; ++i) {
            auto h = co2::spawn(executor, leaf(1));
            executor.run();
            sink = h.join();
        }
        w.stop(n);
    }
    sink = co2::syncWait(generator_1000(2000));
    sink = co2::syncWait(and_ready(300000));
    return 0;
}
```

`benchmarks/native_bench.cpp`：

```cpp
// §15.2：同编译器下的最小原生 C++20 task（对称转移），只用于第 1、2 项。

#include <coroutine>
#include <cstdio>
#include <exception>
#include <utility>

#include "bench_common.hpp"

namespace {

volatile long sink = 0;

template <class T> struct task {
    struct promise_type {
        T value{};
        std::coroutine_handle<> continuation{std::noop_coroutine()};
        task get_return_object() { return task{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }
        struct final_awaiter {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                return h.promise().continuation;
            }
            void await_resume() noexcept {}
        };
        final_awaiter final_suspend() noexcept { return {}; }
        void return_value(T v) { value = v; }
        void unhandled_exception() { std::terminate(); }
    };
    explicit task(std::coroutine_handle<promise_type> h) : handle{h} {}
    task(task&& o) noexcept : handle{std::exchange(o.handle, {})} {}
    ~task() {
        if (handle) handle.destroy();
    }
    bool await_ready() noexcept { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> c) noexcept {
        handle.promise().continuation = c;
        return handle;
    }
    T await_resume() { return handle.promise().value; }
    std::coroutine_handle<promise_type> handle;
};

task<int> leaf(int v) { co_return v; }

task<int> chain(int depth) {
    if (depth == 0) co_return 0;
    int v = co_await chain(depth - 1);
    co_return v + 1;
}

task<int> await_child(long n) {
    for (long i = 0; i != bench::warmup; ++i) sink = sink + co_await leaf(1);
    bench::window w{"await_child"};
    w.start();
    for (long i = 0; i != n; ++i) sink = sink + co_await leaf(static_cast<int>(i));
    w.stop(n);
    co_return 0;
}

task<int> await_nested(long n) {
    for (long i = 0; i != 10; ++i) sink = co_await chain(1000);
    bench::window w{"await_child_nested_1000"};
    w.start();
    for (long i = 0; i != n; ++i) sink = co_await chain(1000);
    w.stop(n);
    co_return 0;
}

void run(task<int> t) { t.handle.resume(); }

} // namespace

int main() {
    for (long i = 0; i != bench::warmup; ++i) { auto a = leaf(1); }
    {
        long const n = 2000000;
        bench::window w{"create_destroy"};
        w.start();
        for (long i = 0; i != n; ++i) { auto a = leaf(static_cast<int>(i)); }
        w.stop(n);
    }
    run(await_child(2000000));
    run(await_nested(2000));
    return 0;
}
```

`benchmarks/CMakeLists.txt`：

```cmake
# §15：基准。-DAW_BUILD_BENCHMARKS=ON 时构建（tools/run-benchmarks.sh 这样配置）；编译选项统一 -O2 -DNDEBUG。
function(aw_bench_target target source std)
    add_executable(${target} ${source})
    set_target_properties(${target} PROPERTIES CXX_STANDARD ${std} CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_compile_options(${target} PRIVATE -O2 -Wall -Wextra -Wpedantic)
    target_compile_definitions(${target} PRIVATE NDEBUG)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()

aw_bench_target(aw-bench aw_bench.cpp 14)
target_link_libraries(aw-bench PRIVATE awaitable::awaitable)

aw_bench_target(aw-bench-nocache aw_bench.cpp 14)
target_link_libraries(aw-bench-nocache PRIVATE awaitable::awaitable)
target_compile_definitions(aw-bench-nocache PRIVATE AW_DISABLE_RECYCLING_ALLOCATOR)

# coro 对照：补丁后的测试夹具（tests/legacy/include），夹具头经转发头接入 core。
aw_bench_target(coro-bench coro_bench.cpp 14)
target_link_libraries(coro-bench PRIVATE awaitable::awaitable)
target_include_directories(coro-bench PRIVATE ${PROJECT_SOURCE_DIR}/tests/legacy/include)

aw_bench_target(native-bench native_bench.cpp 20)

if(AW_ASIO_ROOT)
    aw_bench_target(asio-bench asio_bench.cpp 20)
    target_include_directories(asio-bench SYSTEM PRIVATE ${AW_ASIO_ROOT}/asio/include)
    # 让 asio 的帧分配走 ::operator new，计数分配器才能看到（否则走 std::aligned_alloc）。
    target_compile_definitions(asio-bench PRIVATE ASIO_STANDALONE ASIO_NO_DEPRECATED ASIO_DISABLE_STD_ALIGNED_ALLOC)
    find_package(Threads REQUIRED)
    target_link_libraries(asio-bench PRIVATE Threads::Threads)
endif()
```

`CMakeLists.txt`（整文件替换）：

```cmake
cmake_minimum_required(VERSION 3.21)

project(awaitable VERSION 0.1.0 LANGUAGES CXX)

find_package(Threads REQUIRED)

add_library(awaitable INTERFACE)
add_library(awaitable::awaitable ALIAS awaitable)
target_compile_features(awaitable INTERFACE cxx_std_14)
target_link_libraries(awaitable INTERFACE Threads::Threads)
# core 的预处理器元编程需要标准一致的 __VA_ARGS__ 展开（沿用 coro）。
target_compile_options(awaitable INTERFACE $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>)
target_include_directories(
    awaitable INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                        $<INSTALL_INTERFACE:include>)

set(AW_TEST_CXX_STANDARD 14 CACHE STRING "C++ standard used to build the awaitable tests (14, 17 or 20)")
set_property(CACHE AW_TEST_CXX_STANDARD PROPERTY STRINGS 14 17 20)

option(AW_BUILD_TESTS "Build awaitable tests" ${PROJECT_IS_TOP_LEVEL})
set(AW_ASIO_ROOT "" CACHE PATH "asio source tree (contains asio/include); enables tests/asio_parity and the asio benchmarks")

option(AW_BUILD_BENCHMARKS "Build awaitable benchmarks (benchmarks/)" OFF)

if(AW_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(AW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
```

- [ ] **Step 2：写运行脚本和验收脚本**

`tools/run-benchmarks.sh`：

```bash
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
```

`tools/check-acceptance.py`：

```python
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
```

Run: `chmod +x tools/run-benchmarks.sh tools/check-acceptance.py`

- [ ] **Step 3：冒烟（单次运行）**

Run: `AW_BENCH_RUNS=1 tools/run-benchmarks.sh && rg -c '^bench ' build/bench/raw/aw-bench.1.out build/bench/raw/asio-bench.1.out`
Expected：
- 先输出 `benchmarks/results/<日期>.md` 的绝对路径；
- 然后是 `build/bench/raw/aw-bench.1.out:10` 和 `build/bench/raw/asio-bench.1.out:9`（asio 没有 `generator_yield_1000`）。

`build/bench/raw/configure.log` 或 `build/bench/raw/build.log` 里有错误时，先修复，再继续。

- [ ] **Step 4：正式运行与验收**

Run: `rm -f benchmarks/results/$(date +%F).*; tools/run-benchmarks.sh && tools/check-acceptance.py; echo "exit=$?"`
Expected：
- 输出 20 行 `PASS` / `FAIL` / `n/a`，最后是 `ACCEPTANCE ...` 一行；
- `benchmarks/results/<日期>.md` 里有一张 10 行的对照表，以及一行帧大小探针。

写计划时，在 stage 原型上实测（clang 22.1.8，Xeon，5 次取中位数）的结果是 `ACCEPTANCE FAILED (5 failure(s))`。失败的五项：
- `allocations or_ready`：aw 9 次，asio 5 次；
- `allocations and_ready`：aw 9 次，asio 5 次；
- `time await_child`：aw/asio = 1.36；
- `time bridge_cross_thread`：aw/asio = 1.38；
- `zero-allocation bridge_cross_thread`：1 new/op。完成发生在非库线程上，回收缓存按设计不在那里工作，asio 在这一项同样是 1 次。

其余 15 项是 `PASS` 或 `n/a`。

- [ ] **Step 5：提交结果，然后在用户决策点停下**

```bash
git add CMakeLists.txt benchmarks tools/run-benchmarks.sh tools/check-acceptance.py
git commit -m "bench: aw vs asio vs coro vs native benchmarks, acceptance check (§15)"
```

如果 Step 4 是 `ACCEPTANCE OK`，这个计划到此完成。

如果有 `FAIL`，不要自行调优，也不要放宽阈值。把 `benchmarks/results/<日期>.md` 和 `FAIL` 行交给用户，由用户选择下一步：
- 针对单项调优，例如 `||` / `&&` 的子协程入口帧和共享状态合并分配，或者 `await_child` 的快路径；
- 修订 §15.4 的验收条款，例如把异线程完成从零分配要求中豁免；
- 接受现状。
