#include <memory>
#include <string>
#include <utility>

#include "awaitable/optional.hpp"
#include "awaitable/variant.hpp"
#include "check.hpp"

namespace {

void optionalBasics() {
    aw::optional<int> empty;
    AW_CHECK(!empty.has_value());
    AW_CHECK(!empty);
    aw::optional<int> none{aw::nullopt};
    AW_CHECK(!none);
    aw::optional<std::string> text{std::string{"abc"}};
    AW_CHECK(text.has_value() && *text == "abc" && text->size() == 3U);
    AW_CHECK(text.value() == "abc");
    text.reset();
    AW_CHECK(!text);
    text.emplace(3U, 'x');
    AW_CHECK(*text == "xxx");
}

void optionalMoveOnly() {
    aw::optional<std::unique_ptr<int>> a{std::unique_ptr<int>{new int{7}}};
    aw::optional<std::unique_ptr<int>> b{std::move(a)};
    AW_CHECK(b && **b == 7);
    aw::optional<std::unique_ptr<int>> c;
    c = std::move(b);
    AW_CHECK(c && **c == 7);
}

void variantBasics() {
    aw::variant<aw::monostate, int, std::string> v;
    AW_CHECK(v.index() == 0U);
    aw::variant<aw::monostate, int, std::string> w{aw::in_place_index<2>(), "hi"};
    AW_CHECK(w.index() == 2U);
    AW_CHECK(aw::get<2>(w) == "hi");
    AW_CHECK(aw::get_if<1>(&w) == nullptr);
    AW_CHECK(aw::get_if<2>(&w) != nullptr);
    aw::variant<aw::monostate, int, std::string> moved{std::move(w)};
    AW_CHECK(moved.index() == 2U && aw::get<2>(moved) == "hi");
    aw::variant<int, std::unique_ptr<int>> owner{aw::in_place_index<1>(), new int{5}};
    AW_CHECK(*aw::get<1>(owner) == 5);
}

} // namespace

int main() {
    optionalBasics();
    optionalMoveOnly();
    variantBasics();
    return 0;
}
