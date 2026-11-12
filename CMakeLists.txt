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
