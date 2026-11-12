# §14.2：帧栈原语的差分测试，固定 C++20（同一编译器的原生协程作对照）。
add_executable(aw-frame-stack-differential-test frame_stack_differential_test.cpp)
aw_configure_test(aw-frame-stack-differential-test)
set_target_properties(aw-frame-stack-differential-test PROPERTIES CXX_STANDARD 20)
add_test(NAME aw-frame-stack-differential-test COMMAND aw-frame-stack-differential-test)
