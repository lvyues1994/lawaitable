#pragma once

#include <string>
#include <system_error>
#include <type_traits>

namespace aw {
namespace error {

enum errc : int { operation_aborted = 1 };

namespace detail {
class category_impl final : public std::error_category {
  public:
    char const* name() const noexcept override { return "aw"; }
    std::string message(int value) const override {
        return value == operation_aborted ? "operation aborted" : "unknown aw error";
    }
};
} // namespace detail

inline std::error_category const& category() noexcept {
    static detail::category_impl const instance;
    return instance;
}

inline std::error_code make_error_code(errc value) noexcept {
    return std::error_code{static_cast<int>(value), category()};
}

} // namespace error
} // namespace aw

namespace std {
template <> struct is_error_code_enum<aw::error::errc> : true_type {};
} // namespace std
