# 示例。打开测试时用与测试相同的告警集和 C++ 标准，并作为 ctest 运行（只断言不变量）。
add_executable(aw-example-video-stream video_stream.cpp)
target_link_libraries(aw-example-video-stream PRIVATE awaitable::awaitable)
if(COMMAND aw_configure_test)
    set(AW_TESTS_DIR ${PROJECT_SOURCE_DIR}/tests)
    aw_configure_test(aw-example-video-stream)
    add_test(NAME aw-example-video-stream COMMAND aw-example-video-stream)
    set_tests_properties(aw-example-video-stream PROPERTIES TIMEOUT 120)
endif()
