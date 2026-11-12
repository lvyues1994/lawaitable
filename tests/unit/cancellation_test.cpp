#include <vector>

#include "awaitable/cancel/cancellation_signal.hpp"
#include "awaitable/cancel/cancellation_state.hpp"
#include "awaitable/cancel/cancellation_type.hpp"
#include "awaitable/exec/detail/thread_context.hpp"
#include "check.hpp"
#include "counting_new.hpp"

using aw::cancellation_type;

namespace {

struct recorder {
    std::vector<cancellation_type>* seen;
    void operator()(cancellation_type t) { seen->push_back(t); }
};

void typeOperators() {
    auto t = cancellation_type::terminal | cancellation_type::partial;
    AW_CHECK((t & cancellation_type::partial) == cancellation_type::partial);
    AW_CHECK(!(t & cancellation_type::total));
    AW_CHECK((~cancellation_type::none) == cancellation_type::all);
    t &= ~cancellation_type::terminal;
    AW_CHECK(t == cancellation_type::partial);
}

void slotBasics() {
    aw::cancellation_signal signal;
    aw::cancellation_slot slot = signal.slot();
    AW_CHECK(slot.is_connected() && !slot.has_handler());
    AW_CHECK(!aw::cancellation_slot{}.is_connected());
    signal.emit(cancellation_type::terminal); // 没有处理器：什么也不做
    std::vector<cancellation_type> seen;
    slot.emplace<recorder>(recorder{&seen});
    AW_CHECK(slot.has_handler() && signal.slot() == slot);
    signal.emit(cancellation_type::partial);
    AW_CHECK((seen == std::vector<cancellation_type>{cancellation_type::partial}));
    slot.clear();
    AW_CHECK(!slot.has_handler());
    signal.emit(cancellation_type::terminal);
    AW_CHECK(seen.size() == 1U);
}

void replacingAHandlerReusesItsMemory() {
    aw::detail::thread_context context;
    aw::cancellation_signal signal;
    std::vector<cancellation_type> seen;
    signal.slot().assign(recorder{&seen});
    aw_test::allocation_counter counter;
    for (int i = 0; i != 10; ++i) signal.slot().assign(recorder{&seen});
    signal.slot().clear();
    signal.slot().assign(recorder{&seen});
    if (aw_test::counting_new_enabled) AW_CHECK(counter.news() == 0);
}

void stateFiltersAndForwards() {
    aw::cancellation_signal parent;
    aw::cancellation_state state{parent.slot()}; // 默认只接收 terminal
    std::vector<cancellation_type> child;
    state.slot().assign(recorder{&child});
    parent.emit(cancellation_type::partial);
    AW_CHECK(state.cancelled() == cancellation_type::none && child.empty());
    parent.emit(cancellation_type::terminal | cancellation_type::partial);
    AW_CHECK(state.cancelled() == cancellation_type::terminal);
    AW_CHECK((child == std::vector<cancellation_type>{cancellation_type::terminal}));
    state.clear();
    AW_CHECK(state.cancelled() == cancellation_type::none);
}

void totalFilterAndSeparateOutFilter() {
    aw::cancellation_signal parent;
    aw::cancellation_state state{parent.slot(), aw::enable_total_cancellation{},
                                 aw::enable_terminal_cancellation{}};
    std::vector<cancellation_type> child;
    state.slot().assign(recorder{&child});
    parent.emit(cancellation_type::total);
    AW_CHECK(state.cancelled() == cancellation_type::total);
    AW_CHECK(child.empty()); // out 过滤器只放行 terminal
    aw::cancellation_state disabled{parent.slot(), aw::disable_cancellation{}};
    parent.emit(cancellation_type::all);
    AW_CHECK(disabled.cancelled() == cancellation_type::none);
}

void observerSeesFilteredCancellation() {
    aw::cancellation_signal parent;
    aw::cancellation_state state{parent.slot()};
    std::vector<cancellation_type> observed;
    state.set_observer(
        [](void* arg, cancellation_type t) {
            static_cast<std::vector<cancellation_type>*>(arg)->push_back(t);
        },
        &observed);
    parent.emit(cancellation_type::partial);
    parent.emit(cancellation_type::all);
    AW_CHECK((observed == std::vector<cancellation_type>{cancellation_type::terminal}));
}

void unconnectedState() {
    aw::cancellation_state state;
    AW_CHECK(!state.slot().is_connected());
    AW_CHECK(state.cancelled() == cancellation_type::none);
    aw::cancellation_state from_empty{aw::cancellation_slot{}};
    AW_CHECK(!from_empty.slot().is_connected());
}

} // namespace

int main() {
    typeOperators();
    slotBasics();
    replacingAHandlerReusesItsMemory();
    stateFiltersAndForwards();
    totalFilterAndSeparateOutFilter();
    observerSeesFilteredCancellation();
    unconnectedState();
    return 0;
}
