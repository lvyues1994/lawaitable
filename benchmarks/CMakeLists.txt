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
