// cxx11_compat.hpp
// Minimal C++11 replacements for the few C++14/17 library facilities used by the
// bundled FTXUI amalgamation (std::optional / std::nullopt / std::make_unique).
// This lets PlotEngine build with -std=c++11 without pulling in <optional>,
// <variant> or <string_view>.
#ifndef FTXUI_CXX11_COMPAT_HPP
#define FTXUI_CXX11_COMPAT_HPP

#include <memory>
#include <utility>

namespace ftxui {

// ---- nullopt ---------------------------------------------------------------
struct nullopt_t {
  struct init {};
  explicit nullopt_t(init) {}
};
static const nullopt_t nullopt{nullopt_t::init{}};

// ---- optional<T> -----------------------------------------------------------
// Only the subset of the interface actually consumed by the amalgamation is
// implemented: default/nullopt/value construction, assignment, bool test,
// dereference, value(), ordering (for std::sort) and value comparison.
template <typename T>
class optional {
 public:
  optional() : has_value_(false), value_() {}
  optional(nullopt_t) : has_value_(false), value_() {}          // NOLINT
  optional(const T& value) : has_value_(true), value_(value) {}  // NOLINT
  optional(const optional&) = default;
  optional& operator=(const optional&) = default;

  optional& operator=(nullopt_t) {
    has_value_ = false;
    return *this;
  }
  optional& operator=(const T& value) {
    has_value_ = true;
    value_ = value;
    return *this;
  }

  explicit operator bool() const { return has_value_; }
  bool has_value() const { return has_value_; }

  T& value() { return value_; }
  const T& value() const { return value_; }
  T& operator*() { return value_; }
  const T& operator*() const { return value_; }

  // Ordering: an empty optional is considered smaller than any value.
  bool operator<(const optional& other) const {
    if (!other.has_value_) {
      return false;
    }
    if (!has_value_) {
      return true;
    }
    return value_ < other.value_;
  }

  bool operator==(const T& other) const {
    return has_value_ ? (value_ == other) : false;
  }
  bool operator!=(const T& other) const { return !(*this == other); }

 private:
  bool has_value_;
  T value_;
};

// ---- make_unique -----------------------------------------------------------
template <typename T, typename... Args>
std::unique_ptr<T> make_unique(Args&&... args) {
  return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

}  // namespace ftxui

#endif  // FTXUI_CXX11_COMPAT_HPP
