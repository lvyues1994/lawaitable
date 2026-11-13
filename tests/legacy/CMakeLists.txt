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

# 原样的 tests/CMakeLists.txt 只凭 cxx_std_20 是否在 CMAKE_CXX_COMPILE_FEATURES 里决定是否构建
# 与原生协程对照的 differential_tests.cpp。没有 <coroutine> 时（GCC 8 / NDK r25c 的 libc++），
# 在本目录作用域里把 cxx_std_20 从该列表去掉（只影响这个子目录，不改原样文件）。
if(NOT AW_HAVE_STD_COROUTINE)
    list(REMOVE_ITEM CMAKE_CXX_COMPILE_FEATURES cxx_std_20)
endif()
add_subdirectory(tests)

# GCC 14 -O2 对 core/detail/over_aligned.hpp 的 placeOverAligned 误报 -Warray-bounds
# （它假设 std::align 可能返回空指针；这里的 space 留足了填充，不会失败）。core 与 legacy 测试
# 都须逐字节保持原样，所以只在 GCC 下把这一条从错误降回警告，仅限触发它的这个测试。
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(co2-conformance-tests PRIVATE -Wno-error=array-bounds)
endif()

# 问题 03 的回归：分配次数断言（规格 §14.1）。
add_executable(co2-issue03-allocation-counts-test issue03_allocation_counts_test.cpp)
co2_configure_test_target(co2-issue03-allocation-counts-test)
add_test(NAME co2-issue03-allocation-counts-test COMMAND co2-issue03-allocation-counts-test)
