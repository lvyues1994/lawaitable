#include <exception>
#include <stdexcept>
#include <string>
#include <system_error>

#include "awaitable/error.hpp"
#include "awaitable/multiple_exceptions.hpp"
#include "check.hpp"

int main() {
    std::error_code const ec = aw::error::operation_aborted;
    AW_CHECK(ec.value() == 1);
    AW_CHECK(std::string{ec.category().name()} == "aw");
    AW_CHECK(ec == aw::error::make_error_code(aw::error::operation_aborted));
    AW_CHECK(!ec.message().empty());

    auto const first = std::make_exception_ptr(std::runtime_error{"first"});
    aw::multiple_exceptions const all{first};
    AW_CHECK(all.first_exception() == first);
    AW_CHECK(std::string{all.what()} == "multiple exceptions");
    return 0;
}
