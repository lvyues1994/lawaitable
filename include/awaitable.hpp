#pragma once

// awaitable 总头文件（规格 §2.2）：包含全部公共头。不包含测试夹具 tests/legacy/include/co2/。

#include "awaitable/core/coroutine.hpp"
#include "awaitable/core/dsl.hpp"
#include "awaitable/core/generator.hpp"

#include "awaitable/exec/executor.hpp"
#include "awaitable/exec/manual_executor.hpp"
#include "awaitable/exec/scheduler.hpp"
#include "awaitable/exec/strand.hpp"
#include "awaitable/exec/thread_pool.hpp"

#include "awaitable/cancel/cancellation_signal.hpp"
#include "awaitable/cancel/cancellation_state.hpp"
#include "awaitable/cancel/cancellation_type.hpp"
#include "awaitable/cancel/stop_token.hpp"
#include "awaitable/cancel/stop_token_adapter.hpp"

#include "awaitable/as_tuple.hpp"
#include "awaitable/associated.hpp"
#include "awaitable/async_generator.hpp"
#include "awaitable/async_result.hpp"
#include "awaitable/awaitable.hpp"
#include "awaitable/co_spawn.hpp"
#include "awaitable/detached.hpp"
#include "awaitable/dispatch.hpp"
#include "awaitable/error.hpp"
#include "awaitable/multiple_exceptions.hpp"
#include "awaitable/operators.hpp"
#include "awaitable/optional.hpp"
#include "awaitable/post.hpp"
#include "awaitable/redirect_error.hpp"
#include "awaitable/sync_wait.hpp"
#include "awaitable/this_coro.hpp"
#include "awaitable/use_awaitable.hpp"
#include "awaitable/use_future.hpp"
#include "awaitable/variant.hpp"
