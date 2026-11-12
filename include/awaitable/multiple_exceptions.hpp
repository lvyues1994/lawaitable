#pragma once

#include <exception>
#include <utility>

namespace aw {

class multiple_exceptions : public std::exception {
  public:
    explicit multiple_exceptions(std::exception_ptr first) noexcept : first_{std::move(first)} {}
    std::exception_ptr first_exception() const { return first_; }
    char const* what() const noexcept override { return "multiple exceptions"; }

  private:
    std::exception_ptr first_;
};

} // namespace aw
