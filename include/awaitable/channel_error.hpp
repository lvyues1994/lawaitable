#pragma once

#include <string>
#include <system_error>
#include <type_traits>

namespace aw {

// channel 规格增补 C2 / C4：channel 自己的错误码（asio experimental::channel_errc 的对应物）。
enum class channel_errc : int { channel_closed = 1, channel_cancelled = 2 };

namespace detail {
class channel_category_impl final : public std::error_category {
  public:
    char const* name() const noexcept override { return "aw.channel"; }
    std::string message(int value) const override {
        switch (value) {
        case static_cast<int>(channel_errc::channel_closed):
            return "channel closed";
        case static_cast<int>(channel_errc::channel_cancelled):
            return "channel cancelled";
        default:
            return "unknown channel error";
        }
    }
};
} // namespace detail

inline std::error_category const& channel_category() noexcept {
    static detail::channel_category_impl const instance;
    return instance;
}

inline std::error_code make_error_code(channel_errc value) noexcept {
    return std::error_code{static_cast<int>(value), channel_category()};
}

} // namespace aw

namespace std {
template <> struct is_error_code_enum<aw::channel_errc> : true_type {};
} // namespace std
