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
