# awaitable 设计规格增补：有界 channel（v1）

日期：2026-11-13。主规格见 `2026-11-12-awaitable-design.md`，本文沿用其术语（§0）、取消规则（§6）、
executor 亲和（§7.3）、操作作者契约（§8.1）和回收缓存（§9.2）。

## C1. 背景与目标

`async_generator` 是拉式的：消费者 `next()` 一次，生产者才前进到下一个 `yield`，生成器里最多一个值。
背压是天然的，但真实视频源（相机、解码器、网络）不会因为没人拉就停下。需要一个**有界队列**，
满了按策略处理：

| 策略 | 满队列时 | 典型场景 |
| --- | --- | --- |
| `block` | 发送方等待，直到有空位 | 录制、转码，不能丢帧 |
| `drop_oldest` | 丢掉队首（最旧）元素，新元素入队 | 实时预览，低延迟 |
| `drop_newest` | 丢掉新来的元素，队列不变 | 只要连续的前段（例如抓拍、按序分析） |

参照 asio `experimental::basic_channel`（`asio/experimental/channel.hpp`、
`detail/channel_service.hpp`），接口取其子集；下面列出偏离。

非目标（v1 不做）：多签名 channel、`reset()`、`async_send` 的批量形式、`select`、
无锁实现、`channel` 与 `async_generator` 的库内适配器（示例里给出 5 行写法）。

## C2. 接口

头文件 `awaitable/channel.hpp`（错误码在 `awaitable/channel_error.hpp`），总头文件包含两者。

```cpp
namespace aw {
enum class channel_policy { block, drop_oldest, drop_newest };

enum class channel_errc { channel_closed = 1, channel_cancelled = 2 };   // 类别名 "aw.channel"
std::error_code make_error_code(channel_errc);                            // is_error_code_enum 特化

template <class T, class Executor> class channel {                       // 不可复制、不可移动
public:
    using value_type    = T;
    using executor_type = Executor;
    using receive_type  = T 可默认构造 ? T : aw::optional<T>;          // 同 D8

    channel(Executor const& ex, std::size_t capacity, channel_policy policy = channel_policy::block);
    template <class Context> channel(Context& ctx, std::size_t capacity, channel_policy = block); // Context 有 get_executor()
    ~channel();

    executor_type get_executor() const noexcept;
    std::size_t capacity() const noexcept;
    channel_policy policy() const noexcept;

    // 完成签名 void(std::error_code)
    template <class Token> auto async_send(T value, Token&& token);
    // 完成签名 void(std::error_code, receive_type)
    template <class Token> auto async_receive(Token&& token);

    bool try_send(T&& value);            // 被 channel 接收（入队、交给接收方或按策略丢弃）返回 true
    bool try_send(T const& value);
    aw::optional<T> try_receive();       // 没有可取的元素返回空

    void close();                        // 幂等
    void cancel();                       // 所有挂起的操作以 channel_cancelled 完成；channel 保持打开
    bool is_open() const;
    std::size_t size() const;            // 缓冲区里的元素数（快照）
    std::uint64_t dropped() const;       // 按策略丢弃的元素总数

    template <class F> void set_drop_handler(F&& f);   // f(T&&)；须在并发使用之前调用
};
}
```

用法：

```cpp
CO2_AWAIT(ch.async_send(std::move(frame), aw::use_awaitable));                 // 关闭时抛 system_error
CO2_AWAIT_SET(r, ch.async_receive(aw::as_tuple(aw::use_awaitable)));           // r: tuple<error_code, T>
if (std::get<0>(r) == aw::channel_errc::channel_closed) ...                      // 读完了
```

外部线程（例如相机回调线程）：`try_send` 不阻塞；`ch.async_send(v, aw::use_future).get()` 阻塞等待。

## C3. 容量与策略

1. **容量 0（rendezvous）** 只允许 `block` 策略：发送方等到有接收方取走值才完成。`drop_*` 配容量 0
   是契约违规（`CO2_CONTRACT_CHECK`，与主规格 §14.4 同一机制），因为“满了丢”在没有缓冲区时没有意义。
2. **缓冲区** 在构造时一次分配 `capacity` 个元素的环形存储，之后不再分配。
3. 有接收方在等时（此时缓冲区一定为空），发送的值直接交给最早的接收方，不进缓冲区。
4. **满队列：**
   - `block`：`async_send` 进入发送方等待队列（值存在等待节点里）；`try_send` 返回 false，值不被移走。
   - `drop_oldest`：弹出队首元素作为“被丢弃”，新值入队；`async_send` 以成功完成。
   - `drop_newest`：新值作为“被丢弃”；`async_send` 以成功完成，`try_send` 返回 true。
   被丢弃的元素都计入 `dropped()`。`try_send` 的返回值表示“值已被 channel 接收”，被策略丢弃也算接收。
5. **被丢弃元素的通知：** `dropped()` 计数；可选的 `set_drop_handler(f)`，`f(T&&)` 在发起发送的线程上、
   释放内部锁之后、发送返回（或 initiation 返回）之前同步调用，可以把帧缓冲还给缓冲池。
   `f` 不允许抛出：抛出时 `std::terminate()`（它在 initiation 内运行，没有可交付错误的地方）。
   没有设置时被丢弃的元素直接析构（同样在锁外）。
6. **接收：** 缓冲区非空取队首；`block` 策略下若有发送方在等，把最早一个发送方的值补进队尾并完成它。
   容量 0 时直接从最早的发送方取值。

## C4. 关闭与取消

1. **`close()`：** 之后的发送立即以 `channel_closed` 完成（`try_send` 返回 false，值不被移走）；
   正在等待的发送方以 `channel_closed` 完成，值被丢弃（不计入 `dropped()`）；
   接收方先取完缓冲区里剩下的元素，之后以 `channel_closed` 完成（`try_receive` 返回空）。与 asio 相同。
2. **`cancel()`：** 所有等待中的操作以 `channel_cancelled` 完成，缓冲区保留，channel 保持打开。与 asio 相同。
3. **按操作取消（slot，主机制）：** 只有进入等待队列的操作才在完成回调的关联 slot 上安装处理器
   （立即完成的操作不装，不分配）。收到任何非 none 的取消类型都把操作摘出队列，以
   `aw::error::operation_aborted` 完成。等待中的操作没有副作用，所以 `terminal` / `partial` / `total`
   三级都支持（被取消的发送不会入队，值被丢弃）。
   - 偏离：asio 对按操作取消给 `channel_cancelled`；本库统一用 `operation_aborted`，与 §6 的其他操作、
     `throw_if_cancelled` 和 `stop_token` 适配器一致。`channel_cancelled` 只来自 `channel::cancel()`。
   - 处理器按 §6.2 第 7 条在操作所在的 executor 上被 emit；它与别的线程上的完成之间用内部锁仲裁，
     先拿到锁的一方决定结果。
   - 因此在多线程 `thread_pool` 上，要从外部取消的协程须绑定 `strand`（与主规格 §6.2 第 7 条、压测场景 5 相同）：
     co_spawn 的取消桥把 emit dispatch 到协程的 executor，只有 strand 能保证它与协程恢复时清 slot 串行。
4. **析构：** 挂起的操作的回调被销毁而不调用（与 asio 相同；对 awaitable 而言就是 §11 的“丢弃执行线程”，
   帧栈经 executor 展开）；缓冲区元素析构。析构不能与该 channel 的任何其他调用、也不能与其挂起操作的
   取消 emit 并发。

## C5. 完成交付与 executor 亲和

1. 所有完成都 **post** 到回调的关联 executor（缺省是 channel 的 executor），从不在发起者或对端的栈里
   内联调用回调。所以：
   - 协程总是在自己的 executor（包括 strand）上恢复；
   - 发送方唤醒接收方不会把接收方的帧压进发送方的 pump，不会递归、不会让发送方被接收方的处理拖住；
   - 与 asio channel 唤醒对端的做法一致（asio 用 post 唤醒等待方）。
2. 立即可完成的操作同样 post 一次（对 `use_awaitable` 而言等价于 §8.3 的同步完成路径，成本相同）。
3. 发起时就完成的接收：值在锁内移出，随完成一起 post；投递前回调被销毁（executor 关闭）则值随之析构。

## C6. 线程安全

- `async_send` / `async_receive` / `try_*` / `close` / `cancel` / `is_open` / `size` / `dropped` 都线程安全，
  支持多生产者多消费者。内部一把 `std::mutex`，锁内只做队列操作，不调用用户代码、不投递。
- 偏离：asio channel 不是线程安全的，要求所有调用在同一 strand 上。本库的 channel 主要用于跨线程管道
  （相机线程 → 处理池），所以内置锁；锁外执行 post、drop 回调和元素析构。
- 不线程安全：析构、`set_drop_handler`（须在并发使用之前调用）。
- 等待节点在被完成和被取消之间可能竞争：节点带原子引用计数（队列一份、slot 处理器一份），
  节点里的 `queued` 标志只在锁内修改；slot 处理器先无锁读 `queued`，已出队就不再碰 channel。

## C7. 分配

| 情况 | 分配 |
| --- | --- |
| 构造 | 环形缓冲区一次（`capacity × sizeof(T)`，容量 0 不分配） |
| 立即完成的发送 / 接收 | 投递的 operation 一次，来自 `executor_function` 标签 |
| 进入等待的操作 | 上面一次，加等待节点一次（新标签 `mem_tag::channel_op`），有 slot 时再加处理器一次（`cancellation_signal` 标签，slot 里旧块够大就复用） |
| `try_send` / `try_receive` | 唤醒对端时一次投递；本身不分配 |

打开回收缓存后，在库线程上的稳态都来自缓存：**同一线程上的发送/接收循环每次操作全局 `operator new` 为 0**
（测试 `channel_alloc_test` 断言）。节点与投递的 operation 在别的线程释放时进入那个线程的缓存
（与主规格 D12 相同），跨线程管道的稳态不作 0 分配承诺。

## C8. 测试

- `tests/unit/channel_test.cpp`（manual_executor，确定性）：缓冲与 FIFO、容量 0 rendezvous、三种策略、
  drop 回调与计数、close 语义（剩余元素 → closed）、`cancel()`、slot 取消（三级、取消与完成先后两种顺序、
  完成后再 emit 无害）、`try_*`、不可默认构造的 `T`（`receive_type` 为 `optional<T>`）、只可移动的 `T`、
  原始回调 token 与 `use_future`、析构时挂起的操作被丢弃、strand 亲和。
- `tests/unit/channel_drop_zero_capacity_contract_test.cpp`：`drop_*` 配容量 0 → 退出码 86。
- `tests/unit/channel_alloc_test.cpp`：同线程稳态零分配（TSan 下只做行为检查，同 counting_new 约定）。
- `tests/stress/stress_test.cpp` 新增场景 7：thread_pool 上 4 生产者 × 4 消费者、block 策略、
  小容量与容量 0，核对总和与无丢失；外部线程 try_send + drop_oldest 与池上消费者并发；随机 cancel/close。
- 示例 `examples/video_stream.cpp` 作为 ctest 运行（断言各策略的不变量，不断言具体时间）。
- 头文件自包含检查自动覆盖新头（glob）。

## C9. 基准（只报告）

`channel_ping_pong`：同一 executor 上一个发送协程、一个接收协程，容量 1，每次往返的耗时与分配次数，
与 asio `experimental::channel<void(error_code, int)>` 对比。结果写进 `benchmarks/results/`，不进验收。

## C10. CI 修正（2026-11-13）

GitHub CI 的 TSan（GCC 14 / Clang 18 + libstdc++）在 `channel_test` 的 `use_future` 错误路径报竞争：池线程析构 `std::promise`
时释放异常对象的最后一个引用，而 libstdc++ 的异常引用计数在未插桩的 `libstdc++.so` 里，TSan 看不到它与主线程 `catch`
之间的同步，属于误报（只用 `std::promise` + `std::thread` 的最小程序同样会报，GCC 14 / Clang 18 各 20/20）。之前本地矩阵
用的是 zig c++（libc++ 随程序一起按 TSan 插桩编译），所以没复现。CI 同款工具链（TSan、C++17）修改前各跑 100 轮：单核
（`taskset -c 0`）`channel_test` 96%、`sync_wait_test` 98%～100%；双核 `channel_test` 1%～4%、`sync_wait_test` 0%；
4 核、8 核都是 0%。CI 上 ctest 同时跑 4 个测试抢 CPU，相当于少核，所以偶发。修改后单核、多核都是 0%。

- 测试：错误路径改为在独立线程池上做，`get()` 前先销毁（join）线程池。`use_future` 的 promise 在投递线程上析构是
  `std::promise` 的固有行为，库无法消除。
- 库：`co_spawn` 入口帧交付异常时会在投递线程上残留一份 `exception_ptr`（`sync_wait_test` 在 libstdc++ TSan 单核下
  98%～100% 报同类竞争）。新增 `detail::take_exception` / `detail::exception_handoff`（`impl/awaitable_frame.hpp`）：交出后
  源对象一定置空（先内联判空，没有异常时不多调库函数）。不能只写 `std::move`：LLVM 20 及以前的 libc++（含 NDK r25c、QNX SDP 7.1）`exception_ptr` 没有移动
  构造，`std::move` 退化成复制，被移走的 `deliver_op` 仍持有引用。`entry_point.hpp` 的 `deliver_op` / `deliver_void_op`、
  `sync_wait` 的回调、`parallel_group` 的子任务回调都改用它。
- 回归测试：`awaitable_test` 的 `spawnHandsTheExceptionOverByMove`（回调丢掉异常后异常对象立即析构；旧实现在 libstdc++
  和 LLVM 20 libc++ 下都失败）与 `exceptionHandoffEmptiesTheSource`。
- 验收补充：频道与跨线程改动须在 CI 同款工具链（GCC 14、Clang 18 + libstdc++）的 24 个配置上跑通，TSan 下相关测试单核与
  多核各 200 轮（C++17；C++14 / C++20 各 50 轮）。
