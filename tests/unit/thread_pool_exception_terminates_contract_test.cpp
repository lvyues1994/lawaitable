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
