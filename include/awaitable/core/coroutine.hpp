#pragma once

// co2 协程核心的聚合头：句柄、awaitable 协议、帧与 ramp、宏 DSL。
#include "awaitable/core/coroutine_handle.hpp"
#include "awaitable/core/detail/awaitable.hpp"
#include "awaitable/core/detail/frame.hpp"
#include "awaitable/core/dsl.hpp"
