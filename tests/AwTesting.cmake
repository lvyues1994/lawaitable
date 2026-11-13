# 新层测试的公共设置：与 coro 相同的告警集；C++ 标准取 AW_TEST_CXX_STANDARD。
set(AW_TESTS_DIR ${CMAKE_CURRENT_LIST_DIR})

# 差分测试与 asio 对照测试用同一编译器的原生 C++20 协程作对照，需要标准库提供 <coroutine>。
# 编译器声称支持 C++20 不代表有 <coroutine>：GCC 8（QNX 7.1）没有，NDK r25c 的 libc++ 只有
# <experimental/coroutine>。没有时跳过这些测试（库本身不依赖 <coroutine>）。
include(CheckCXXSourceCompiles)
if(NOT DEFINED AW_HAVE_STD_COROUTINE)
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_CXX20_STANDARD_COMPILE_OPTION}")
    check_cxx_source_compiles([[
#include <coroutine>
int main() { std::coroutine_handle<> h{}; return h ? 1 : 0; }
]] AW_HAVE_STD_COROUTINE)
    unset(CMAKE_REQUIRED_FLAGS)
endif()
if(NOT AW_HAVE_STD_COROUTINE)
    message(STATUS "<coroutine> unavailable: differential and asio parity tests skipped")
endif()

# GCC 8 + libc++（QNX 7.1）：libc++ 头文件里的默认实参 `= 0`（如 std::error_code 的构造函数）
# 在实例化点触发 -Wzero-as-null-pointer-constant；legacy 的 allocation_tests.cpp 用了 GCC 8
# 还不认识的 -Wmismatched-new-delete，触发 -Wpragmas。这两条在 GCC 8 下只保留为警告。
# 用目录级选项，使原样搬入的 legacy 测试也受影响。
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    add_compile_options(-Wno-error=zero-as-null-pointer-constant -Wno-error=pragmas)
endif()

function(aw_configure_test target)
    target_link_libraries(${target} PRIVATE awaitable::awaitable)
    target_include_directories(${target} PRIVATE ${AW_TESTS_DIR}/support)
    set_target_properties(${target} PROPERTIES CXX_STANDARD ${AW_TEST_CXX_STANDARD}
                                               CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic -Werror -Wconversion -Wsign-conversion -Wnon-virtual-dtor
        -Wold-style-cast -Wzero-as-null-pointer-constant -Werror=return-type
        -Werror=uninitialized -Werror=suggest-override -Werror=vla
        # GCC 在 -O2/-O3 深度内联后对 optional / variant 存储和协程帧局部量的 -Wmaybe-uninitialized
        # 是已知的误报来源（Release 下 operators_test、async_generator_test 触发）；GCC 下保留警告、
        # 不作为错误。-Werror=uninitialized（确定未初始化）不受影响。
        $<$<CXX_COMPILER_ID:GNU>:-Wno-error=maybe-uninitialized>)
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
