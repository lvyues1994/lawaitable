#pragma once

#include <cstddef>
#include <exception>
#include <memory>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

#include "awaitable/core/contract.hpp"
#include "awaitable/core/coroutine_handle.hpp"
#include "awaitable/core/detail/await_slot.hpp"
#include "awaitable/core/detail/awaitable.hpp"
#include "awaitable/core/detail/over_aligned.hpp"
#include "awaitable/core/detail/uninitialized.hpp"

// 帧的恢复/收尾路径把状态机（Body::operator()）内联进来后，MSVC 会把 co_return 之后的
// 收尾判成不可达并报到本文件的行号上；C4702 以函数开括号处的状态为准，见 config.hpp。
CO2_DETAIL_MSVC_WARNING_PUSH_DISABLE(4702)

// co2 核心：协程帧与 ramp。执行 [dcl.fct.def.coroutine] 规定的协程体变换：
//
//   promise-type promise promise-constructor-arguments;
//   try { co_await promise.initial_suspend(); function-body }
//   catch (...) { if (!initial-await-resume-called) throw;
//   promise.unhandled_exception(); } final-suspend: co_await promise.final_suspend();
//
// 帧是一次分配的标准布局对象：{ header, promise } 前缀、参数副本（Params）、局部与
// 程序计数器（Body）、awaiter 槽、allocator。构造顺序 参数副本 → promise → 局部，
// 销毁顺序 awaiter → 局部 → promise → 参数副本，与标准一致。核心不含任何同步。
//
// 变换里的 try 块分在两处：initial-await-resume-called 为假的那一段（promise 构造到
// initial awaiter 的 await_suspend）在 ramp（runRamp）里，异常销毁帧并抛给调用方；为真
// 的那一段（initial awaiter 的 await_resume 起）在 step 里，异常进入 unhandled_exception。

namespace co2 {
namespace detail {

// 挂起点编号（程序计数器）：0 是协程体起点，其余取自源码行号。它是没有枚举项的作用域
// 枚举而不是整数：CO2_AWAIT 展开出的 case 标签必须能隐式转换成所在 switch 的条件类型，
// 而作用域枚举不能隐式转换成任何别的类型——挂起点一旦落进协程体里用户自己的 switch，
// 就是编译错误，而不是等到恢复时才发现标签绑错了 switch。
enum class SuspendPoint : unsigned {};

// 帧内 awaiter 槽的内联容量（字节）。默认是 CO2_AWAIT_STORAGE_SIZE；一个 promise 的全部
// awaiter 都是已知的小类型时（例如生成器：体内禁止 co_await）可以特化得更小，省掉每个帧
// 里用不上的缓冲。超出容量的 awaiter 照常回落到堆，所以特化只影响大小，不影响正确性。
template <class Promise>
struct AwaitStorageSize
    : std::integral_constant<std::size_t, CO2_AWAIT_STORAGE_SIZE> {};

// 帧中与 allocator 无关的部分。Body 的 operator() 以它为上下文，因此每个协程体只
// 实例化一份 step 逻辑，不随 allocator 变化。
template <class Promise, class Params, class Body> struct FrameCore {
    using InitialAwaiter =
        AwaiterOf<decltype(std::declval<Promise&>().initial_suspend())>;
    using FinalAwaiter = AwaiterOf<decltype(std::declval<Promise&>().final_suspend())>;

    // ---- 布局：prefix 必须是第一个成员 ----
    FramePrefix<Promise> prefix;
    alignas(Params) unsigned char paramsStorage[sizeof(Params)];
    alignas(Body) unsigned char bodyStorage[sizeof(Body)];
    // 两个小成员排在 awaiter 槽之前：槽按 max_align_t 对齐，它前面常有一段填充可以容纳
    // 它们；排在槽后面则每个帧都要为这 5 个字节多占一整个对齐单位。
    SuspendPoint suspendPoint;
    // 局部（Body）在离开协程体作用域时就销毁——co_return、掉出末尾或异常逃出——
    // 早于 final_suspend，与标准一致；destroy() 只销毁仍然存活的那一份。
    bool bodyAlive;
    AwaitSlot<AwaitStorageSize<Promise>::value> slot;

    // ---- Body 侧的访问 ----

    Promise& promise() noexcept { return *reinterpret_cast<Promise*>(prefix.promise); }
    Params& params() noexcept { return *reinterpret_cast<Params*>(paramsStorage); }
    Body& body() noexcept { return *reinterpret_cast<Body*>(bodyStorage); }

    coroutine_handle<Promise> handle() noexcept {
        return coroutine_handle<Promise>::from_address(&prefix.header);
    }

    template <class Awaitable>
    auto transform(Awaitable&& awaitable)
        -> decltype(transformAwaitable(std::declval<Promise&>(),
                                       std::forward<Awaitable>(awaitable))) {
        return transformAwaitable(promise(), std::forward<Awaitable>(awaitable));
    }

    template <class Awaiter, class Value> Awaiter& emplaceAwaiter(Value&& value) {
        return slot.template emplace<Awaiter>(std::forward<Value>(value));
    }

    template <class Awaiter> Awaiter& awaiter() noexcept {
        return slot.template get<Awaiter>();
    }

    // 类型擦除的复位：异常路径不知道槽里是什么。
    void resetAwaiter() noexcept { slot.reset(); }

    // await 正常结束：awaiter 的类型已知，直接析构（平凡析构的内联 awaiter 是空操作）。
    template <class Awaiter> void resetAwaiterAs() noexcept {
        slot.template resetAs<Awaiter>();
    }

    // 一经调用 await_suspend，协程即视为挂起；另一线程可以在 await_suspend 返回前
    // 就 resume()。因此生成的代码在调用 await_suspend 之前就清掉 running。
    void markSuspended() noexcept { prefix.header.running = false; }
    void markRunning() noexcept { prefix.header.running = true; }

    // 初始挂起点之后的第一条语句：initial awaiter 的 await_resume()。标准在求值它之前
    // 就把 initial-await-resume-called 置真（[dcl.fct.def.coroutine]/5），所以它抛出的
    // 异常与协程体里的异常一样进入 unhandled_exception，帧仍由拥有者销毁。"初始挂起点
    // 之前"的异常（get_return_object、initial_suspend、await_ready、await_suspend）全部
    // 发生在 ramp 里，由 runRamp 销毁帧并抛给调用方。
    void finishInitialSuspend() {
        awaiter<InitialAwaiter>().await_resume();
        resetAwaiterAs<InitialAwaiter>();
    }

    // 掉出协程末尾：有 return_void 就调用它；没有时标准是未定义行为，这里报告契约违规。
    void returnVoidAtEnd() { returnVoidAtEnd(0); }

    // 离开协程体作用域：销毁局部。Body 正在执行自己的成员函数时也可以调用，只要之后
    // 不再访问它的成员。
    void destroyBody() noexcept {
        if (not bodyAlive) return;
        bodyAlive = false;
        body().~Body();
    }

    // co_await promise.final_suspend()。挂起时 done 置真、帧保留到 owner destroy()；
    // 不挂起（await_ready 为真，或 await_suspend 返回 false）时与任何 await 表达式一样
    // 先求值 await_resume()，然后控制流掉出协程末尾，协程状态被销毁，返回后不得再访问帧。
    FrameHeader* enterFinalSuspend() noexcept {
        destroyBody();
        auto& awaiter =
            slot.template emplace<FinalAwaiter>(getAwaiter(promise().final_suspend()));
        if (awaiter.await_ready()) {
            awaiter.await_resume();
            destroySelf();
            return nullptr;
        }
        prefix.header.done = true;
        markSuspended();
        auto const outcome = suspendWith(awaiter, handle());
        if (not outcome.suspended) {
            prefix.header.done = false;
            markRunning();
            awaiter.await_resume();
            destroySelf();
            return nullptr;
        }
        return outcome.next;
    }

  private:
    template <class P = Promise>
    auto returnVoidAtEnd(int) -> decltype(std::declval<P&>().return_void()) {
        return promise().return_void();
    }

    void returnVoidAtEnd(long) {
        CO2_CONTRACT_FAIL("co2 coroutine flowed off the end without return_void()");
    }

    void destroySelf() noexcept { prefix.header.destroy(&prefix.header); }
};

// 宏生成的 Body 的空基类：让成员初始化列表在没有参数时也有合法的首项。
struct BodyBase {};

// ---- promise 构造：能用参数副本构造就传入，否则默认构造 ----

// 宏生成的参数列表以它收尾，这样 0 个参数时也没有悬空逗号。
struct NoMoreParams {};

template <class Promise, class... Params>
void constructPromiseAt(void* const at, std::true_type, Params&... params) {
    ::new (at) Promise(params...);
}

template <class Promise, class... Params>
void constructPromiseAt(void* const at, std::false_type, Params&...) {
    ::new (at) Promise();
}

template <class Promise, class Tuple, std::size_t... Index>
void constructPromiseFromTuple(void* const at, Tuple& params,
                               std::index_sequence<Index...>) {
    constructPromiseAt<Promise>(
        at, std::is_constructible<Promise, decltype(std::get<Index>(params))...>{},
        std::get<Index>(params)...);
}

// 末尾实参是 NoMoreParams；其余是参数副本的左值。
template <class Promise, class... Args>
void constructPromise(void* const at, Args&&... args) {
    auto params = std::forward_as_tuple(args...);
    constructPromiseFromTuple<Promise>(
        at, params, std::make_index_sequence<sizeof...(Args) - 1U>{});
}

// 宏生成的参数副本类型提供 _co2_construct_promise(void*)（局部类不能有成员模板，
// promise 类型在生成时已知）；手写的（测试）没有时默认构造 promise。
template <class Promise, class Params>
auto constructPromiseFor(Params& params, void* const at, int)
    -> decltype(params._co2_construct_promise(at)) {
    return params._co2_construct_promise(at);
}

template <class Promise, class Params>
void constructPromiseFor(Params&, void* const at, long) {
    ::new (at) Promise();
}

// ---- 分配：promise 的 operator new / operator delete /
// get_return_object_on_allocation_failure ----

template <class Promise> struct HasPromiseOperatorNew {
    template <class P>
    static auto probe(int)
        -> decltype(P::operator new (std::size_t{}), std::true_type{});
    template <class> static std::false_type probe(long);
    static constexpr bool value = decltype(probe<Promise>(0))::value;
};

// 任何形式的 promise 类内 operator new（含 operator new(size_t, 参数...)）。借助名字查找
// 二义性探测：派生类同时继承 promise 与一个自带 operator new 的探针基类，promise 只要
// 声明了 operator new，派生类里的查找就二义、表达式无效。
struct OperatorNewProbe {
    static void* operator new(std::size_t, OperatorNewProbe*) noexcept;
};
template <class Promise> struct OperatorNewProbeDerived : Promise, OperatorNewProbe {};

template <class Promise, bool Inheritable> struct HasAnyPromiseOperatorNewImpl {
    template <class D>
    static auto probe(int) -> decltype(D::operator new (std::size_t{},
                                                        static_cast<OperatorNewProbe*>(nullptr)),
                                       std::false_type{});
    template <class> static std::true_type probe(long);
    static constexpr bool value =
        decltype(probe<OperatorNewProbeDerived<Promise>>(0))::value;
};
template <class Promise> struct HasAnyPromiseOperatorNewImpl<Promise, false> {
    static constexpr bool value = false;
};
template <class Promise>
struct HasAnyPromiseOperatorNew
    : std::integral_constant<
          bool, HasAnyPromiseOperatorNewImpl<
                    Promise, std::is_class<Promise>::value &&
                                 not std::is_final<Promise>::value>::value> {};

template <class Promise>
auto promiseAllocate(std::size_t const size, int)
    -> decltype(Promise::operator new(size)) {
    return Promise::operator new(size);
}

template <class Promise>
auto promiseDeallocate(void* const pointer, std::size_t const size, Rank<2>)
    -> decltype(Promise::operator delete(pointer, size)) {
    return Promise::operator delete(pointer, size);
}

template <class Promise>
auto promiseDeallocate(void* const pointer, std::size_t, Rank<1>)
    -> decltype(Promise::operator delete(pointer)) {
    return Promise::operator delete(pointer);
}

template <class Promise>
void promiseDeallocate(void* const pointer, std::size_t, Rank<0>) noexcept {
    ::operator delete(pointer); // promise 只提供 operator new 时回落到全局 delete
}

template <class Promise> struct HasAllocationFailureObject {
    template <class P>
    static auto probe(int)
        -> decltype(P::get_return_object_on_allocation_failure(), std::true_type{});
    template <class> static std::false_type probe(long);
    static constexpr bool value = decltype(probe<Promise>(0))::value;
};

// ---- 标准判为病式、co2 曾默默接受的 promise（设计稿 D9） ----

// [dcl.fct.def.coroutine]/6 按名字判断：在 promise 类型的作用域里查找 return_void 与
// return_value，两者都找到任何声明（哪怕是被 SFINAE 禁用的模板）即为病式。探测同样借助
// 名字查找二义性：派生类同时继承 promise 与声明了该名字的探针基类。
struct ReturnProbeTag {};
struct ReturnValueProbe {
    void return_value(ReturnProbeTag);
};
struct ReturnVoidProbe {
    void return_void(ReturnProbeTag);
};
template <class Promise, class Probe> struct ReturnProbeDerived : Promise, Probe {};

template <class Promise, class Probe, bool Inheritable> struct DeclaresReturnNameImpl {
    template <class D> static auto probe(int, ReturnValueProbe*)
        -> decltype(std::declval<D&>().return_value(ReturnProbeTag{}), std::false_type{});
    template <class D> static auto probe(int, ReturnVoidProbe*)
        -> decltype(std::declval<D&>().return_void(ReturnProbeTag{}), std::false_type{});
    template <class> static std::true_type probe(long, ...);
    static constexpr bool value = decltype(probe<ReturnProbeDerived<Promise, Probe>>(
        0, static_cast<Probe*>(nullptr)))::value;
};
template <class Promise, class Probe> struct DeclaresReturnNameImpl<Promise, Probe, false> {
    static constexpr bool value = false;
};
template <class Promise, class Probe>
struct DeclaresReturnName
    : std::integral_constant<
          bool, DeclaresReturnNameImpl<Promise, Probe,
                                       std::is_class<Promise>::value &&
                                           not std::is_final<Promise>::value>::value> {};

// co_await promise.final_suspend() 整个表达式不得潜在抛出（[dcl.fct.def.coroutine]/15）。
template <class Promise> struct FinalSuspendIsNoexcept {
    using Expression = decltype(getAwaiter(std::declval<Promise&>().final_suspend()));
    using Stored = AwaiterOf<decltype(std::declval<Promise&>().final_suspend())>;
    static constexpr bool value =
        noexcept(getAwaiter(std::declval<Promise&>().final_suspend())) &&
        std::is_nothrow_constructible<Stored, Expression>::value &&
        noexcept(std::declval<Expression&>().await_ready()) &&
        noexcept(std::declval<Expression&>().await_suspend(
            std::declval<coroutine_handle<Promise>>())) &&
        noexcept(std::declval<Expression&>().await_resume());
};

template <class Promise> void checkPromiseRequirements() noexcept {
    static_assert(not(HasAnyPromiseOperatorNew<Promise>::value &&
                      not HasPromiseOperatorNew<Promise>::value),
                  "co2: the promise declares operator new but not operator new(std::size_t); "
                  "co2 cannot pass the coroutine parameters to operator new (design D9), "
                  "so provide operator new(std::size_t) or use an allocator parameter");
    static_assert(not(DeclaresReturnName<Promise, ReturnVoidProbe>::value &&
                      DeclaresReturnName<Promise, ReturnValueProbe>::value),
                  "co2: a promise type must not declare both return_void and return_value "
                  "([dcl.fct.def.coroutine]/6)");
    static_assert(FinalSuspendIsNoexcept<Promise>::value,
                  "co2: co_await promise.final_suspend() must not be potentially-throwing "
                  "([dcl.fct.def.coroutine]/15): mark final_suspend and the final awaiter's "
                  "await_ready / await_suspend / await_resume noexcept");
}

// 帧的原始内存：来源是 promise 的 operator new（若有）或 allocator。
//
// 帧的对齐不超过 alignof(std::max_align_t) 时按帧类型分配一个对象。超对齐的帧（局部或
// 参数里有 alignas 更大的成员）不能指望这两个来源给出足够的对齐，于是按字节多要一些并
// 在块内手工对齐（over_aligned.hpp），释放时归还整块。只在成员函数里实例化——那时
// Frame 已经是完整类型。
template <class Frame, class Promise, class FrameAllocator> struct FrameMemory {
    using Traits = std::allocator_traits<FrameAllocator>;
    using ByteAllocator = typename Traits::template rebind_alloc<unsigned char>;
    using ByteTraits = std::allocator_traits<ByteAllocator>;
    using FromPromise =
        std::integral_constant<bool, HasPromiseOperatorNew<Promise>::value>;
    using OverAligned = IsOverAligned<Frame>;

    // 返回未构造的帧存储。promise 的 operator new 返回空时返回空。
    static Frame* allocate(FrameAllocator& allocator) {
        return place(allocateBlock(allocator, FromPromise{}), OverAligned{});
    }

    static void deallocate(Frame* const frame, FrameAllocator& allocator) noexcept {
        deallocateBlock(blockOf(frame, OverAligned{}), allocator, FromPromise{});
    }

  private:
    static constexpr std::size_t blockSize() noexcept {
        return OverAligned::value
                   ? overAlignedBlockSize(sizeof(Frame), alignof(Frame))
                   : sizeof(Frame);
    }

    static void* allocateBlock(FrameAllocator&, std::true_type) {
        return promiseAllocate<Promise>(blockSize(), 0);
    }

    static void* allocateBlock(FrameAllocator& allocator, std::false_type) {
        return allocateFrom(allocator, OverAligned{});
    }

    static void* allocateFrom(FrameAllocator& allocator, std::false_type) {
        return Traits::allocate(allocator, 1);
    }

    static void* allocateFrom(FrameAllocator& allocator, std::true_type) {
        auto bytes = ByteAllocator{allocator};
        return ByteTraits::allocate(bytes, blockSize());
    }

    static Frame* place(void* const block, std::false_type) noexcept {
        return static_cast<Frame*>(block);
    }

    static Frame* place(void* const block, std::true_type) noexcept {
        if (block == nullptr) return nullptr;
        return static_cast<Frame*>(
            placeOverAligned(block, sizeof(Frame), alignof(Frame)));
    }

    static void* blockOf(Frame* const frame, std::false_type) noexcept { return frame; }

    static void* blockOf(Frame* const frame, std::true_type) noexcept {
        return overAlignedBlockOf(frame);
    }

    static void deallocateBlock(void* const block, FrameAllocator&,
                                std::true_type) noexcept {
        promiseDeallocate<Promise>(block, blockSize(), Rank<2>{});
    }

    static void deallocateBlock(void* const block, FrameAllocator& allocator,
                                std::false_type) noexcept {
        deallocateTo(block, allocator, OverAligned{});
    }

    static void deallocateTo(void* const block, FrameAllocator& allocator,
                             std::false_type) noexcept {
        Traits::deallocate(allocator, static_cast<Frame*>(block), 1);
    }

    static void deallocateTo(void* const block, FrameAllocator& allocator,
                             std::true_type) noexcept {
        auto bytes = ByteAllocator{allocator};
        ByteTraits::deallocate(bytes, static_cast<unsigned char*>(block), blockSize());
    }
};

// ---- 完整的帧 ----

// 帧的数据成员：核心 + allocator 的副本（释放帧时要用）。allocator 是空类型且可默认构造
// 时（std::allocator、无状态的自定义 allocator）不保存副本——这类 allocator 的实例可以
// 互换，释放时重新构造一个即可，帧因此不必为一个空对象多占一个对齐单位。
template <class Core, class FrameAllocator, bool StatelessAllocator>
struct FrameLayout {
    Core core;
    alignas(FrameAllocator) unsigned char allocatorStorage[sizeof(FrameAllocator)];

    void storeAllocator(FrameAllocator const& allocator) noexcept {
        ::new (static_cast<void*>(allocatorStorage)) FrameAllocator{allocator};
    }

    // 取走副本并结束它在帧里的生命周期。
    FrameAllocator takeAllocator() noexcept {
        auto& stored = *reinterpret_cast<FrameAllocator*>(allocatorStorage);
        auto taken = FrameAllocator{std::move(stored)};
        stored.~FrameAllocator();
        return taken;
    }
};

template <class Core, class FrameAllocator>
struct FrameLayout<Core, FrameAllocator, true> {
    Core core;

    void storeAllocator(FrameAllocator const&) noexcept {}
    FrameAllocator takeAllocator() noexcept { return FrameAllocator{}; }
};

template <class Allocator>
using IsStatelessAllocator =
    std::integral_constant<bool, std::is_empty<Allocator>::value &&
                                     std::is_default_constructible<Allocator>::value>;

// 帧本身没有自己的数据成员，全部在 FrameLayout 里：标准布局类与它唯一带数据的基类共用
// 地址，帧头 → 前缀 → 核心 → 帧 这条"首成员"链不变。
template <class Promise, class Params, class Body, class Allocator>
struct CoroutineFrame
    : FrameLayout<FrameCore<Promise, Params, Body>,
                  typename std::allocator_traits<Allocator>::template rebind_alloc<
                      CoroutineFrame<Promise, Params, Body, Allocator>>,
                  IsStatelessAllocator<Allocator>::value> {
    using Core = FrameCore<Promise, Params, Body>;
    using FrameAllocator = typename std::allocator_traits<
        Allocator>::template rebind_alloc<CoroutineFrame>;
    using Memory = FrameMemory<CoroutineFrame, Promise, FrameAllocator>;
    using HasFailureObject =
        std::integral_constant<bool, HasAllocationFailureObject<Promise>::value>;
    // allocator 以 bad_alloc 报告失败；promise 的 operator new 以返回空报告。
    using CatchesBadAlloc = std::integral_constant<
        bool, HasAllocationFailureObject<Promise>::value &&
                  not HasPromiseOperatorNew<Promise>::value>;

    // 分配并初始化帧：参数副本 → promise（能用参数副本构造就传入）→ 局部（Body）。
    // `makeParams(void*)` 在给定地址上构造参数副本：帧先分配，参数再直接移动进去。
    // 返回空表示分配失败且 promise 提供了 get_return_object_on_allocation_failure。
    template <class MakeParams>
    static CoroutineFrame* create(MakeParams&& makeParams, Allocator const& allocator) {
        static_assert(
            std::is_standard_layout<CoroutineFrame>::value,
            "the coroutine frame must stay standard-layout so that the header, "
            "the prefix and the frame share one address");
        auto frameAllocator = FrameAllocator{allocator};
        auto* const storage = allocate(frameAllocator);
        if (storage == nullptr) return nullptr;

        // 默认初始化：只构造 AwaitSlot，字节数组保持未初始化。
        auto* const frame = ::new (static_cast<void*>(storage)) CoroutineFrame;
        frame->storeAllocator(frameAllocator);
        frame->core.prefix.header = FrameHeader{&destroy, &step, false, false};
        frame->core.suspendPoint = SuspendPoint{};
        frame->core.bodyAlive = false;

        try {
            makeParams(static_cast<void*>(frame->core.paramsStorage));
        } catch (...) {
            release(frame);
            throw;
        }
        try {
            constructPromiseFor<Promise>(frame->core.params(),
                                         frame->core.prefix.promise, 0);
        } catch (...) {
            frame->core.params().~Params();
            release(frame);
            throw;
        }
        try {
            ::new (static_cast<void*>(frame->core.bodyStorage))
                Body(frame->core.params());
        } catch (...) {
            frame->core.promise().~Promise();
            frame->core.params().~Params();
            release(frame);
            throw;
        }
        frame->core.bodyAlive = true;
        return frame;
    }

    // 运行到下一个挂起点。catch 块就是标准协程体变换里的那个 catch；step 只在初始挂起点
    // 之后运行（第一条语句是 initial awaiter 的 await_resume），此时
    // initial-await-resume-called 恒为真，所以这里没有"重新抛出"的分支。
    static FrameHeader* step(FrameHeader* const header) {
        auto* const frame = fromHeader(header);
        auto& core = frame->core;
        core.markRunning();
        try {
            return core.body()(core);
        } catch (...) {
            // 异常离开协程体：等待中的 awaiter 临时对象与局部先于 catch 处理销毁。
            core.resetAwaiter();
            core.destroyBody();
            // unhandled_exception 再抛出时，协程停在 final suspend point。
            core.prefix.header.done = true;
            core.markSuspended();
            core.promise().unhandled_exception();
            core.prefix.header.done = false;
            core.markRunning();
            return core.enterFinalSuspend();
        }
    }

    // 销毁协程状态：等待中的 awaiter、仍存活的局部（Body）、promise、参数副本，然后
    // 释放帧。
    static void destroy(FrameHeader* const header) noexcept {
        auto* const frame = fromHeader(header);
        frame->core.slot.reset();
        frame->core.destroyBody();
        frame->core.promise().~Promise();
        frame->core.params().~Params();
        release(frame);
    }

  private:
    static CoroutineFrame* fromHeader(FrameHeader* const header) noexcept {
        // header 是 core.prefix 的首成员，core.prefix 是 core 的首成员，core 是
        // FrameLayout 的首成员，FrameLayout 是帧唯一的基类；标准布局保证它们地址相同。
        return reinterpret_cast<CoroutineFrame*>(header);
    }

    static CoroutineFrame* allocate(FrameAllocator& frameAllocator) {
        return allocateOrNull(frameAllocator, CatchesBadAlloc{});
    }

    static CoroutineFrame* allocateOrNull(FrameAllocator& frameAllocator,
                                          std::false_type) {
        return Memory::allocate(frameAllocator);
    }

    static CoroutineFrame* allocateOrNull(FrameAllocator& frameAllocator,
                                          std::true_type) {
        try {
            return Memory::allocate(frameAllocator);
        } catch (std::bad_alloc const&) {
            return nullptr;
        }
    }

    // 结束帧对象自身的生命周期并释放内存。
    static void release(CoroutineFrame* const frame) noexcept {
        auto allocatorCopy = frame->takeAllocator();
        frame->~CoroutineFrame();
        Memory::deallocate(frame, allocatorCopy);
    }
};

using DefaultFrameAllocator = std::allocator<unsigned char>;

// ---- ramp ----
//
// 分配帧 → 构造 promise → get_return_object → co_await initial_suspend。不挂起的
// 协程在这里直接运行；初始挂起点之前的异常销毁帧并抛给调用方。

// 帧已分配：get_return_object → co_await initial_suspend。
template <class Return, class Promise, class Params, class Body, class Allocator>
Return runRamp(CoroutineFrame<Promise, Params, Body, Allocator>& frame) {
    using Core = FrameCore<Promise, Params, Body>;
    auto& core = frame.core;
    checkPromiseRequirements<Promise>();

    using Returned = typename std::decay<decltype(core.promise().get_return_object())>::type;

    struct DestroyOnException {
        FrameHeader* header;
        ~DestroyOnException() {
            if (header != nullptr) header->destroy(header);
        }
    } guard{&core.prefix.header};

    // get_return_object 的结果放在原始存储里，只在"初始挂起点之前没有异常"时析构。
    // 初始挂起点之前的异常由 guard 销毁协程状态；返回对象若拥有帧（常见写法），它的析构
    // 会再销毁一次帧（双重销毁）。与 clang 的原生实现一致：此时返回对象不被析构。
    struct ReturnedObject {
        Uninitialized<Returned> storage;
        bool settled = false; // 初始挂起点之前的阶段已无异常地结束
        ~ReturnedObject() {
            if (settled) storage.destroy();
        }
    } returned;
    returned.storage.construct(core.promise().get_return_object());

    auto& initial = core.slot.template emplace<typename Core::InitialAwaiter>(
        getAwaiter(core.promise().initial_suspend()));
    if (initial.await_ready()) {
        guard.header = nullptr;
        returned.settled = true;
        core.handle()
            .resume(); // 不挂起：直接进入 case 0（await_resume 之后是用户代码）
        return Return(std::move(returned.storage.get()));
    }
    auto const outcome = suspendWith(initial, core.handle());
    guard.header = nullptr;
    returned.settled = true;
    if (not outcome.suspended) {
        core.handle().resume();
    } else if (outcome.next != nullptr) {
        // 初始挂起点上的对称转移：驱动目标，本协程保持挂起。
        coroutine_handle<>::from_address(outcome.next).resume();
    }
    return Return(std::move(returned.storage.get()));
}

// 空指针检查只在 promise 提供 get_return_object_on_allocation_failure 时存在：没有它时
// Frame::create 以 bad_alloc 报告失败、从不返回空，MSVC 会把这个分支判成不可达（C4702）。
template <class Return, class Promise, class Params, class Body, class Allocator>
Return startCoroutineWith(CoroutineFrame<Promise, Params, Body, Allocator>* const frame,
                          std::true_type) {
    if (frame == nullptr) return Promise::get_return_object_on_allocation_failure();
    return runRamp<Return>(*frame);
}

template <class Return, class Promise, class Params, class Body, class Allocator>
Return startCoroutineWith(CoroutineFrame<Promise, Params, Body, Allocator>* const frame,
                          std::false_type) {
    return runRamp<Return>(*frame);
}

// `makeParams(void*)` 在帧内给定的地址上构造 Params（宏生成：把函数参数转发进去）。
template <class Return, class Promise, class Body, class Params, class MakeParams,
          class Allocator>
Return startCoroutine(MakeParams&& makeParams, Allocator const& allocator) {
    using Frame = CoroutineFrame<Promise, Params, Body, Allocator>;
    return startCoroutineWith<Return>(
        Frame::create(std::forward<MakeParams>(makeParams), allocator),
        typename Frame::HasFailureObject{});
}

} // namespace detail
} // namespace co2

CO2_DETAIL_MSVC_WARNING_POP
