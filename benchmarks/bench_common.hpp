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
