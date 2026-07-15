// =====================================================================
// plotengine_release.cpp - single-file text-adventure game engine
// (PlotEngine + FTXUI 4.1.1 amalgamation + C++11 compat shim)
//
// Build (Windows / MinGW):
//   g++ -std=gnu++11 -O2 -finput-charset=UTF-8 -fexec-charset=UTF-8 -DUNICODE -D_UNICODE plotengine_release.cpp \
//       -o plotengine.exe
// Build (Linux):
//   g++ -std=gnu++11 -O2 -finput-charset=UTF-8 -fexec-charset=UTF-8 plotengine_release.cpp -o plotengine
//
// This file is fully self-contained: all headers have been inlined in
// dependency order (mingw_std_threads.hpp -> cxx11_compat.hpp ->
// ftxui_amalgamation.hpp -> app). No -pthread / -lpthread needed.
// FTXUI is MIT licensed, Copyright (c) 2019 Arthur Sonzogni.
// =====================================================================

// ===================== Part 0: mingw_std_threads.hpp ===============
// =====================================================================
// mingw_std_threads.hpp
//
// Drop-in replacement for the C++ <mutex> / <thread> / <condition_variable>
// facilities on MinGW toolchains whose libstdc++ was built WITHOUT POSIX
// thread support (i.e. _GLIBCXX_HAS_GTHREADS is not defined). It implements
// the missing std::mutex / std::thread / std::condition_variable on top of
// native Win32 APIs, keeping everything in namespace std so existing code
// (e.g. the bundled FTXUI) does not need any changes.
//
// On toolchains that already ship working threads (normal MinGW-w64, MSVC,
// libc++) this header is a transparent no-op.
//
// Usage: include this header ONCE, as early as possible (before any FTXUI /
// <mutex> / <thread> usage), e.g. at the very top of your .cpp:
//     #include "mingw_std_threads.hpp"
// No extra -I flag or link library is required (uses kernel32 only).
// =====================================================================
#ifndef MINGW_STD_THREADS_COMPAT_HPP
#define MINGW_STD_THREADS_COMPAT_HPP

// Force _GLIBCXX_HAS_GTHREADS (and friends) to be defined before we test for
// it below. On toolchains WITH working threads, <mutex> transitively includes
// <bits/gthr.h> which defines the macro, so our guard correctly detects them.
// On toolchains WITHOUT thread support <mutex> is a guarded-empty header, so
// this include is harmless and the macro simply stays undefined.
// This must come BEFORE the #if, because this header is typically included
// very early (before any other standard header that would define the macro).
#include <mutex>

#if defined(_GLIBCXX_HAS_GTHREADS) || defined(_LIBCPP_VERSION) || \
    defined(_MSC_VER)
  // The standard library already provides working threads.
  #include <thread>
  #include <condition_variable>
#else
  // -------------------------------------------------------------------
  // No working std threads -> provide a Win32 based implementation.
  // -------------------------------------------------------------------
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  // CONDITION_VARIABLE / SleepConditionVariableCS are Vista+ (0x0600) Win32
  // APIs, gated behind _WIN32_WINNT in <windows.h>. Force the macro up to
  // 0x0600 unconditionally: a plain "#ifndef" guard is not enough, because
  // the macro may already be defined lower (command line / another header),
  // or <windows.h> may already have been parsed without it.
  #undef _WIN32_WINNT
  #define _WIN32_WINNT 0x0600
  #undef WINVER
  #define WINVER 0x0600

  #include <windows.h>
  #include <process.h>
  #include <exception>
  #include <cstddef>
  #include <cstdint>
  #include <utility>
  #include <tuple>
  #include <type_traits>
  #include <chrono>
  #include <system_error>
  #include <cerrno>

  // Helper used to expand a stored argument tuple into a function call
  // (std::index_sequence is C++14, so we provide our own).
  namespace mingw_compat {
  template <size_t... I>
  struct index_sequence {};
  template <size_t N, size_t... I>
  struct make_index_sequence : make_index_sequence<N - 1, N - 1, I...> {};
  template <size_t... I>
  struct make_index_sequence<0, I...> : index_sequence<I...> {};

  template <class Func, class Tuple, size_t... I>
  inline void apply_impl(Func& f, Tuple& t, index_sequence<I...>) {
    f(std::move(std::get<I>(t))...);
  }
  }  // namespace mingw_compat

  namespace std {

  // --------------------------- mutex -----------------------------------
  class mutex {
   public:
    typedef CRITICAL_SECTION* native_handle_type;
    mutex() noexcept { InitializeCriticalSection(&cs_); }
    ~mutex() { DeleteCriticalSection(&cs_); }
    mutex(const mutex&) = delete;
    mutex& operator=(const mutex&) = delete;
    void lock() { EnterCriticalSection(&cs_); }
    void unlock() { LeaveCriticalSection(&cs_); }
    bool try_lock() noexcept { return TryEnterCriticalSection(&cs_) != 0; }
    native_handle_type native_handle() { return &cs_; }

   private:
    CRITICAL_SECTION cs_;
  };

  // ------------------------ recursive_mutex ----------------------------
  class recursive_mutex {
   public:
    typedef CRITICAL_SECTION* native_handle_type;
    recursive_mutex() { InitializeCriticalSection(&cs_); }
    ~recursive_mutex() { DeleteCriticalSection(&cs_); }
    recursive_mutex(const recursive_mutex&) = delete;
    recursive_mutex& operator=(const recursive_mutex&) = delete;
    void lock() { EnterCriticalSection(&cs_); }
    void unlock() { LeaveCriticalSection(&cs_); }
    bool try_lock() noexcept { return TryEnterCriticalSection(&cs_) != 0; }
    native_handle_type native_handle() { return &cs_; }

   private:
    CRITICAL_SECTION cs_;
  };

  // ----------------------- condition_variable --------------------------
  // NOTE on what we re-provide vs. what <mutex>/<condition_variable> already
  // give us UNCONDITIONALLY in libstdc++ (even when _GLIBCXX_HAS_GTHREADS is
  // undefined, i.e. on a "broken" MinGW):
  //   * lock tag types (defer_lock_t / adopt_lock_t / try_to_lock_t) + their
  //     constexpr instances  -> provided by <mutex>, do NOT redefine.
  //   * std::lock_guard<M>  -> provided by <mutex> (outside the gthreads
  //     guard), do NOT redefine (would cause "redefinition of
  //     'class std::lock_guard<_Mutex>'").
  //   * std::unique_lock<M> -> provided by <bits/unique_lock.h> (also outside
  //     the gthreads guard), do NOT redefine.
  // Only the actual synchronization primitives below are guarded out on broken
  // toolchains and therefore re-provided here: std::mutex, std::recursive_mutex,
  // std::cv_status + std::condition_variable, std::thread, std::this_thread.
  enum class cv_status { no_timeout, timeout };

  class condition_variable {
   public:
    typedef CONDITION_VARIABLE* native_handle_type;

    condition_variable() { InitializeConditionVariable(&cv_); }
    condition_variable(const condition_variable&) = delete;
    condition_variable& operator=(const condition_variable&) = delete;
    ~condition_variable() {}

    void notify_one() noexcept { WakeConditionVariable(&cv_); }
    void notify_all() noexcept { WakeAllConditionVariable(&cv_); }

    void wait(unique_lock<mutex>& lock) {
      SleepConditionVariableCS(&cv_, lock.mutex()->native_handle(), INFINITE);
    }

    template <class Predicate>
    void wait(unique_lock<mutex>& lock, Predicate pred) {
      while (!pred()) wait(lock);
    }

    template <class Rep, class Period>
    cv_status wait_for(unique_lock<mutex>& lock,
                       const chrono::duration<Rep, Period>& rel_time) {
      DWORD ms =
          (DWORD)chrono::duration_cast<chrono::milliseconds>(rel_time).count();
      if (ms == 0) ms = 1;
      if (SleepConditionVariableCS(&cv_, lock.mutex()->native_handle(), ms))
        return cv_status::no_timeout;
      if (GetLastError() == ERROR_TIMEOUT) return cv_status::timeout;
      return cv_status::no_timeout;
    }

    template <class Rep, class Period, class Predicate>
    bool wait_for(unique_lock<mutex>& lock,
                  const chrono::duration<Rep, Period>& rel_time,
                  Predicate pred) {
      return wait_for(lock, rel_time) == cv_status::no_timeout && pred();
    }

    native_handle_type native_handle() { return &cv_; }

   private:
    CONDITION_VARIABLE cv_;
  };

  // ---------------------------- thread ---------------------------------
  class thread {
   public:
    typedef HANDLE native_handle_type;

    class id {
     public:
      id() noexcept : id_(0) {}
      explicit id(unsigned int i) noexcept : id_(i) {}
      bool operator==(const id& o) const noexcept { return id_ == o.id_; }
      bool operator!=(const id& o) const noexcept { return id_ != o.id_; }
      bool operator<(const id& o) const noexcept { return id_ < o.id_; }
      bool operator<=(const id& o) const noexcept { return id_ <= o.id_; }
      bool operator>(const id& o) const noexcept { return id_ > o.id_; }
      bool operator>=(const id& o) const noexcept { return id_ >= o.id_; }
      unsigned int get() const noexcept { return id_; }

     private:
      unsigned int id_;
    };

    thread() noexcept : handle_(nullptr), win_id_(0), id_(0) {}

    template <class Func, class... Args,
              class = typename enable_if<!is_same<
                  typename decay<Func>::type, thread>::value>::type>
    explicit thread(Func&& func, Args&&... args) {
      typedef thread_data<typename decay<Func>::type,
                          typename decay<Args>::type...> data_t;
      data_t* d = new data_t(std::forward<Func>(func),
                             std::forward<Args>(args)...);
      handle_ = reinterpret_cast<HANDLE>(
          _beginthreadex(nullptr, 0, &thread::trampoline<data_t>, d, 0,
                         &win_id_));
      if (handle_ == nullptr) {
        delete d;
        throw system_error(error_code(EAGAIN, system_category()),
                           "thread create failed");
      }
      id_ = id(win_id_);
    }

    thread(thread&& o) noexcept
        : handle_(o.handle_), win_id_(o.win_id_), id_(o.id_) {
      o.handle_ = nullptr;
      o.win_id_ = 0;
      o.id_ = id(0);
    }
    thread& operator=(thread&& o) noexcept {
      if (joinable()) std::terminate();
      handle_ = o.handle_;
      win_id_ = o.win_id_;
      id_ = o.id_;
      o.handle_ = nullptr;
      o.win_id_ = 0;
      o.id_ = id(0);
      return *this;
    }
    thread(const thread&) = delete;
    thread& operator=(const thread&) = delete;

    ~thread() {
      if (joinable()) std::terminate();
    }

    bool joinable() const noexcept { return handle_ != nullptr; }
    id get_id() const noexcept { return id_; }
    native_handle_type native_handle() { return handle_; }

    void join() {
      if (!joinable())
        throw system_error(error_code(EINVAL, system_category()),
                           "thread not joinable");
      WaitForSingleObject(handle_, INFINITE);
      CloseHandle(handle_);
      handle_ = nullptr;
      win_id_ = 0;
      id_ = id(0);
    }

    void detach() {
      if (!joinable())
        throw system_error(error_code(EINVAL, system_category()),
                           "thread not joinable");
      CloseHandle(handle_);
      handle_ = nullptr;
      win_id_ = 0;
      id_ = id(0);
    }

    static unsigned int hardware_concurrency() noexcept {
      SYSTEM_INFO si;
      GetSystemInfo(&si);
      return si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
    }

   private:
    template <class Func, class... Args>
    struct thread_data {
      Func func;
      tuple<Args...> args;
      thread_data(Func&& f, Args&&... a)
          : func(std::forward<Func>(f)), args(std::forward<Args>(a)...) {}
    };

    template <class Data>
    static unsigned __stdcall trampoline(void* p) {
      Data* d = static_cast<Data*>(p);
      mingw_compat::apply_impl(
          d->func, d->args,
          mingw_compat::make_index_sequence<
              tuple_size<decltype(d->args)>::value>{});
      delete d;
      return 0;
    }

    HANDLE handle_;
    unsigned int win_id_;
    id id_;
  };

  namespace this_thread {
  inline thread::id get_id() noexcept {
    return thread::id((unsigned int)GetCurrentThreadId());
  }
  inline void yield() noexcept { SwitchToThread(); }
  template <class Rep, class Period>
  inline void sleep_for(const chrono::duration<Rep, Period>& d) {
    DWORD ms = (DWORD)chrono::duration_cast<chrono::milliseconds>(d).count();
    if (ms == 0) ms = 1;
    Sleep(ms);
  }
  }  // namespace this_thread

  }  // namespace std
#endif  // !working std threads
#endif  // MINGW_STD_THREADS_COMPAT_HPP


// ===================== Part 1: cxx11_compat.hpp =====================
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


// ============== Part 2: ftxui_amalgamation.hpp (FTXUI) ==============
// =====================================================================
// ftxui_amalgamation.hpp - single-header build of FTXUI 4.1.1
// Auto-generated by tools/amalgamate_ftxui.py. DO NOT EDIT BY HAND.
//
// FTXUI - https://github.com/ArthurSonzogni/FTXUI
// The MIT License (MIT)
// Copyright (c) 2019 Arthur Sonzogni.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND. See the
// full text in FTXUI-4.1.1/LICENSE.
// =====================================================================
#ifndef FTXUI_AMALGAMATION_HPP
#define FTXUI_AMALGAMATION_HPP

#ifndef FTXUI_SCREEN_BOX_HPP
#define FTXUI_SCREEN_BOX_HPP

namespace ftxui {

struct Box {
  int x_min = 0;
  int x_max = 0;
  int y_min = 0;
  int y_max = 0;

  Box() = default;
  Box(int xmin, int xmax, int ymin, int ymax)
      : x_min(xmin), x_max(xmax), y_min(ymin), y_max(ymax) {}

  static auto Intersection(Box a, Box b) -> Box;
  static auto Union(Box a, Box b) -> Box;
  bool Contain(int x, int y) const;
  bool operator==(const Box& other) const;
  bool operator!=(const Box& other) const;
};

}  // namespace ftxui

#endif  // FTXUI_SCREEN_BOX_HPP

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

#include <algorithm>

namespace ftxui {
/// @return the biggest Box contained in both |a| and |b|.
/// @ingroup screen
// static
Box Box::Intersection(Box a, Box b) {
  return Box{
      std::max(a.x_min, b.x_min),
      std::min(a.x_max, b.x_max),
      std::max(a.y_min, b.y_min),
      std::min(a.y_max, b.y_max),
  };
}

/// @return the smallest Box containing both |a| and |b|.
/// @ingroup screen
// static
Box Box::Union(Box a, Box b) {
  return Box{
      std::min(a.x_min, b.x_min),
      std::max(a.x_max, b.x_max),
      std::min(a.y_min, b.y_min),
      std::max(a.y_max, b.y_max),
  };
}

/// @return whether (x,y) is contained inside the box.
/// @ingroup screen
bool Box::Contain(int x, int y) const {
  return x_min <= x &&  //
         x_max >= x &&  //
         y_min <= y &&  //
         y_max >= y;
}

/// @return whether |other| is the same as |this|
/// @ingroup screen
bool Box::operator==(const Box& other) const {
  return (x_min == other.x_min) && (x_max == other.x_max) &&
         (y_min == other.y_min) && (y_max == other.y_max);
}

/// @return whether |other| and |this| are different.
/// @ingroup screen
bool Box::operator!=(const Box& other) const {
  return !operator==(other);
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#ifndef FTXUI_SCREEN_COLOR_HPP
#define FTXUI_SCREEN_COLOR_HPP

#include <cstdint>  // for uint8_t
#include <string>   // for string
#include <vector>   // for vector

#ifdef RGB
// Workaround for wingdi.h (via Windows.h) defining macros that break things.
// https://docs.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-rgb
#undef RGB
#endif

namespace ftxui {

/// @brief A class representing terminal colors.
/// @ingroup screen
class Color {
 public:
  enum Palette1 : uint8_t;
  enum Palette16 : uint8_t;
  enum Palette256 : uint8_t;

  Color();                  // Transparent.
  Color(Palette1 index);    // Transparent.
  Color(Palette16 index);   // Implicit conversion from index to Color.
  Color(Palette256 index);  // Implicit conversion from index to Color.
  Color(uint8_t red, uint8_t green, uint8_t blue);
  static Color RGB(uint8_t red, uint8_t green, uint8_t blue);
  static Color HSV(uint8_t hue, uint8_t saturation, uint8_t value);
  static Color Interpolate(float t, const Color& a, const Color& b);

  //---------------------------
  // List of colors:
  //---------------------------
  // clang-format off
  enum Palette1 : uint8_t{
    Default, // Transparent
  };

  enum Palette16 : uint8_t {
    Black        = 0,
    Red          = 1,
    Green        = 2,
    Yellow       = 3,
    Blue         = 4,
    Magenta      = 5,
    Cyan         = 6,
    GrayLight    = 7,
    GrayDark     = 8,
    RedLight     = 9,
    GreenLight   = 10,
    YellowLight  = 11,
    BlueLight    = 12,
    MagentaLight = 13,
    CyanLight    = 14,
    White        = 15,
  };

  enum Palette256 : uint8_t {
    Aquamarine1        = 122,
    Aquamarine1Bis     = 86,
    Aquamarine3        = 79,
    Blue1              = 21,
    Blue3              = 19,
    Blue3Bis           = 20,
    BlueViolet         = 57,
    CadetBlue          = 72,
    CadetBlueBis       = 73,
    Chartreuse1        = 118,
    Chartreuse2        = 112,
    Chartreuse2Bis     = 82,
    Chartreuse3        = 70,
    Chartreuse3Bis     = 76,
    Chartreuse4        = 64,
    CornflowerBlue     = 69,
    Cornsilk1          = 230,
    Cyan1              = 51,
    Cyan2              = 50,
    Cyan3              = 43,
    DarkBlue           = 18,
    DarkCyan           = 36,
    DarkGoldenrod      = 136,
    DarkGreen          = 22,
    DarkKhaki          = 143,
    DarkMagenta        = 90,
    DarkMagentaBis     = 91,
    DarkOliveGreen1    = 191,
    DarkOliveGreen1Bis = 192,
    DarkOliveGreen2    = 155,
    DarkOliveGreen3    = 107,
    DarkOliveGreen3Bis = 113,
    DarkOliveGreen3Ter = 149,
    DarkOrange         = 208,
    DarkOrange3        = 130,
    DarkOrange3Bis     = 166,
    DarkRed            = 52,
    DarkRedBis         = 88,
    DarkSeaGreen       = 108,
    DarkSeaGreen1      = 158,
    DarkSeaGreen1Bis   = 193,
    DarkSeaGreen2      = 151,
    DarkSeaGreen2Bis   = 157,
    DarkSeaGreen3      = 115,
    DarkSeaGreen3Bis   = 150,
    DarkSeaGreen4      = 65,
    DarkSeaGreen4Bis   = 71,
    DarkSlateGray1     = 123,
    DarkSlateGray2     = 87,
    DarkSlateGray3     = 116,
    DarkTurquoise      = 44,
    DarkViolet         = 128,
    DarkVioletBis      = 92,
    DeepPink1          = 198,
    DeepPink1Bis       = 199,
    DeepPink2          = 197,
    DeepPink3          = 161,
    DeepPink3Bis       = 162,
    DeepPink4          = 125,
    DeepPink4Bis          = 89,
    DeepPink4Ter       = 53,
    DeepSkyBlue1       = 39,
    DeepSkyBlue2       = 38,
    DeepSkyBlue3       = 31,
    DeepSkyBlue3Bis    = 32,
    DeepSkyBlue4       = 23,
    DeepSkyBlue4Bis    = 24,
    DeepSkyBlue4Ter    = 25,
    DodgerBlue1        = 33,
    DodgerBlue2        = 27,
    DodgerBlue3        = 26,
    Gold1              = 220,
    Gold3              = 142,
    Gold3Bis           = 178,
    Green1             = 46,
    Green3             = 34,
    Green3Bis          = 40,
    Green4             = 28,
    GreenYellow        = 154,
    Grey0              = 16,
    Grey100            = 231,
    Grey11             = 234,
    Grey15             = 235,
    Grey19             = 236,
    Grey23             = 237,
    Grey27             = 238,
    Grey3              = 232,
    Grey30             = 239,
    Grey35             = 240,
    Grey37             = 59,
    Grey39             = 241,
    Grey42             = 242,
    Grey46             = 243,
    Grey50             = 244,
    Grey53             = 102,
    Grey54             = 245,
    Grey58             = 246,
    Grey62             = 247,
    Grey63             = 139,
    Grey66             = 248,
    Grey69             = 145,
    Grey7              = 233,
    Grey70             = 249,
    Grey74             = 250,
    Grey78             = 251,
    Grey82             = 252,
    Grey84             = 188,
    Grey85             = 253,
    Grey89             = 254,
    Grey93             = 255,
    Honeydew2          = 194,
    HotPink            = 205,
    HotPink2           = 169,
    HotPink3           = 132,
    HotPink3Bis        = 168,
    HotPinkBis         = 206,
    IndianRed          = 131,
    IndianRed1         = 203,
    IndianRed1Bis      = 204,
    IndianRedBis       = 167,
    Khaki1             = 228,
    Khaki3             = 185,
    LightCoral         = 210,
    LightCyan1Bis      = 195,
    LightCyan3         = 152,
    LightGoldenrod1    = 227,
    LightGoldenrod2    = 186,
    LightGoldenrod2Bis = 221,
    LightGoldenrod2Ter = 222,
    LightGoldenrod3    = 179,
    LightGreen         = 119,
    LightGreenBis      = 120,
    LightPink1         = 217,
    LightPink3         = 174,
    LightPink4         = 95,
    LightSalmon1       = 216,
    LightSalmon3       = 137,
    LightSalmon3Bis       = 173,
    LightSeaGreen      = 37,
    LightSkyBlue1      = 153,
    LightSkyBlue3      = 109,
    LightSkyBlue3Bis      = 110,
    LightSlateBlue     = 105,
    LightSlateGrey     = 103,
    LightSteelBlue     = 147,
    LightSteelBlue1    = 189,
    LightSteelBlue3    = 146,
    LightYellow3       = 187,
    Magenta1           = 201,
    Magenta2           = 165,
    Magenta2Bis        = 200,
    Magenta3           = 127,
    Magenta3Bis        = 163,
    Magenta3Ter        = 164,
    MediumOrchid       = 134,
    MediumOrchid1      = 171,
    MediumOrchid1Bis   = 207,
    MediumOrchid3      = 133,
    MediumPurple       = 104,
    MediumPurple1      = 141,
    MediumPurple2      = 135,
    MediumPurple2Bis   = 140,
    MediumPurple3      = 97,
    MediumPurple3Bis   = 98,
    MediumPurple4      = 60,
    MediumSpringGreen  = 49,
    MediumTurquoise    = 80,
    MediumVioletRed    = 126,
    MistyRose1         = 224,
    MistyRose3         = 181,
    NavajoWhite1       = 223,
    NavajoWhite3       = 144,
    NavyBlue           = 17,
    Orange1            = 214,
    Orange3            = 172,
    Orange4            = 58,
    Orange4Bis         = 94,
    OrangeRed1         = 202,
    Orchid             = 170,
    Orchid1            = 213,
    Orchid2            = 212,
    PaleGreen1         = 121,
    PaleGreen1Bis      = 156,
    PaleGreen3         = 114,
    PaleGreen3Bis      = 77,
    PaleTurquoise1     = 159,
    PaleTurquoise4     = 66,
    PaleVioletRed1     = 211,
    Pink1              = 218,
    Pink3              = 175,
    Plum1              = 219,
    Plum2              = 183,
    Plum3              = 176,
    Plum4              = 96,
    Purple             = 129,
    Purple3            = 56,
    Purple4            = 54,
    Purple4Bis         = 55,
    PurpleBis          = 93,
    Red1               = 196,
    Red3               = 124,
    Red3Bis            = 160,
    RosyBrown          = 138,
    RoyalBlue1         = 63,
    Salmon1            = 209,
    SandyBrown         = 215,
    SeaGreen1          = 84,
    SeaGreen1Bis       = 85,
    SeaGreen2          = 83,
    SeaGreen3          = 78,
    SkyBlue1           = 117,
    SkyBlue2           = 111,
    SkyBlue3           = 74,
    SlateBlue1         = 99,
    SlateBlue3         = 61,
    SlateBlue3Bis      = 62,
    SpringGreen1       = 48,
    SpringGreen2       = 42,
    SpringGreen2Bis    = 47,
    SpringGreen3       = 35,
    SpringGreen3Bis    = 41,
    SpringGreen4       = 29,
    SteelBlue          = 67,
    SteelBlue1         = 75,
    SteelBlue1Bis      = 81,
    SteelBlue3         = 68,
    Tan                = 180,
    Thistle1           = 225,
    Thistle3           = 182,
    Turquoise2         = 45,
    Turquoise4         = 30,
    Violet             = 177,
    Wheat1             = 229,
    Wheat4             = 101,
    Yellow1            = 226,
    Yellow2            = 190,
    Yellow3            = 148,
    Yellow3Bis         = 184,
    Yellow4            = 100,
    Yellow4Bis         = 106,
  };
  // clang-format on

  // --- Operators ------
  bool operator==(const Color& rhs) const;
  bool operator!=(const Color& rhs) const;

  std::string Print(bool is_background_color) const;

 private:
  enum class ColorType : uint8_t {
    Palette1,
    Palette16,
    Palette256,
    TrueColor,
  };
  ColorType type_ = ColorType::Palette1;
  uint8_t red_ = 0;
  uint8_t green_ = 0;
  uint8_t blue_ = 0;
};

inline namespace literals {

/// @brief Creates a color from a combined hex RGB representation,
/// e.g. 0x808000_rgb
Color operator""_rgb(unsigned long long int combined);

}  // namespace literals

}  // namespace ftxui

#endif  // FTXUI_SCREEN_COLOR_HPP

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

#include <array>  // for array
#include <cmath>

#ifndef FTXUI_SCREEN_COLOR_INFO_HPP
#define FTXUI_SCREEN_COLOR_INFO_HPP

#include <cstdint>

namespace ftxui {

struct ColorInfo {
  const char* name;
  uint8_t index_256;
  uint8_t index_16;
  uint8_t red;
  uint8_t green;
  uint8_t blue;
  uint8_t hue;
  uint8_t saturation;
  uint8_t value;
};

ColorInfo GetColorInfo(Color::Palette256 index);
ColorInfo GetColorInfo(Color::Palette16 index);

}  // namespace ftxui

#endif  // FTXUI_SCREEN_COLOR_INFO_HPP

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#ifndef FTXUI_SCREEN_TERMINAL_HPP
#define FTXUI_SCREEN_TERMINAL_HPP

namespace ftxui {
struct Dimensions {
  int dimx;
  int dimy;
};

namespace Terminal {
Dimensions Size();
void SetFallbackSize(const Dimensions& fallbackSize);

enum Color {
  Palette1,
  Palette16,
  Palette256,
  TrueColor,
};
Color ColorSupport();
void SetColorSupport(Color color);

}  // namespace Terminal

}  // namespace ftxui

#endif  // FTXUI_SCREEN_TERMINAL_HPP

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

namespace ftxui {

// using namespace std::literals;  // removed: std::literals is C++14; the "s" suffixes below are expanded manually

namespace {
const std::array<const char*, 33> palette16code = {
    "30", "40",   //
    "31", "41",   //
    "32", "42",   //
    "33", "43",   //
    "34", "44",   //
    "35", "45",   //
    "36", "46",   //
    "37", "47",   //
    "90", "100",  //
    "91", "101",  //
    "92", "102",  //
    "93", "103",  //
    "94", "104",  //
    "95", "105",  //
    "96", "106",  //
    "97", "107",  //
};

}  // namespace

bool Color::operator==(const Color& rhs) const {
  return red_ == rhs.red_ && green_ == rhs.green_ && blue_ == rhs.blue_ &&
         type_ == rhs.type_;
}

bool Color::operator!=(const Color& rhs) const {
  return !operator==(rhs);
}

std::string Color::Print(bool is_background_color) const {
  switch (type_) {
    case ColorType::Palette1:
      return is_background_color ? std::string("49") : std::string("39");

    case ColorType::Palette16:
      return palette16code[2 * red_ + is_background_color];  // NOLINT;

    case ColorType::Palette256:
      return (is_background_color ? std::string("48;5;") : std::string("38;5;")) + std::to_string(red_);

    case ColorType::TrueColor:
    default:
      return (is_background_color ? std::string("48;2;") : std::string("38;2;"))  //
             + std::to_string(red_) + ";"                 //
             + std::to_string(green_) + ";"               //
             + std::to_string(blue_);                     //
  }
}

/// @brief Build a transparent color.
/// @ingroup screen
Color::Color() = default;

/// @brief Build a transparent color.
/// @ingroup screen
Color::Color(Palette1 /*value*/) : Color() {}

/// @brief Build a transparent using Palette16 colors.
/// @ingroup screen
Color::Color(Palette16 index) : type_(ColorType::Palette16), red_(index) {}

/// @brief Build a transparent using Palette256 colors.
/// @ingroup screen
Color::Color(Palette256 index) : type_(ColorType::Palette256), red_(index) {
  if (Terminal::ColorSupport() >= Terminal::Color::Palette256) {
    return;
  }
  type_ = ColorType::Palette16;
  red_ = GetColorInfo(Color::Palette256(red_)).index_16;
}

/// @brief Build a Color from its RGB representation.
/// https://en.wikipedia.org/wiki/RGB_color_model
///
/// @param red The quantity of red [0,255]
/// @param green The quantity of green [0,255]
/// @param blue The quantity of blue [0,255]
/// @ingroup screen
Color::Color(uint8_t red, uint8_t green, uint8_t blue)
    : type_(ColorType::TrueColor), red_(red), green_(green), blue_(blue) {
  if (Terminal::ColorSupport() == Terminal::Color::TrueColor) {
    return;
  }

  // Find the closest Color from the database:
  const int max_distance = 256 * 256 * 3;
  int closest = max_distance;
  int best = 0;
  const int database_begin = 16;
  const int database_end = 256;
  for (int i = database_begin; i < database_end; ++i) {
    const ColorInfo color_info = GetColorInfo(Color::Palette256(i));
    const int dr = color_info.red - red;
    const int dg = color_info.green - green;
    const int db = color_info.blue - blue;
    const int dist = dr * dr + dg * dg + db * db;
    if (closest > dist) {
      closest = dist;
      best = i;
    }
  }

  if (Terminal::ColorSupport() == Terminal::Color::Palette256) {
    type_ = ColorType::Palette256;
    red_ = best;
  } else {
    type_ = ColorType::Palette16;
    red_ = GetColorInfo(Color::Palette256(best)).index_16;
  }
}

/// @brief Build a Color from its RGB representation.
/// https://en.wikipedia.org/wiki/RGB_color_model
///
/// @param red The quantity of red [0,255]
/// @param green The quantity of green [0,255]
/// @param blue The quantity of blue [0,255]
/// @ingroup screen
// static
Color Color::RGB(uint8_t red, uint8_t green, uint8_t blue) {
  return {red, green, blue};
}

/// @brief Build a Color from its HSV representation.
/// https://en.wikipedia.org/wiki/HSL_and_HSV
///
/// @param h The hue of the color [0,255]
/// @param s The "colorfulness" [0,255].
/// @param v The "Lightness" [0,255]
/// @ingroup screen
// static
Color Color::HSV(uint8_t h, uint8_t s, uint8_t v) {
  if (s == 0) {
    return {0, 0, 0};
  }

  uint8_t region = h / 43;                                        // NOLINT
  uint8_t remainder = (h - (region * 43)) * 6;                    // NOLINT
  uint8_t p = (v * (255 - s)) >> 8;                               // NOLINT
  uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;          // NOLINT
  uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;  // NOLINT

  // clang-format off
  switch (region) {              // NOLINT
    case 0: return Color(v,t,p); // NOLINT
    case 1: return Color(q,v,p); // NOLINT
    case 2: return Color(p,v,t); // NOLINT
    case 3: return Color(p,q,v); // NOLINT
    case 4: return Color(t,p,v); // NOLINT
    case 5: return Color(v,p,q); // NOLINT
  }                              // NOLINT
  // clang-format on
  return {0, 0, 0};
}

// static
Color Color::Interpolate(float t, const Color& a, const Color& b) {
  if (a.type_ == ColorType::Palette1 ||  //
      b.type_ == ColorType::Palette1) {
    if (t < 0.5F) {  // NOLINT
      return a;
    } else {
      return b;
    }
  }

  auto get_color = [](const Color& color,  //
                      uint8_t* red, uint8_t* green, uint8_t* blue) {
    switch (color.type_) {
      case ColorType::Palette1: {
        return;
      }

      case ColorType::Palette16: {
        const ColorInfo info = GetColorInfo(Color::Palette16(color.red_));
        *red = info.red;
        *green = info.green;
        *blue = info.blue;
        return;
      }

      case ColorType::Palette256: {
        const ColorInfo info = GetColorInfo(Color::Palette256(color.red_));
        *red = info.red;
        *green = info.green;
        *blue = info.blue;
        return;
      }

      case ColorType::TrueColor:
      default: {
        *red = color.red_;
        *green = color.green_;
        *blue = color.blue_;
        return;
      }
    }
  };

  uint8_t red_a = 0;
  uint8_t green_a = 0;
  uint8_t blue_a = 0;
  uint8_t red_b = 0;
  uint8_t green_b = 0;
  uint8_t blue_b = 0;
  get_color(a, &red_a, &green_a, &blue_a);
  get_color(b, &red_b, &green_b, &blue_b);

  // Gamma correction:
  // https://en.wikipedia.org/wiki/Gamma_correction
  return Color::RGB(
      pow(pow(red_a, 2.2f) * (1 - t) + pow(red_b, 2.2f) * t, 1 / 2.2f),
      pow(pow(green_a, 2.2f) * (1 - t) + pow(green_b, 2.2f) * t, 1 / 2.2f),
      pow(pow(blue_a, 2.2f) * (1 - t) + pow(blue_b, 2.2f) * t, 1 / 2.2f));
}

inline namespace literals {

Color operator""_rgb(unsigned long long int combined) {
  // assert(combined <= 0xffffffU);
  auto const red = static_cast<uint8_t>(combined >> 16U);
  auto const green = static_cast<uint8_t>(combined >> 8U);
  auto const blue = static_cast<uint8_t>(combined);
  return {red, green, blue};
}

}  // namespace literals

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

#include <array>

namespace ftxui {

// clang-format off
const std::array<ColorInfo, 256> palette256 = {{
     {"Black"              , 0   , 0  , 0   , 0   , 0   , 0   , 0   , 0   } ,
     {"Red"                , 1   , 1  , 128 , 0   , 0   , 0   , 255 , 128 } ,
     {"Green"              , 2   , 2  , 0   , 128 , 0   , 85  , 255 , 128 } ,
     {"Yellow"             , 3   , 3  , 128 , 128 , 0   , 43  , 255 , 128 } ,
     {"Blue"               , 4   , 4  , 0   , 0   , 128 , 171 , 255 , 128 } ,
     {"Magenta"            , 5   , 5  , 128 , 0   , 128 , 213 , 255 , 128 } ,
     {"Cyan"               , 6   , 6  , 0   , 128 , 128 , 128 , 255 , 128 } ,
     {"GrayLight"          , 7   , 7  , 192 , 192 , 192 , 0   , 0   , 192 } ,
     {"GrayDark"           , 8   , 8  , 128 , 128 , 128 , 0   , 0   , 128 } ,
     {"RedLight"           , 9   , 9  , 255 , 0   , 0   , 0   , 255 , 255 } ,
     {"GreenLight"         , 10  , 10 , 0   , 255 , 0   , 85  , 255 , 255 } ,
     {"YellowLight"        , 11  , 11 , 255 , 255 , 0   , 43  , 255 , 255 } ,
     {"BlueLight"          , 12  , 12 , 0   , 0   , 255 , 171 , 255 , 255 } ,
     {"MagentaLight"       , 13  , 13 , 255 , 0   , 255 , 213 , 255 , 255 } ,
     {"CyanLight"          , 14  , 14 , 0   , 255 , 255 , 128 , 255 , 255 } ,
     {"White"              , 15  , 15 , 255 , 255 , 255 , 0   , 0   , 255 } ,
     {"Grey0"              , 16  , 0  , 0   , 0   , 0   , 0   , 0   , 0   } ,
     {"NavyBlue"           , 17  , 4  , 0   , 0   , 95  , 171 , 255 , 95  } ,
     {"DarkBlue"           , 18  , 4  , 0   , 0   , 135 , 171 , 255 , 135 } ,
     {"Blue3"              , 19  , 4  , 0   , 0   , 175 , 171 , 255 , 175 } ,
     {"Blue3Bis"           , 20  , 12 , 0   , 0   , 215 , 171 , 255 , 215 } ,
     {"Blue1"              , 21  , 12 , 0   , 0   , 255 , 171 , 255 , 255 } ,
     {"DarkGreen"          , 22  , 2  , 0   , 95  , 0   , 85  , 255 , 95  } ,
     {"DeepSkyBlue4"       , 23  , 6  , 0   , 95  , 95  , 128 , 255 , 95  } ,
     {"DeepSkyBlue4Bis"    , 24  , 6  , 0   , 95  , 135 , 141 , 255 , 135 } ,
     {"DeepSkyBlue4Ter"    , 25  , 6  , 0   , 95  , 175 , 148 , 255 , 175 } ,
     {"DodgerBlue3"        , 26  , 12 , 0   , 95  , 215 , 152 , 255 , 215 } ,
     {"DodgerBlue2"        , 27  , 12 , 0   , 95  , 255 , 155 , 255 , 255 } ,
     {"Green4"             , 28  , 2  , 0   , 135 , 0   , 85  , 255 , 135 } ,
     {"SpringGreen4"       , 29  , 6  , 0   , 135 , 95  , 115 , 255 , 135 } ,
     {"Turquoise4"         , 30  , 6  , 0   , 135 , 135 , 128 , 255 , 135 } ,
     {"DeepSkyBlue3"       , 31  , 6  , 0   , 135 , 175 , 138 , 255 , 175 } ,
     {"DeepSkyBlue3Bis"    , 32  , 14 , 0   , 135 , 215 , 144 , 255 , 215 } ,
     {"DodgerBlue1"        , 33  , 14 , 0   , 135 , 255 , 149 , 255 , 255 } ,
     {"Green3"             , 34  , 2  , 0   , 175 , 0   , 85  , 255 , 175 } ,
     {"SpringGreen3"       , 35  , 6  , 0   , 175 , 95  , 108 , 255 , 175 } ,
     {"DarkCyan"           , 36  , 6  , 0   , 175 , 135 , 118 , 255 , 175 } ,
     {"LightSeaGreen"      , 37  , 6  , 0   , 175 , 175 , 128 , 255 , 175 } ,
     {"DeepSkyBlue2"       , 38  , 14 , 0   , 175 , 215 , 136 , 255 , 215 } ,
     {"DeepSkyBlue1"       , 39  , 14 , 0   , 175 , 255 , 142 , 255 , 255 } ,
     {"Green3Bis"          , 40  , 10 , 0   , 215 , 0   , 85  , 255 , 215 } ,
     {"SpringGreen3Bis"    , 41  , 10 , 0   , 215 , 95  , 104 , 255 , 215 } ,
     {"SpringGreen2"       , 42  , 14 , 0   , 215 , 135 , 112 , 255 , 215 } ,
     {"Cyan3"              , 43  , 14 , 0   , 215 , 175 , 120 , 255 , 215 } ,
     {"DarkTurquoise"      , 44  , 14 , 0   , 215 , 215 , 128 , 255 , 215 } ,
     {"Turquoise2"         , 45  , 14 , 0   , 215 , 255 , 135 , 255 , 255 } ,
     {"Green1"             , 46  , 10 , 0   , 255 , 0   , 85  , 255 , 255 } ,
     {"SpringGreen2Bis"    , 47  , 10 , 0   , 255 , 95  , 101 , 255 , 255 } ,
     {"SpringGreen1"       , 48  , 14 , 0   , 255 , 135 , 107 , 255 , 255 } ,
     {"MediumSpringGreen"  , 49  , 14 , 0   , 255 , 175 , 114 , 255 , 255 } ,
     {"Cyan2"              , 50  , 14 , 0   , 255 , 215 , 121 , 255 , 255 } ,
     {"Cyan1"              , 51  , 14 , 0   , 255 , 255 , 128 , 255 , 255 } ,
     {"DarkRed"            , 52  , 1  , 95  , 0   , 0   , 0   , 255 , 95  } ,
     {"DeepPink4Ter"       , 53  , 5  , 95  , 0   , 95  , 213 , 255 , 95  } ,
     {"Purple4"            , 54  , 5  , 95  , 0   , 135 , 201 , 255 , 135 } ,
     {"Purple4Bis"         , 55  , 5  , 95  , 0   , 175 , 194 , 255 , 175 } ,
     {"Purple3"            , 56  , 12 , 95  , 0   , 215 , 190 , 255 , 215 } ,
     {"BlueViolet"         , 57  , 12 , 95  , 0   , 255 , 187 , 255 , 255 } ,
     {"Orange4"            , 58  , 3  , 95  , 95  , 0   , 43  , 255 , 95  } ,
     {"Grey37"             , 59  , 8  , 95  , 95  , 95  , 0   , 0   , 95  } ,
     {"MediumPurple4"      , 60  , 4  , 95  , 95  , 135 , 171 , 75  , 135 } ,
     {"SlateBlue3"         , 61  , 4  , 95  , 95  , 175 , 171 , 116 , 175 } ,
     {"SlateBlue3Bis"      , 62  , 12 , 95  , 95  , 215 , 171 , 142 , 215 } ,
     {"RoyalBlue1"         , 63  , 12 , 95  , 95  , 255 , 171 , 160 , 255 } ,
     {"Chartreuse4"        , 64  , 3  , 95  , 135 , 0   , 55  , 255 , 135 } ,
     {"DarkSeaGreen4"      , 65  , 8  , 95  , 135 , 95  , 85  , 75  , 135 } ,
     {"PaleTurquoise4"     , 66  , 6  , 95  , 135 , 135 , 128 , 75  , 135 } ,
     {"SteelBlue"          , 67  , 4  , 95  , 135 , 175 , 150 , 116 , 175 } ,
     {"SteelBlue3"         , 68  , 12 , 95  , 135 , 215 , 157 , 142 , 215 } ,
     {"CornflowerBlue"     , 69  , 12 , 95  , 135 , 255 , 161 , 160 , 255 } ,
     {"Chartreuse3"        , 70  , 3  , 95  , 175 , 0   , 62  , 255 , 175 } ,
     {"DarkSeaGreen4Bis"   , 71  , 2  , 95  , 175 , 95  , 85  , 116 , 175 } ,
     {"CadetBlue"          , 72  , 2  , 95  , 175 , 135 , 106 , 116 , 175 } ,
     {"CadetBlueBis"       , 73  , 6  , 95  , 175 , 175 , 128 , 116 , 175 } ,
     {"SkyBlue3"           , 74  , 14 , 95  , 175 , 215 , 143 , 142 , 215 } ,
     {"SteelBlue1"         , 75  , 12 , 95  , 175 , 255 , 150 , 160 , 255 } ,
     {"Chartreuse3Bis"     , 76  , 10 , 95  , 215 , 0   , 66  , 255 , 215 } ,
     {"PaleGreen3Bis"      , 77  , 10 , 95  , 215 , 95  , 85  , 142 , 215 } ,
     {"SeaGreen3"          , 78  , 10 , 95  , 215 , 135 , 99  , 142 , 215 } ,
     {"Aquamarine3"        , 79  , 14 , 95  , 215 , 175 , 113 , 142 , 215 } ,
     {"MediumTurquoise"    , 80  , 14 , 95  , 215 , 215 , 128 , 142 , 215 } ,
     {"SteelBlue1Bis"      , 81  , 14 , 95  , 215 , 255 , 139 , 160 , 255 } ,
     {"Chartreuse2Bis"     , 82  , 10 , 95  , 255 , 0   , 69  , 255 , 255 } ,
     {"SeaGreen2"          , 83  , 10 , 95  , 255 , 95  , 85  , 160 , 255 } ,
     {"SeaGreen1"          , 84  , 10 , 95  , 255 , 135 , 95  , 160 , 255 } ,
     {"SeaGreen1Bis"       , 85  , 10 , 95  , 255 , 175 , 106 , 160 , 255 } ,
     {"Aquamarine1Bis"     , 86  , 14 , 95  , 255 , 215 , 117 , 160 , 255 } ,
     {"DarkSlateGray2"     , 87  , 14 , 95  , 255 , 255 , 128 , 160 , 255 } ,
     {"DarkRedBis"         , 88  , 1  , 135 , 0   , 0   , 0   , 255 , 135 } ,
     {"DeepPink4Bis"       , 89  , 5  , 135 , 0   , 95  , 226 , 255 , 135 } ,
     {"DarkMagenta"        , 90  , 5  , 135 , 0   , 135 , 213 , 255 , 135 } ,
     {"DarkMagentaBis"     , 91  , 5  , 135 , 0   , 175 , 204 , 255 , 175 } ,
     {"DarkVioletBis"      , 92  , 13 , 135 , 0   , 215 , 198 , 255 , 215 } ,
     {"PurpleBis"          , 93  , 13 , 135 , 0   , 255 , 193 , 255 , 255 } ,
     {"Orange4Bis"         , 94  , 3  , 135 , 95  , 0   , 30  , 255 , 135 } ,
     {"LightPink4"         , 95  , 8  , 135 , 95  , 95  , 0   , 75  , 135 } ,
     {"Plum4"              , 96  , 5  , 135 , 95  , 135 , 213 , 75  , 135 } ,
     {"MediumPurple3"      , 97  , 4  , 135 , 95  , 175 , 192 , 116 , 175 } ,
     {"MediumPurple3Bis"   , 98  , 12 , 135 , 95  , 215 , 185 , 142 , 215 } ,
     {"SlateBlue1"         , 99  , 12 , 135 , 95  , 255 , 181 , 160 , 255 } ,
     {"Yellow4"            , 100 , 3  , 135 , 135 , 0   , 43  , 255 , 135 } ,
     {"Wheat4"             , 101 , 8  , 135 , 135 , 95  , 43  , 75  , 135 } ,
     {"Grey53"             , 102 , 8  , 135 , 135 , 135 , 0   , 0   , 135 } ,
     {"LightSlateGrey"     , 103 , 4  , 135 , 135 , 175 , 171 , 58  , 175 } ,
     {"MediumPurple"       , 104 , 12 , 135 , 135 , 215 , 171 , 94  , 215 } ,
     {"LightSlateBlue"     , 105 , 12 , 135 , 135 , 255 , 171 , 120 , 255 } ,
     {"Yellow4Bis"         , 106 , 3  , 135 , 175 , 0   , 52  , 255 , 175 } ,
     {"DarkOliveGreen3"    , 107 , 7  , 135 , 175 , 95  , 64  , 116 , 175 } ,
     {"DarkSeaGreen"       , 108 , 7  , 135 , 175 , 135 , 85  , 58  , 175 } ,
     {"LightSkyBlue3"      , 109 , 7  , 135 , 175 , 175 , 128 , 58  , 175 } ,
     {"LightSkyBlue3Bis"   , 110 , 12 , 135 , 175 , 215 , 150 , 94  , 215 } ,
     {"SkyBlue2"           , 111 , 12 , 135 , 175 , 255 , 157 , 120 , 255 } ,
     {"Chartreuse2"        , 112 , 11 , 135 , 215 , 0   , 58  , 255 , 215 } ,
     {"DarkOliveGreen3Bis" , 113 , 10 , 135 , 215 , 95  , 71  , 142 , 215 } ,
     {"PaleGreen3"         , 114 , 7  , 135 , 215 , 135 , 85  , 94  , 215 } ,
     {"DarkSeaGreen3"      , 115 , 10 , 135 , 215 , 175 , 106 , 94  , 215 } ,
     {"DarkSlateGray3"     , 116 , 14 , 135 , 215 , 215 , 128 , 94  , 215 } ,
     {"SkyBlue1"           , 117 , 14 , 135 , 215 , 255 , 143 , 120 , 255 } ,
     {"Chartreuse1"        , 118 , 11 , 135 , 255 , 0   , 63  , 255 , 255 } ,
     {"LightGreen"         , 119 , 10 , 135 , 255 , 95  , 75  , 160 , 255 } ,
     {"LightGreenBis"      , 120 , 10 , 135 , 255 , 135 , 85  , 120 , 255 } ,
     {"PaleGreen1"         , 121 , 10 , 135 , 255 , 175 , 99  , 120 , 255 } ,
     {"Aquamarine1"        , 122 , 14 , 135 , 255 , 215 , 113 , 120 , 255 } ,
     {"DarkSlateGray1"     , 123 , 14 , 135 , 255 , 255 , 128 , 120 , 255 } ,
     {"Red3"               , 124 , 1  , 175 , 0   , 0   , 0   , 255 , 175 } ,
     {"DeepPink4"          , 125 , 5  , 175 , 0   , 95  , 233 , 255 , 175 } ,
     {"MediumVioletRed"    , 126 , 5  , 175 , 0   , 135 , 223 , 255 , 175 } ,
     {"Magenta3"           , 127 , 5  , 175 , 0   , 175 , 213 , 255 , 175 } ,
     {"DarkViolet"         , 128 , 13 , 175 , 0   , 215 , 206 , 255 , 215 } ,
     {"Purple"             , 129 , 13 , 175 , 0   , 255 , 200 , 255 , 255 } ,
     {"DarkOrange3"        , 130 , 3  , 175 , 95  , 0   , 23  , 255 , 175 } ,
     {"IndianRed"          , 131 , 7  , 175 , 95  , 95  , 0   , 116 , 175 } ,
     {"HotPink3"           , 132 , 5  , 175 , 95  , 135 , 235 , 116 , 175 } ,
     {"MediumOrchid3"      , 133 , 5  , 175 , 95  , 175 , 213 , 116 , 175 } ,
     {"MediumOrchid"       , 134 , 13 , 175 , 95  , 215 , 199 , 142 , 215 } ,
     {"MediumPurple2"      , 135 , 12 , 175 , 95  , 255 , 192 , 160 , 255 } ,
     {"DarkGoldenrod"      , 136 , 3  , 175 , 135 , 0   , 33  , 255 , 175 } ,
     {"LightSalmon3"       , 137 , 7  , 175 , 135 , 95  , 21  , 116 , 175 } ,
     {"RosyBrown"          , 138 , 7  , 175 , 135 , 135 , 0   , 58  , 175 } ,
     {"Grey63"             , 139 , 5  , 175 , 135 , 175 , 213 , 58  , 175 } ,
     {"MediumPurple2Bis"   , 140 , 12 , 175 , 135 , 215 , 192 , 94  , 215 } ,
     {"MediumPurple1"      , 141 , 12 , 175 , 135 , 255 , 185 , 120 , 255 } ,
     {"Gold3"              , 142 , 3  , 175 , 175 , 0   , 43  , 255 , 175 } ,
     {"DarkKhaki"          , 143 , 7  , 175 , 175 , 95  , 43  , 116 , 175 } ,
     {"NavajoWhite3"       , 144 , 7  , 175 , 175 , 135 , 43  , 58  , 175 } ,
     {"Grey69"             , 145 , 7  , 175 , 175 , 175 , 0   , 0   , 175 } ,
     {"LightSteelBlue3"    , 146 , 12 , 175 , 175 , 215 , 171 , 47  , 215 } ,
     {"LightSteelBlue"     , 147 , 12 , 175 , 175 , 255 , 171 , 80  , 255 } ,
     {"Yellow3"            , 148 , 11 , 175 , 215 , 0   , 50  , 255 , 215 } ,
     {"DarkOliveGreen3Ter" , 149 , 11 , 175 , 215 , 95  , 57  , 142 , 215 } ,
     {"DarkSeaGreen3Bis"   , 150 , 7  , 175 , 215 , 135 , 64  , 94  , 215 } ,
     {"DarkSeaGreen2"      , 151 , 7  , 175 , 215 , 175 , 85  , 47  , 215 } ,
     {"LightCyan3"         , 152 , 7  , 175 , 215 , 215 , 128 , 47  , 215 } ,
     {"LightSkyBlue1"      , 153 , 12 , 175 , 215 , 255 , 150 , 80  , 255 } ,
     {"GreenYellow"        , 154 , 11 , 175 , 255 , 0   , 56  , 255 , 255 } ,
     {"DarkOliveGreen2"    , 155 , 10 , 175 , 255 , 95  , 64  , 160 , 255 } ,
     {"PaleGreen1Bis"      , 156 , 10 , 175 , 255 , 135 , 71  , 120 , 255 } ,
     {"DarkSeaGreen2Bis"   , 157 , 15 , 175 , 255 , 175 , 85  , 80  , 255 } ,
     {"DarkSeaGreen1"      , 158 , 15 , 175 , 255 , 215 , 106 , 80  , 255 } ,
     {"PaleTurquoise1"     , 159 , 14 , 175 , 255 , 255 , 128 , 80  , 255 } ,
     {"Red3Bis"            , 160 , 9  , 215 , 0   , 0   , 0   , 255 , 215 } ,
     {"DeepPink3"          , 161 , 13 , 215 , 0   , 95  , 237 , 255 , 215 } ,
     {"DeepPink3Bis"       , 162 , 13 , 215 , 0   , 135 , 229 , 255 , 215 } ,
     {"Magenta3Bis"        , 163 , 13 , 215 , 0   , 175 , 221 , 255 , 215 } ,
     {"Magenta3Ter"        , 164 , 13 , 215 , 0   , 215 , 213 , 255 , 215 } ,
     {"Magenta2"           , 165 , 13 , 215 , 0   , 255 , 207 , 255 , 255 } ,
     {"DarkOrange3Bis"     , 166 , 9  , 215 , 95  , 0   , 19  , 255 , 215 } ,
     {"IndianRedBis"       , 167 , 9  , 215 , 95  , 95  , 0   , 142 , 215 } ,
     {"HotPink3Bis"        , 168 , 13 , 215 , 95  , 135 , 242 , 142 , 215 } ,
     {"HotPink2"           , 169 , 13 , 215 , 95  , 175 , 228 , 142 , 215 } ,
     {"Orchid"             , 170 , 13 , 215 , 95  , 215 , 213 , 142 , 215 } ,
     {"MediumOrchid1"      , 171 , 13 , 215 , 95  , 255 , 203 , 160 , 255 } ,
     {"Orange3"            , 172 , 11 , 215 , 135 , 0   , 27  , 255 , 215 } ,
     {"LightSalmon3"       , 173 , 9  , 215 , 135 , 95  , 14  , 142 , 215 } ,
     {"LightPink3"         , 174 , 7  , 215 , 135 , 135 , 0   , 94  , 215 } ,
     {"Pink3"              , 175 , 13 , 215 , 135 , 175 , 235 , 94  , 215 } ,
     {"Plum3"              , 176 , 13 , 215 , 135 , 215 , 213 , 94  , 215 } ,
     {"Violet"             , 177 , 13 , 215 , 135 , 255 , 199 , 120 , 255 } ,
     {"Gold3Bis"           , 178 , 11 , 215 , 175 , 0   , 35  , 255 , 215 } ,
     {"LightGoldenrod3"    , 179 , 11 , 215 , 175 , 95  , 28  , 142 , 215 } ,
     {"Tan"                , 180 , 7  , 215 , 175 , 135 , 21  , 94  , 215 } ,
     {"MistyRose3"         , 181 , 7  , 215 , 175 , 175 , 0   , 47  , 215 } ,
     {"Thistle3"           , 182 , 13 , 215 , 175 , 215 , 213 , 47  , 215 } ,
     {"Plum2"              , 183 , 12 , 215 , 175 , 255 , 192 , 80  , 255 } ,
     {"Yellow3Bis"         , 184 , 11 , 215 , 215 , 0   , 43  , 255 , 215 } ,
     {"Khaki3"             , 185 , 11 , 215 , 215 , 95  , 43  , 142 , 215 } ,
     {"LightGoldenrod2"    , 186 , 7  , 215 , 215 , 135 , 43  , 94  , 215 } ,
     {"LightYellow3"       , 187 , 7  , 215 , 215 , 175 , 43  , 47  , 215 } ,
     {"Grey84"             , 188 , 7  , 215 , 215 , 215 , 0   , 0   , 215 } ,
     {"LightSteelBlue1"    , 189 , 12 , 215 , 215 , 255 , 171 , 40  , 255 } ,
     {"Yellow2"            , 190 , 11 , 215 , 255 , 0   , 49  , 255 , 255 } ,
     {"DarkOliveGreen1"    , 191 , 11 , 215 , 255 , 95  , 53  , 160 , 255 } ,
     {"DarkOliveGreen1Bis" , 192 , 11 , 215 , 255 , 135 , 57  , 120 , 255 } ,
     {"DarkSeaGreen1Bis"   , 193 , 15 , 215 , 255 , 175 , 64  , 80  , 255 } ,
     {"Honeydew2"          , 194 , 15 , 215 , 255 , 215 , 85  , 40  , 255 } ,
     {"LightCyan1Bis"      , 195 , 15 , 215 , 255 , 255 , 128 , 40  , 255 } ,
     {"Red1"               , 196 , 9  , 255 , 0   , 0   , 0   , 255 , 255 } ,
     {"DeepPink2"          , 197 , 13 , 255 , 0   , 95  , 240 , 255 , 255 } ,
     {"DeepPink1"          , 198 , 13 , 255 , 0   , 135 , 234 , 255 , 255 } ,
     {"DeepPink1Bis"       , 199 , 13 , 255 , 0   , 175 , 227 , 255 , 255 } ,
     {"Magenta2Bis"        , 200 , 13 , 255 , 0   , 215 , 220 , 255 , 255 } ,
     {"Magenta1"           , 201 , 13 , 255 , 0   , 255 , 213 , 255 , 255 } ,
     {"OrangeRed1"         , 202 , 9  , 255 , 95  , 0   , 16  , 255 , 255 } ,
     {"IndianRed1"         , 203 , 9  , 255 , 95  , 95  , 0   , 160 , 255 } ,
     {"IndianRed1Bis"      , 204 , 13 , 255 , 95  , 135 , 246 , 160 , 255 } ,
     {"HotPink"            , 205 , 13 , 255 , 95  , 175 , 235 , 160 , 255 } ,
     {"HotPinkBis"         , 206 , 13 , 255 , 95  , 215 , 224 , 160 , 255 } ,
     {"MediumOrchid1Bis"   , 207 , 13 , 255 , 95  , 255 , 213 , 160 , 255 } ,
     {"DarkOrange"         , 208 , 11 , 255 , 135 , 0   , 22  , 255 , 255 } ,
     {"Salmon1"            , 209 , 9  , 255 , 135 , 95  , 10  , 160 , 255 } ,
     {"LightCoral"         , 210 , 15 , 255 , 135 , 135 , 0   , 120 , 255 } ,
     {"PaleVioletRed1"     , 211 , 13 , 255 , 135 , 175 , 242 , 120 , 255 } ,
     {"Orchid2"            , 212 , 13 , 255 , 135 , 215 , 228 , 120 , 255 } ,
     {"Orchid1"            , 213 , 13 , 255 , 135 , 255 , 213 , 120 , 255 } ,
     {"Orange1"            , 214 , 11 , 255 , 175 , 0   , 29  , 255 , 255 } ,
     {"SandyBrown"         , 215 , 9  , 255 , 175 , 95  , 21  , 160 , 255 } ,
     {"LightSalmon1"       , 216 , 15 , 255 , 175 , 135 , 14  , 120 , 255 } ,
     {"LightPink1"         , 217 , 15 , 255 , 175 , 175 , 0   , 80  , 255 } ,
     {"Pink1"              , 218 , 13 , 255 , 175 , 215 , 235 , 80  , 255 } ,
     {"Plum1"              , 219 , 13 , 255 , 175 , 255 , 213 , 80  , 255 } ,
     {"Gold1"              , 220 , 11 , 255 , 215 , 0   , 36  , 255 , 255 } ,
     {"LightGoldenrod2Bis" , 221 , 11 , 255 , 215 , 95  , 32  , 160 , 255 } ,
     {"LightGoldenrod2Ter" , 222 , 15 , 255 , 215 , 135 , 28  , 120 , 255 } ,
     {"NavajoWhite1"       , 223 , 15 , 255 , 215 , 175 , 21  , 80  , 255 } ,
     {"MistyRose1"         , 224 , 15 , 255 , 215 , 215 , 0   , 40  , 255 } ,
     {"Thistle1"           , 225 , 13 , 255 , 215 , 255 , 213 , 40  , 255 } ,
     {"Yellow1"            , 226 , 11 , 255 , 255 , 0   , 43  , 255 , 255 } ,
     {"LightGoldenrod1"    , 227 , 11 , 255 , 255 , 95  , 43  , 160 , 255 } ,
     {"Khaki1"             , 228 , 15 , 255 , 255 , 135 , 43  , 120 , 255 } ,
     {"Wheat1"             , 229 , 15 , 255 , 255 , 175 , 43  , 80  , 255 } ,
     {"Cornsilk1"          , 230 , 15 , 255 , 255 , 215 , 43  , 40  , 255 } ,
     {"Grey100"            , 231 , 15 , 255 , 255 , 255 , 0   , 0   , 255 } ,
     {"Grey3"              , 232 , 0  , 8   , 8   , 8   , 0   , 0   , 8   } ,
     {"Grey7"              , 233 , 0  , 18  , 18  , 18  , 0   , 0   , 18  } ,
     {"Grey11"             , 234 , 0  , 28  , 28  , 28  , 0   , 0   , 28  } ,
     {"Grey15"             , 235 , 0  , 38  , 38  , 38  , 0   , 0   , 38  } ,
     {"Grey19"             , 236 , 0  , 48  , 48  , 48  , 0   , 0   , 48  } ,
     {"Grey23"             , 237 , 0  , 58  , 58  , 58  , 0   , 0   , 58  } ,
     {"Grey27"             , 238 , 8  , 68  , 68  , 68  , 0   , 0   , 68  } ,
     {"Grey30"             , 239 , 8  , 78  , 78  , 78  , 0   , 0   , 78  } ,
     {"Grey35"             , 240 , 8  , 88  , 88  , 88  , 0   , 0   , 88  } ,
     {"Grey39"             , 241 , 8  , 98  , 98  , 98  , 0   , 0   , 98  } ,
     {"Grey42"             , 242 , 8  , 108 , 108 , 108 , 0   , 0   , 108 } ,
     {"Grey46"             , 243 , 8  , 118 , 118 , 118 , 0   , 0   , 118 } ,
     {"Grey50"             , 244 , 8  , 128 , 128 , 128 , 0   , 0   , 128 } ,
     {"Grey54"             , 245 , 8  , 138 , 138 , 138 , 0   , 0   , 138 } ,
     {"Grey58"             , 246 , 8  , 148 , 148 , 148 , 0   , 0   , 148 } ,
     {"Grey62"             , 247 , 8  , 158 , 158 , 158 , 0   , 0   , 158 } ,
     {"Grey66"             , 248 , 7  , 168 , 168 , 168 , 0   , 0   , 168 } ,
     {"Grey70"             , 249 , 7  , 178 , 178 , 178 , 0   , 0   , 178 } ,
     {"Grey74"             , 250 , 7  , 188 , 188 , 188 , 0   , 0   , 188 } ,
     {"Grey78"             , 251 , 7  , 198 , 198 , 198 , 0   , 0   , 198 } ,
     {"Grey82"             , 252 , 7  , 208 , 208 , 208 , 0   , 0   , 208 } ,
     {"Grey85"             , 253 , 7  , 218 , 218 , 218 , 0   , 0   , 218 } ,
     {"Grey89"             , 254 , 15 , 228 , 228 , 228 , 0   , 0   , 228 } ,
     {"Grey93"             , 255 , 15 , 238 , 238 , 238 , 0   , 0   , 238 } ,
}};

ColorInfo GetColorInfo(Color::Palette256 index) {
  return palette256[index]; // NOLINT;
}

ColorInfo GetColorInfo(Color::Palette16 index) {
  return palette256[index]; // NOLINT;
}
// clang-format off

}

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <cstdint>  // for uint8_t
#include <iostream>  // for operator<<, stringstream, basic_ostream, flush, cout, ostream
#include <map>      // for _Rb_tree_const_iterator, map, operator!=, operator==
#include <memory>   // for allocator
#include <sstream>  // IWYU pragma: keep
#include <utility>  // for pair

#ifndef FTXUI_SCREEN_SCREEN_HPP
#define FTXUI_SCREEN_SCREEN_HPP

#include <memory>
#include <string>  // for string, allocator, basic_string
#include <vector>  // for vector

namespace ftxui {

/// @brief A unicode character and its associated style.
/// @ingroup screen
struct Pixel {
  bool operator==(const Pixel& other) const;

  // The graphemes stored into the pixel. To support combining characters,
  // like: a⃦, this can potentially contains multiple codepoitns.
  std::string character = " ";

  // Colors:
  Color background_color = Color::Default;
  Color foreground_color = Color::Default;

  // A bit field representing the style:
  bool blink : 1;
  bool bold : 1;
  bool dim : 1;
  bool inverted : 1;
  bool underlined : 1;
  bool underlined_double : 1;
  bool strikethrough : 1;
  bool automerge : 1;

  Pixel()
      : blink(false),
        bold(false),
        dim(false),
        inverted(false),
        underlined(false),
        underlined_double(false),
        strikethrough(false),
        automerge(false) {}
};

/// @brief Define how the Screen's dimensions should look like.
/// @ingroup screen
namespace Dimension {
Dimensions Fixed(int);
Dimensions Full();
}  // namespace Dimension

/// @brief A rectangular grid of Pixel.
/// @ingroup screen
class Screen {
 public:
  // Constructors:
  Screen(int dimx, int dimy);
  static Screen Create(Dimensions dimension);
  static Screen Create(Dimensions width, Dimensions height);

  // Node write into the screen using Screen::at.
  std::string& at(int x, int y);
  Pixel& PixelAt(int x, int y);

  // Convert the screen into a printable string in the terminal.
  std::string ToString();
  void Print();

  // Get screen dimensions.
  int dimx() const { return dimx_; }
  int dimy() const { return dimy_; }

  // Move the terminal cursor n-lines up with n = dimy().
  std::string ResetPosition(bool clear = false) const;

  // Fill with space.
  void Clear();

  void ApplyShader();

  struct Cursor {
    int x = 0;
    int y = 0;

    enum Shape {
      Hidden = 0,
      BlockBlinking = 1,
      Block = 2,
      UnderlineBlinking = 3,
      Underline = 4,
      BarBlinking = 5,
      Bar = 6,
    };
    Shape shape;

    Cursor() = default;
    Cursor(int cx, int cy, Shape cshape) : x(cx), y(cy), shape(cshape) {}
  };
  Cursor cursor() const { return cursor_; }
  void SetCursor(Cursor cursor) { cursor_ = cursor; }

  Box stencil;

 protected:
  int dimx_;
  int dimy_;
  std::vector<std::vector<Pixel>> pixels_;
  Cursor cursor_;
};

}  // namespace ftxui

#endif  // FTXUI_SCREEN_SCREEN_HPP

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#ifndef FTXUI_SCREEN_STRING_HPP
#define FTXUI_SCREEN_STRING_HPP

#include <stddef.h>  // for size_t
#include <string>    // for string, wstring, to_string
#include <vector>    // for vector

namespace ftxui {
std::string to_string(const std::wstring& s);
std::wstring to_wstring(const std::string& s);

template <typename T>
std::wstring to_wstring(T s) {
  return to_wstring(std::to_string(s));
}

int string_width(const std::string&);
// Split the string into a its glyphs. An empty one is inserted ater fullwidth
// ones.
std::vector<std::string> Utf8ToGlyphs(const std::string& input);
// If |input| was an array of glyphs, this returns the number of char to eat
// before reaching the glyph at index |glyph_index|.
int GlyphPosition(const std::string& input,
                  size_t glyph_index,
                  size_t start = 0);
// Returns the number of glyphs in |input|.
int GlyphCount(const std::string& input);

// Properties from:
// https://www.unicode.org/Public/UCD/latest/ucd/auxiliary/WordBreakProperty.txt
enum class WordBreakProperty {
  ALetter,
  CR,
  Double_Quote,
  Extend,
  ExtendNumLet,
  Format,
  Hebrew_Letter,
  Katakana,
  LF,
  MidLetter,
  MidNum,
  MidNumLet,
  Newline,
  Numeric,
  Regional_Indicator,
  Single_Quote,
  WSegSpace,
  ZWJ,
};
std::vector<WordBreakProperty> Utf8ToWordBreakProperty(
    const std::string& input);

bool IsWordBreakingCharacter(const std::string& input, size_t glyph_index);

// Map every cells drawn by |input| to their corresponding Glyphs. Half-size
// Glyphs takes one cell, full-size Glyphs take two cells.
std::vector<int> CellToGlyphIndex(const std::string& input);

}  // namespace ftxui

#ifndef FTXUI_SCREEN_DEPRECATED_HPP
#define FTXUI_SCREEN_DEPRECATED_HPP

#include <string>

namespace ftxui {
int wchar_width(wchar_t);
int wstring_width(const std::wstring&);
}  // namespace ftxui

#endif  // FTXUI_SCREEN_DEPRECATED_HPP

// Copyright 2021 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

#endif /* end of include guard: FTXUI_SCREEN_STRING_HPP */

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ftxui {

namespace {

Pixel& dev_null_pixel() {
  static Pixel pixel;
  return pixel;
}

#if defined(_WIN32)
void WindowsEmulateVT100Terminal() {
  static bool done = false;
  if (done)
    return;
  done = true;

  // Enable VT processing on stdout and stdin
  auto stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);

  DWORD out_mode = 0;
  GetConsoleMode(stdout_handle, &out_mode);

  // https://docs.microsoft.com/en-us/windows/console/setconsolemode
  const int enable_virtual_terminal_processing = 0x0004;
  const int disable_newline_auto_return = 0x0008;
  out_mode |= enable_virtual_terminal_processing;
  out_mode |= disable_newline_auto_return;

  SetConsoleMode(stdout_handle, out_mode);
}
#endif

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void UpdatePixelStyle(std::stringstream& ss,
                      Pixel& previous,
                      const Pixel& next) {
  if (next == previous) {
    return;
  }

  if ((!next.bold && previous.bold) ||  //
      (!next.dim && previous.dim)) {
    ss << "\x1B[22m";  // BOLD_RESET and DIM_RESET
    // We might have wrongfully reset dim or bold because they share the same
    // resetter. Take it into account so that the side effect will cause it to
    // be set again below.
    previous.bold = false;
    previous.dim = false;
  }

  if ((!next.underlined && previous.underlined) ||
      (!next.underlined_double && previous.underlined_double)) {
    // We might have wrongfully reset underlined or underlinedbold because they
    // share the same resetter. Take it into account so that the side effect
    // will cause it to be set again below.
    ss << "\x1B[24m";  // UNDERLINED_RESET
    previous.underlined = false;
    previous.underlined_double = false;
  }

  if (next.bold && !previous.bold) {
    ss << "\x1B[1m";  // BOLD_SET
  }

  if (next.dim && !previous.dim) {
    ss << "\x1B[2m";  // DIM_SET
  }

  if (next.underlined && !previous.underlined) {
    ss << "\x1B[4m";  // UNDERLINED_SET
  }

  if (next.blink && !previous.blink) {
    ss << "\x1B[5m";  // BLINK_SET
  }

  if (!next.blink && previous.blink) {
    ss << "\x1B[25m";  // BLINK_RESET
  }

  if (next.inverted && !previous.inverted) {
    ss << "\x1B[7m";  // INVERTED_SET
  }

  if (!next.inverted && previous.inverted) {
    ss << "\x1B[27m";  // INVERTED_RESET
  }

  if (next.strikethrough && !previous.strikethrough) {
    ss << "\x1B[9m";  // CROSSED_OUT
  }

  if (!next.strikethrough && previous.strikethrough) {
    ss << "\x1B[29m";  // CROSSED_OUT_RESET
  }

  if (next.underlined_double && !previous.underlined_double) {
    ss << "\x1B[21m";  // DOUBLE_UNDERLINED_SET
  }

  if (next.foreground_color != previous.foreground_color ||
      next.background_color != previous.background_color) {
    ss << "\x1B[" + next.foreground_color.Print(false) + "m";
    ss << "\x1B[" + next.background_color.Print(true) + "m";
  }

  previous = next;
}

struct TileEncoding {
  uint8_t left : 2;
  uint8_t top : 2;
  uint8_t right : 2;
  uint8_t down : 2;
  uint8_t round : 1;

  // clang-format off
  bool operator<(const TileEncoding& other) const {
    if (left < other.left)   { return  true;  }
    if (left > other.left)   { return  false; }
    if (top < other.top)     { return  true;  }
    if (top > other.top)     { return  false; }
    if (right < other.right) { return  true;  }
    if (right > other.right) { return  false; }
    if (down < other.down)   { return  true;  }
    if (down > other.down)   { return  false; }
    if (round < other.round) { return  true;  }
    if (round > other.round) { return  false; }
      return false;
  }
  // clang-format on
};

// clang-format off
const std::map<std::string, TileEncoding> tile_encoding = { // NOLINT
    {"─", {1, 0, 1, 0, 0}},
    {"━", {2, 0, 2, 0, 0}},
    {"╍", {2, 0, 2, 0, 0}},

    {"│", {0, 1, 0, 1, 0}},
    {"┃", {0, 2, 0, 2, 0}},
    {"╏", {0, 2, 0, 2, 0}},

    {"┌", {0, 0, 1, 1, 0}},
    {"┍", {0, 0, 2, 1, 0}},
    {"┎", {0, 0, 1, 2, 0}},
    {"┏", {0, 0, 2, 2, 0}},

    {"┐", {1, 0, 0, 1, 0}},
    {"┑", {2, 0, 0, 1, 0}},
    {"┒", {1, 0, 0, 2, 0}},
    {"┓", {2, 0, 0, 2, 0}},

    {"└", {0, 1, 1, 0, 0}},
    {"┕", {0, 1, 2, 0, 0}},
    {"┖", {0, 2, 1, 0, 0}},
    {"┗", {0, 2, 2, 0, 0}},

    {"┘", {1, 1, 0, 0, 0}},
    {"┙", {2, 1, 0, 0, 0}},
    {"┚", {1, 2, 0, 0, 0}},
    {"┛", {2, 2, 0, 0, 0}},

    {"├", {0, 1, 1, 1, 0}},
    {"┝", {0, 1, 2, 1, 0}},
    {"┞", {0, 2, 1, 1, 0}},
    {"┟", {0, 1, 1, 2, 0}},
    {"┠", {0, 2, 1, 2, 0}},
    {"┡", {0, 2, 2, 1, 0}},
    {"┢", {0, 1, 2, 2, 0}},
    {"┣", {0, 2, 2, 2, 0}},

    {"┤", {1, 1, 0, 1, 0}},
    {"┥", {2, 1, 0, 1, 0}},
    {"┦", {1, 2, 0, 1, 0}},
    {"┧", {1, 1, 0, 2, 0}},
    {"┨", {1, 2, 0, 2, 0}},
    {"┩", {2, 2, 0, 1, 0}},
    {"┪", {2, 1, 0, 2, 0}},
    {"┫", {2, 2, 0, 2, 0}},

    {"┬", {1, 0, 1, 1, 0}},
    {"┭", {2, 0, 1, 1, 0}},
    {"┮", {1, 0, 2, 1, 0}},
    {"┯", {2, 0, 2, 1, 0}},
    {"┰", {1, 0, 1, 2, 0}},
    {"┱", {2, 0, 1, 2, 0}},
    {"┲", {1, 0, 2, 2, 0}},
    {"┳", {2, 0, 2, 2, 0}},

    {"┴", {1, 1, 1, 0, 0}},
    {"┵", {2, 1, 1, 0, 0}},
    {"┶", {1, 1, 2, 0, 0}},
    {"┷", {2, 1, 2, 0, 0}},
    {"┸", {1, 2, 1, 0, 0}},
    {"┹", {2, 2, 1, 0, 0}},
    {"┺", {1, 2, 2, 0, 0}},
    {"┻", {2, 2, 2, 0, 0}},

    {"┼", {1, 1, 1, 1, 0}},
    {"┽", {2, 1, 1, 1, 0}},
    {"┾", {1, 1, 2, 1, 0}},
    {"┿", {2, 1, 2, 1, 0}},
    {"╀", {1, 2, 1, 1, 0}},
    {"╁", {1, 1, 1, 2, 0}},
    {"╂", {1, 2, 1, 2, 0}},
    {"╃", {2, 2, 1, 1, 0}},
    {"╄", {1, 2, 2, 1, 0}},
    {"╅", {2, 1, 1, 2, 0}},
    {"╆", {1, 1, 2, 2, 0}},
    {"╇", {2, 2, 2, 1, 0}},
    {"╈", {2, 1, 2, 2, 0}},
    {"╉", {2, 2, 1, 2, 0}},
    {"╊", {1, 2, 2, 2, 0}},
    {"╋", {2, 2, 2, 2, 0}},

    {"═", {3, 0, 3, 0, 0}},
    {"║", {0, 3, 0, 3, 0}},

    {"╒", {0, 0, 3, 1, 0}},
    {"╓", {0, 0, 1, 3, 0}},
    {"╔", {0, 0, 3, 3, 0}},

    {"╕", {3, 0, 0, 1, 0}},
    {"╖", {1, 0, 0, 3, 0}},
    {"╗", {3, 0, 0, 3, 0}},

    {"╘", {0, 1, 3, 0, 0}},
    {"╙", {0, 3, 1, 0, 0}},
    {"╚", {0, 3, 3, 0, 0}},

    {"╛", {3, 1, 0, 0, 0}},
    {"╜", {1, 3, 0, 0, 0}},
    {"╝", {3, 3, 0, 0, 0}},

    {"╞", {0, 1, 3, 1, 0}},
    {"╟", {0, 3, 1, 3, 0}},
    {"╠", {0, 3, 3, 3, 0}},

    {"╡", {3, 1, 0, 1, 0}},
    {"╢", {1, 3, 0, 3, 0}},
    {"╣", {3, 3, 0, 3, 0}},

    {"╤", {3, 0, 3, 1, 0}},
    {"╥", {1, 0, 1, 3, 0}},
    {"╦", {3, 0, 3, 3, 0}},

    {"╧", {3, 1, 3, 0, 0}},
    {"╨", {1, 3, 1, 0, 0}},
    {"╩", {3, 3, 3, 0, 0}},

    {"╪", {3, 1, 3, 1, 0}},
    {"╫", {1, 3, 1, 3, 0}},
    {"╬", {3, 3, 3, 3, 0}},

    {"╭", {0, 0, 1, 1, 1}},
    {"╮", {1, 0, 0, 1, 1}},
    {"╯", {1, 1, 0, 0, 1}},
    {"╰", {0, 1, 1, 0, 1}},

    {"╴", {1, 0, 0, 0, 0}},
    {"╵", {0, 1, 0, 0, 0}},
    {"╶", {0, 0, 1, 0, 0}},
    {"╷", {0, 0, 0, 1, 0}},

    {"╸", {2, 0, 0, 0, 0}},
    {"╹", {0, 2, 0, 0, 0}},
    {"╺", {0, 0, 2, 0, 0}},
    {"╻", {0, 0, 0, 2, 0}},

    {"╼", {1, 0, 2, 0, 0}},
    {"╽", {0, 1, 0, 2, 0}},
    {"╾", {2, 0, 1, 0, 0}},
    {"╿", {0, 2, 0, 1, 0}},
};
// clang-format on

template <class A, class B>
std::map<B, A> InvertMap(const std::map<A, B> input) {
  std::map<B, A> output;
  for (const auto& it : input) {
    output[it.second] = it.first;
  }
  return output;
}

const std::map<TileEncoding, std::string> tile_encoding_inverse =  // NOLINT
    InvertMap(tile_encoding);

void UpgradeLeftRight(std::string& left, std::string& right) {
  const auto it_left = tile_encoding.find(left);
  if (it_left == tile_encoding.end()) {
    return;
  }
  const auto it_right = tile_encoding.find(right);
  if (it_right == tile_encoding.end()) {
    return;
  }

  if (it_left->second.right == 0 && it_right->second.left != 0) {
    TileEncoding encoding_left = it_left->second;
    encoding_left.right = it_right->second.left;
    const auto it_left_upgrade = tile_encoding_inverse.find(encoding_left);
    if (it_left_upgrade != tile_encoding_inverse.end()) {
      left = it_left_upgrade->second;
    }
  }

  if (it_right->second.left == 0 && it_left->second.right != 0) {
    TileEncoding encoding_right = it_right->second;
    encoding_right.left = it_left->second.right;
    const auto it_right_upgrade = tile_encoding_inverse.find(encoding_right);
    if (it_right_upgrade != tile_encoding_inverse.end()) {
      right = it_right_upgrade->second;
    }
  }
}

void UpgradeTopDown(std::string& top, std::string& down) {
  const auto it_top = tile_encoding.find(top);
  if (it_top == tile_encoding.end()) {
    return;
  }
  const auto it_down = tile_encoding.find(down);
  if (it_down == tile_encoding.end()) {
    return;
  }

  if (it_top->second.down == 0 && it_down->second.top != 0) {
    TileEncoding encoding_top = it_top->second;
    encoding_top.down = it_down->second.top;
    const auto it_top_down = tile_encoding_inverse.find(encoding_top);
    if (it_top_down != tile_encoding_inverse.end()) {
      top = it_top_down->second;
    }
  }

  if (it_down->second.top == 0 && it_top->second.down != 0) {
    TileEncoding encoding_down = it_down->second;
    encoding_down.top = it_top->second.down;
    const auto it_down_top = tile_encoding_inverse.find(encoding_down);
    if (it_down_top != tile_encoding_inverse.end()) {
      down = it_down_top->second;
    }
  }
}

bool ShouldAttemptAutoMerge(Pixel& pixel) {
  return pixel.automerge && pixel.character.size() == 3;
}

}  // namespace

bool Pixel::operator==(const Pixel& other) const {
  return character == other.character &&                //
         background_color == other.background_color &&  //
         foreground_color == other.foreground_color &&  //
         blink == other.blink &&                        //
         bold == other.bold &&                          //
         dim == other.dim &&                            //
         inverted == other.inverted &&                  //
         underlined == other.underlined &&              //
         automerge == other.automerge;                  //
}

/// A fixed dimension.
/// @see Fit
/// @see Full
Dimensions Dimension::Fixed(int v) {
  return {v, v};
}

/// Use the terminal dimensions.
/// @see Fixed
/// @see Fit
Dimensions Dimension::Full() {
  return Terminal::Size();
}

// static
/// Create a screen with the given dimension along the x-axis and y-axis.
Screen Screen::Create(Dimensions width, Dimensions height) {
  return {width.dimx, height.dimy};
}

// static
/// Create a screen with the given dimension.
Screen Screen::Create(Dimensions dimension) {
  return {dimension.dimx, dimension.dimy};
}

Screen::Screen(int dimx, int dimy)
    : stencil{0, dimx - 1, 0, dimy - 1},
      dimx_(dimx),
      dimy_(dimy),
      pixels_(dimy, std::vector<Pixel>(dimx)) {
#if defined(_WIN32)
  // The placement of this call is a bit weird, however we can assume that
  // anybody who instantiates a Screen object eventually wants to output
  // something to the console.
  // As we require UTF8 for all input/output operations we will just switch to
  // UTF8 encoding here
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
  WindowsEmulateVT100Terminal();
#endif
}

/// Produce a std::string that can be used to print the Screen on the terminal.
/// Don't forget to flush stdout. Alternatively, you can use Screen::Print();
std::string Screen::ToString() {
  std::stringstream ss;

  Pixel previous_pixel;
  const Pixel final_pixel;

  for (int y = 0; y < dimy_; ++y) {
    if (y != 0) {
      UpdatePixelStyle(ss, previous_pixel, final_pixel);
      ss << "\r\n";
    }
    bool previous_fullwidth = false;
    for (const auto& pixel : pixels_[y]) {
      if (!previous_fullwidth) {
        UpdatePixelStyle(ss, previous_pixel, pixel);
        ss << pixel.character;
      }
      previous_fullwidth = (string_width(pixel.character) == 2);
    }
  }

  UpdatePixelStyle(ss, previous_pixel, final_pixel);

  return ss.str();
}

void Screen::Print() {
  std::cout << ToString() << '\0' << std::flush;
}

/// @brief Access a character a given position.
/// @param x The character position along the x-axis.
/// @param y The character position along the y-axis.
std::string& Screen::at(int x, int y) {
  return PixelAt(x, y).character;
}

/// @brief Access a Pixel at a given position.
/// @param x The pixel position along the x-axis.
/// @param y The pixel position along the y-axis.
Pixel& Screen::PixelAt(int x, int y) {
  return stencil.Contain(x, y) ? pixels_[y][x] : dev_null_pixel();
}

/// @brief Return a string to be printed in order to reset the cursor position
///        to the beginning of the screen.
///
/// ```cpp
/// std::string reset_position;
/// while(true) {
///   auto document = render();
///   auto screen = Screen::Create(Dimension::Full(), Dimension::Fit(document));
///   Render(screen, document);
///   std::cout << reset_position << screen.ToString() << std::flush;
///   reset_position = screen.ResetPosition();
///
///   using namespace std::chrono_literals;
///   std::this_thread::sleep_for(0.01s);
/// }
/// ```
///
/// @return The string to print in order to reset the cursor position to the
///         beginning.
std::string Screen::ResetPosition(bool clear) const {
  std::stringstream ss;
  if (clear) {
    ss << "\r";       // MOVE_LEFT;
    ss << "\x1b[2K";  // CLEAR_SCREEN;
    for (int y = 1; y < dimy_; ++y) {
      ss << "\x1B[1A";  // MOVE_UP;
      ss << "\x1B[2K";  // CLEAR_LINE;
    }
  } else {
    ss << "\r";  // MOVE_LEFT;
    for (int y = 1; y < dimy_; ++y) {
      ss << "\x1B[1A";  // MOVE_UP;
    }
  }
  return ss.str();
}

/// @brief Clear all the pixel from the screen.
void Screen::Clear() {
  for (auto& line : pixels_) {
    for (auto& cell : line) {
      cell = Pixel();
    }
  }
  cursor_.x = dimx_ - 1;
  cursor_.y = dimy_ - 1;
}

// clang-format off
void Screen::ApplyShader() {
  // Merge box characters togethers.
  for (int y = 0; y < dimy_; ++y) {
    for (int x = 0; x < dimx_; ++x) {
      // Box drawing character uses exactly 3 byte.
      Pixel& cur = pixels_[y][x];
      if (!ShouldAttemptAutoMerge(cur)) {
        continue;
      }

      if (x > 0) {
        Pixel& left = pixels_[y][x-1];
        if (ShouldAttemptAutoMerge(left)) {
          UpgradeLeftRight(left.character, cur.character);
        }
      }
      if (y > 0) {
        Pixel& top = pixels_[y-1][x];
        if (ShouldAttemptAutoMerge(top)) {
          UpgradeTopDown(top.character, cur.character);
        }
      }
    }
  }
}

// clang-format on

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
// Content of this file was created thanks to:
// -
// https://www.unicode.org/Public/UCD/latest/ucd/auxiliary/WordBreakProperty.txt
// - Markus Kuhn -- 2007-05-26 (Unicode 5.0)
//   http://www.cl.cam.ac.uk/~mgk25/ucs/wcwidth.c
// Thanks you!

#include <array>    // for array
#include <cstdint>  // for uint32_t, uint8_t, uint16_t, int32_t
#include <string>   // for string, basic_string, wstring
#include <tuple>    // for _Swallow_assign, ignore

namespace {

struct Interval {
  uint32_t first;
  uint32_t last;
};

// Sorted list of non-overlapping intervals of non-spacing characters
// generated by "uniset +cat=Me +cat=Mn +cat=Cf -00AD +1160-11FF +200B c"
// As of Unicode 13.0.0
const std::array<Interval, 324> g_combining_characters = {{
    {0x00300, 0x0036f}, {0x00483, 0x00489}, {0x00591, 0x005bd},
    {0x005bf, 0x005bf}, {0x005c1, 0x005c2}, {0x005c4, 0x005c5},
    {0x005c7, 0x005c7}, {0x00610, 0x0061a}, {0x0064b, 0x0065f},
    {0x00670, 0x00670}, {0x006d6, 0x006dc}, {0x006df, 0x006e4},
    {0x006e7, 0x006e8}, {0x006ea, 0x006ed}, {0x00711, 0x00711},
    {0x00730, 0x0074a}, {0x007a6, 0x007b0}, {0x007eb, 0x007f3},
    {0x007fd, 0x007fd}, {0x00816, 0x00819}, {0x0081b, 0x00823},
    {0x00825, 0x00827}, {0x00829, 0x0082d}, {0x00859, 0x0085b},
    {0x008d3, 0x008e1}, {0x008e3, 0x00902}, {0x0093a, 0x0093a},
    {0x0093c, 0x0093c}, {0x00941, 0x00948}, {0x0094d, 0x0094d},
    {0x00951, 0x00957}, {0x00962, 0x00963}, {0x00981, 0x00981},
    {0x009bc, 0x009bc}, {0x009c1, 0x009c4}, {0x009cd, 0x009cd},
    {0x009e2, 0x009e3}, {0x009fe, 0x009fe}, {0x00a01, 0x00a02},
    {0x00a3c, 0x00a3c}, {0x00a41, 0x00a42}, {0x00a47, 0x00a48},
    {0x00a4b, 0x00a4d}, {0x00a51, 0x00a51}, {0x00a70, 0x00a71},
    {0x00a75, 0x00a75}, {0x00a81, 0x00a82}, {0x00abc, 0x00abc},
    {0x00ac1, 0x00ac5}, {0x00ac7, 0x00ac8}, {0x00acd, 0x00acd},
    {0x00ae2, 0x00ae3}, {0x00afa, 0x00aff}, {0x00b01, 0x00b01},
    {0x00b3c, 0x00b3c}, {0x00b3f, 0x00b3f}, {0x00b41, 0x00b44},
    {0x00b4d, 0x00b4d}, {0x00b55, 0x00b56}, {0x00b62, 0x00b63},
    {0x00b82, 0x00b82}, {0x00bc0, 0x00bc0}, {0x00bcd, 0x00bcd},
    {0x00c00, 0x00c00}, {0x00c04, 0x00c04}, {0x00c3e, 0x00c40},
    {0x00c46, 0x00c48}, {0x00c4a, 0x00c4d}, {0x00c55, 0x00c56},
    {0x00c62, 0x00c63}, {0x00c81, 0x00c81}, {0x00cbc, 0x00cbc},
    {0x00cbf, 0x00cbf}, {0x00cc6, 0x00cc6}, {0x00ccc, 0x00ccd},
    {0x00ce2, 0x00ce3}, {0x00d00, 0x00d01}, {0x00d3b, 0x00d3c},
    {0x00d41, 0x00d44}, {0x00d4d, 0x00d4d}, {0x00d62, 0x00d63},
    {0x00d81, 0x00d81}, {0x00dca, 0x00dca}, {0x00dd2, 0x00dd4},
    {0x00dd6, 0x00dd6}, {0x00e31, 0x00e31}, {0x00e34, 0x00e3a},
    {0x00e47, 0x00e4e}, {0x00eb1, 0x00eb1}, {0x00eb4, 0x00ebc},
    {0x00ec8, 0x00ecd}, {0x00f18, 0x00f19}, {0x00f35, 0x00f35},
    {0x00f37, 0x00f37}, {0x00f39, 0x00f39}, {0x00f71, 0x00f7e},
    {0x00f80, 0x00f84}, {0x00f86, 0x00f87}, {0x00f8d, 0x00f97},
    {0x00f99, 0x00fbc}, {0x00fc6, 0x00fc6}, {0x0102d, 0x01030},
    {0x01032, 0x01037}, {0x01039, 0x0103a}, {0x0103d, 0x0103e},
    {0x01058, 0x01059}, {0x0105e, 0x01060}, {0x01071, 0x01074},
    {0x01082, 0x01082}, {0x01085, 0x01086}, {0x0108d, 0x0108d},
    {0x0109d, 0x0109d}, {0x0135d, 0x0135f}, {0x01712, 0x01714},
    {0x01732, 0x01734}, {0x01752, 0x01753}, {0x01772, 0x01773},
    {0x017b4, 0x017b5}, {0x017b7, 0x017bd}, {0x017c6, 0x017c6},
    {0x017c9, 0x017d3}, {0x017dd, 0x017dd}, {0x0180b, 0x0180d},
    {0x01885, 0x01886}, {0x018a9, 0x018a9}, {0x01920, 0x01922},
    {0x01927, 0x01928}, {0x01932, 0x01932}, {0x01939, 0x0193b},
    {0x01a17, 0x01a18}, {0x01a1b, 0x01a1b}, {0x01a56, 0x01a56},
    {0x01a58, 0x01a5e}, {0x01a60, 0x01a60}, {0x01a62, 0x01a62},
    {0x01a65, 0x01a6c}, {0x01a73, 0x01a7c}, {0x01a7f, 0x01a7f},
    {0x01ab0, 0x01ac0}, {0x01b00, 0x01b03}, {0x01b34, 0x01b34},
    {0x01b36, 0x01b3a}, {0x01b3c, 0x01b3c}, {0x01b42, 0x01b42},
    {0x01b6b, 0x01b73}, {0x01b80, 0x01b81}, {0x01ba2, 0x01ba5},
    {0x01ba8, 0x01ba9}, {0x01bab, 0x01bad}, {0x01be6, 0x01be6},
    {0x01be8, 0x01be9}, {0x01bed, 0x01bed}, {0x01bef, 0x01bf1},
    {0x01c2c, 0x01c33}, {0x01c36, 0x01c37}, {0x01cd0, 0x01cd2},
    {0x01cd4, 0x01ce0}, {0x01ce2, 0x01ce8}, {0x01ced, 0x01ced},
    {0x01cf4, 0x01cf4}, {0x01cf8, 0x01cf9}, {0x01dc0, 0x01df9},
    {0x01dfb, 0x01dff}, {0x020d0, 0x020f0}, {0x02cef, 0x02cf1},
    {0x02d7f, 0x02d7f}, {0x02de0, 0x02dff}, {0x0302a, 0x0302d},
    {0x03099, 0x0309a}, {0x0a66f, 0x0a672}, {0x0a674, 0x0a67d},
    {0x0a69e, 0x0a69f}, {0x0a6f0, 0x0a6f1}, {0x0a802, 0x0a802},
    {0x0a806, 0x0a806}, {0x0a80b, 0x0a80b}, {0x0a825, 0x0a826},
    {0x0a82c, 0x0a82c}, {0x0a8c4, 0x0a8c5}, {0x0a8e0, 0x0a8f1},
    {0x0a8ff, 0x0a8ff}, {0x0a926, 0x0a92d}, {0x0a947, 0x0a951},
    {0x0a980, 0x0a982}, {0x0a9b3, 0x0a9b3}, {0x0a9b6, 0x0a9b9},
    {0x0a9bc, 0x0a9bd}, {0x0a9e5, 0x0a9e5}, {0x0aa29, 0x0aa2e},
    {0x0aa31, 0x0aa32}, {0x0aa35, 0x0aa36}, {0x0aa43, 0x0aa43},
    {0x0aa4c, 0x0aa4c}, {0x0aa7c, 0x0aa7c}, {0x0aab0, 0x0aab0},
    {0x0aab2, 0x0aab4}, {0x0aab7, 0x0aab8}, {0x0aabe, 0x0aabf},
    {0x0aac1, 0x0aac1}, {0x0aaec, 0x0aaed}, {0x0aaf6, 0x0aaf6},
    {0x0abe5, 0x0abe5}, {0x0abe8, 0x0abe8}, {0x0abed, 0x0abed},
    {0x0fb1e, 0x0fb1e}, {0x0fe00, 0x0fe0f}, {0x0fe20, 0x0fe2f},
    {0x101fd, 0x101fd}, {0x102e0, 0x102e0}, {0x10376, 0x1037a},
    {0x10a01, 0x10a03}, {0x10a05, 0x10a06}, {0x10a0c, 0x10a0f},
    {0x10a38, 0x10a3a}, {0x10a3f, 0x10a3f}, {0x10ae5, 0x10ae6},
    {0x10d24, 0x10d27}, {0x10eab, 0x10eac}, {0x10f46, 0x10f50},
    {0x11001, 0x11001}, {0x11038, 0x11046}, {0x1107f, 0x11081},
    {0x110b3, 0x110b6}, {0x110b9, 0x110ba}, {0x11100, 0x11102},
    {0x11127, 0x1112b}, {0x1112d, 0x11134}, {0x11173, 0x11173},
    {0x11180, 0x11181}, {0x111b6, 0x111be}, {0x111c9, 0x111cc},
    {0x111cf, 0x111cf}, {0x1122f, 0x11231}, {0x11234, 0x11234},
    {0x11236, 0x11237}, {0x1123e, 0x1123e}, {0x112df, 0x112df},
    {0x112e3, 0x112ea}, {0x11300, 0x11301}, {0x1133b, 0x1133c},
    {0x11340, 0x11340}, {0x11366, 0x1136c}, {0x11370, 0x11374},
    {0x11438, 0x1143f}, {0x11442, 0x11444}, {0x11446, 0x11446},
    {0x1145e, 0x1145e}, {0x114b3, 0x114b8}, {0x114ba, 0x114ba},
    {0x114bf, 0x114c0}, {0x114c2, 0x114c3}, {0x115b2, 0x115b5},
    {0x115bc, 0x115bd}, {0x115bf, 0x115c0}, {0x115dc, 0x115dd},
    {0x11633, 0x1163a}, {0x1163d, 0x1163d}, {0x1163f, 0x11640},
    {0x116ab, 0x116ab}, {0x116ad, 0x116ad}, {0x116b0, 0x116b5},
    {0x116b7, 0x116b7}, {0x1171d, 0x1171f}, {0x11722, 0x11725},
    {0x11727, 0x1172b}, {0x1182f, 0x11837}, {0x11839, 0x1183a},
    {0x1193b, 0x1193c}, {0x1193e, 0x1193e}, {0x11943, 0x11943},
    {0x119d4, 0x119d7}, {0x119da, 0x119db}, {0x119e0, 0x119e0},
    {0x11a01, 0x11a0a}, {0x11a33, 0x11a38}, {0x11a3b, 0x11a3e},
    {0x11a47, 0x11a47}, {0x11a51, 0x11a56}, {0x11a59, 0x11a5b},
    {0x11a8a, 0x11a96}, {0x11a98, 0x11a99}, {0x11c30, 0x11c36},
    {0x11c38, 0x11c3d}, {0x11c3f, 0x11c3f}, {0x11c92, 0x11ca7},
    {0x11caa, 0x11cb0}, {0x11cb2, 0x11cb3}, {0x11cb5, 0x11cb6},
    {0x11d31, 0x11d36}, {0x11d3a, 0x11d3a}, {0x11d3c, 0x11d3d},
    {0x11d3f, 0x11d45}, {0x11d47, 0x11d47}, {0x11d90, 0x11d91},
    {0x11d95, 0x11d95}, {0x11d97, 0x11d97}, {0x11ef3, 0x11ef4},
    {0x16af0, 0x16af4}, {0x16b30, 0x16b36}, {0x16f4f, 0x16f4f},
    {0x16f8f, 0x16f92}, {0x16fe4, 0x16fe4}, {0x1bc9d, 0x1bc9e},
    {0x1d167, 0x1d169}, {0x1d17b, 0x1d182}, {0x1d185, 0x1d18b},
    {0x1d1aa, 0x1d1ad}, {0x1d242, 0x1d244}, {0x1da00, 0x1da36},
    {0x1da3b, 0x1da6c}, {0x1da75, 0x1da75}, {0x1da84, 0x1da84},
    {0x1da9b, 0x1da9f}, {0x1daa1, 0x1daaf}, {0x1e000, 0x1e006},
    {0x1e008, 0x1e018}, {0x1e01b, 0x1e021}, {0x1e023, 0x1e024},
    {0x1e026, 0x1e02a}, {0x1e130, 0x1e136}, {0x1e2ec, 0x1e2ef},
    {0x1e8d0, 0x1e8d6}, {0x1e944, 0x1e94a}, {0xe0100, 0xe01ef},
}};

// As of Unicode 13.0.0
const std::array<Interval, 116> g_full_width_characters = {{
    {0x01100, 0x0115f}, {0x0231a, 0x0231b}, {0x02329, 0x0232a},
    {0x023e9, 0x023ec}, {0x023f0, 0x023f0}, {0x023f3, 0x023f3},
    {0x025fd, 0x025fe}, {0x02614, 0x02615}, {0x02648, 0x02653},
    {0x0267f, 0x0267f}, {0x02693, 0x02693}, {0x026a1, 0x026a1},
    {0x026aa, 0x026ab}, {0x026bd, 0x026be}, {0x026c4, 0x026c5},
    {0x026ce, 0x026ce}, {0x026d4, 0x026d4}, {0x026ea, 0x026ea},
    {0x026f2, 0x026f3}, {0x026f5, 0x026f5}, {0x026fa, 0x026fa},
    {0x026fd, 0x026fd}, {0x02705, 0x02705}, {0x0270a, 0x0270b},
    {0x02728, 0x02728}, {0x0274c, 0x0274c}, {0x0274e, 0x0274e},
    {0x02753, 0x02755}, {0x02757, 0x02757}, {0x02795, 0x02797},
    {0x027b0, 0x027b0}, {0x027bf, 0x027bf}, {0x02b1b, 0x02b1c},
    {0x02b50, 0x02b50}, {0x02b55, 0x02b55}, {0x02e80, 0x02e99},
    {0x02e9b, 0x02ef3}, {0x02f00, 0x02fd5}, {0x02ff0, 0x02ffb},
    {0x03000, 0x0303e}, {0x03041, 0x03096}, {0x03099, 0x030ff},
    {0x03105, 0x0312f}, {0x03131, 0x0318e}, {0x03190, 0x031e3},
    {0x031f0, 0x0321e}, {0x03220, 0x03247}, {0x03250, 0x04dbf},
    {0x04e00, 0x0a48c}, {0x0a490, 0x0a4c6}, {0x0a960, 0x0a97c},
    {0x0ac00, 0x0d7a3}, {0x0f900, 0x0faff}, {0x0fe10, 0x0fe19},
    {0x0fe30, 0x0fe52}, {0x0fe54, 0x0fe66}, {0x0fe68, 0x0fe6b},
    {0x0ff01, 0x0ff60}, {0x0ffe0, 0x0ffe6}, {0x16fe0, 0x16fe4},
    {0x16ff0, 0x16ff1}, {0x17000, 0x187f7}, {0x18800, 0x18cd5},
    {0x18d00, 0x18d08}, {0x1b000, 0x1b11e}, {0x1b150, 0x1b152},
    {0x1b164, 0x1b167}, {0x1b170, 0x1b2fb}, {0x1f004, 0x1f004},
    {0x1f0cf, 0x1f0cf}, {0x1f18e, 0x1f18e}, {0x1f191, 0x1f19a},
    {0x1f200, 0x1f202}, {0x1f210, 0x1f23b}, {0x1f240, 0x1f248},
    {0x1f250, 0x1f251}, {0x1f260, 0x1f265}, {0x1f300, 0x1f320},
    {0x1f32d, 0x1f335}, {0x1f337, 0x1f37c}, {0x1f37e, 0x1f393},
    {0x1f3a0, 0x1f3ca}, {0x1f3cf, 0x1f3d3}, {0x1f3e0, 0x1f3f0},
    {0x1f3f4, 0x1f3f4}, {0x1f3f8, 0x1f43e}, {0x1f440, 0x1f440},
    {0x1f442, 0x1f4fc}, {0x1f4ff, 0x1f53d}, {0x1f54b, 0x1f54e},
    {0x1f550, 0x1f567}, {0x1f57a, 0x1f57a}, {0x1f595, 0x1f596},
    {0x1f5a4, 0x1f5a4}, {0x1f5fb, 0x1f64f}, {0x1f680, 0x1f6c5},
    {0x1f6cc, 0x1f6cc}, {0x1f6d0, 0x1f6d2}, {0x1f6d5, 0x1f6d7},
    {0x1f6eb, 0x1f6ec}, {0x1f6f4, 0x1f6fc}, {0x1f7e0, 0x1f7eb},
    {0x1f90c, 0x1f93a}, {0x1f93c, 0x1f945}, {0x1f947, 0x1f978},
    {0x1f97a, 0x1f9cb}, {0x1f9cd, 0x1f9ff}, {0x1fa70, 0x1fa74},
    {0x1fa78, 0x1fa7a}, {0x1fa80, 0x1fa86}, {0x1fa90, 0x1faa8},
    {0x1fab0, 0x1fab6}, {0x1fac0, 0x1fac2}, {0x1fad0, 0x1fad6},
    {0x20000, 0x2fffd}, {0x30000, 0x3fffd},
}};

using WBP = ftxui::WordBreakProperty;
struct WordBreakPropertyInterval {
  uint32_t first;
  uint32_t last;
  WBP property;
};

// Properties from:
// https://www.unicode.org/Public/UCD/latest/ucd/auxiliary/WordBreakProperty.txt
const std::array<WordBreakPropertyInterval, 1288> g_word_break_intervals = {{
    {0x0000A, 0x0000A, WBP::LF},
    {0x0000B, 0x0000C, WBP::Newline},
    {0x0000D, 0x0000D, WBP::CR},
    {0x00020, 0x00020, WBP::WSegSpace},
    {0x00022, 0x00022, WBP::Double_Quote},
    {0x00027, 0x00027, WBP::Single_Quote},
    {0x0002C, 0x0002C, WBP::MidNum},
    {0x0002E, 0x0002E, WBP::MidNumLet},
    {0x00030, 0x00039, WBP::Numeric},
    {0x0003A, 0x0003A, WBP::MidLetter},
    {0x0003B, 0x0003B, WBP::MidNum},
    {0x00041, 0x0005A, WBP::ALetter},
    {0x0005F, 0x0005F, WBP::ExtendNumLet},
    {0x00061, 0x0007A, WBP::ALetter},
    {0x00085, 0x00085, WBP::Newline},
    {0x000AA, 0x000AA, WBP::ALetter},
    {0x000AD, 0x000AD, WBP::Format},
    {0x000B5, 0x000B5, WBP::ALetter},
    {0x000B7, 0x000B7, WBP::MidLetter},
    {0x000BA, 0x000BA, WBP::ALetter},
    {0x000C0, 0x000D6, WBP::ALetter},
    {0x000D8, 0x000F6, WBP::ALetter},
    {0x000F8, 0x001BA, WBP::ALetter},
    {0x001BB, 0x001BB, WBP::ALetter},
    {0x001BC, 0x001BF, WBP::ALetter},
    {0x001C0, 0x001C3, WBP::ALetter},
    {0x001C4, 0x00293, WBP::ALetter},
    {0x00294, 0x00294, WBP::ALetter},
    {0x00295, 0x002AF, WBP::ALetter},
    {0x002B0, 0x002C1, WBP::ALetter},
    {0x002C2, 0x002C5, WBP::ALetter},
    {0x002C6, 0x002D1, WBP::ALetter},
    {0x002D2, 0x002D7, WBP::ALetter},
    {0x002DE, 0x002DF, WBP::ALetter},
    {0x002E0, 0x002E4, WBP::ALetter},
    {0x002E5, 0x002EB, WBP::ALetter},
    {0x002EC, 0x002EC, WBP::ALetter},
    {0x002ED, 0x002ED, WBP::ALetter},
    {0x002EE, 0x002EE, WBP::ALetter},
    {0x002EF, 0x002FF, WBP::ALetter},
    {0x00300, 0x0036F, WBP::Extend},
    {0x00370, 0x00373, WBP::ALetter},
    {0x00374, 0x00374, WBP::ALetter},
    {0x00376, 0x00377, WBP::ALetter},
    {0x0037A, 0x0037A, WBP::ALetter},
    {0x0037B, 0x0037D, WBP::ALetter},
    {0x0037E, 0x0037E, WBP::MidNum},
    {0x0037F, 0x0037F, WBP::ALetter},
    {0x00386, 0x00386, WBP::ALetter},
    {0x00387, 0x00387, WBP::MidLetter},
    {0x00388, 0x0038A, WBP::ALetter},
    {0x0038C, 0x0038C, WBP::ALetter},
    {0x0038E, 0x003A1, WBP::ALetter},
    {0x003A3, 0x003F5, WBP::ALetter},
    {0x003F7, 0x00481, WBP::ALetter},
    {0x00483, 0x00487, WBP::Extend},
    {0x00488, 0x00489, WBP::Extend},
    {0x0048A, 0x0052F, WBP::ALetter},
    {0x00531, 0x00556, WBP::ALetter},
    {0x00559, 0x00559, WBP::ALetter},
    {0x0055A, 0x0055C, WBP::ALetter},
    {0x0055E, 0x0055E, WBP::ALetter},
    {0x0055F, 0x0055F, WBP::MidLetter},
    {0x00560, 0x00588, WBP::ALetter},
    {0x00589, 0x00589, WBP::MidNum},
    {0x0058A, 0x0058A, WBP::ALetter},
    {0x00591, 0x005BD, WBP::Extend},
    {0x005BF, 0x005BF, WBP::Extend},
    {0x005C1, 0x005C2, WBP::Extend},
    {0x005C4, 0x005C5, WBP::Extend},
    {0x005C7, 0x005C7, WBP::Extend},
    {0x005D0, 0x005EA, WBP::Hebrew_Letter},
    {0x005EF, 0x005F2, WBP::Hebrew_Letter},
    {0x005F3, 0x005F3, WBP::ALetter},
    {0x005F4, 0x005F4, WBP::MidLetter},
    {0x00600, 0x00605, WBP::Format},
    {0x0060C, 0x0060D, WBP::MidNum},
    {0x00610, 0x0061A, WBP::Extend},
    {0x0061C, 0x0061C, WBP::Format},
    {0x00620, 0x0063F, WBP::ALetter},
    {0x00640, 0x00640, WBP::ALetter},
    {0x00641, 0x0064A, WBP::ALetter},
    {0x0064B, 0x0065F, WBP::Extend},
    {0x00660, 0x00669, WBP::Numeric},
    {0x0066B, 0x0066B, WBP::Numeric},
    {0x0066C, 0x0066C, WBP::MidNum},
    {0x0066E, 0x0066F, WBP::ALetter},
    {0x00670, 0x00670, WBP::Extend},
    {0x00671, 0x006D3, WBP::ALetter},
    {0x006D5, 0x006D5, WBP::ALetter},
    {0x006D6, 0x006DC, WBP::Extend},
    {0x006DD, 0x006DD, WBP::Format},
    {0x006DF, 0x006E4, WBP::Extend},
    {0x006E5, 0x006E6, WBP::ALetter},
    {0x006E7, 0x006E8, WBP::Extend},
    {0x006EA, 0x006ED, WBP::Extend},
    {0x006EE, 0x006EF, WBP::ALetter},
    {0x006F0, 0x006F9, WBP::Numeric},
    {0x006FA, 0x006FC, WBP::ALetter},
    {0x006FF, 0x006FF, WBP::ALetter},
    {0x0070F, 0x0070F, WBP::Format},
    {0x00710, 0x00710, WBP::ALetter},
    {0x00711, 0x00711, WBP::Extend},
    {0x00712, 0x0072F, WBP::ALetter},
    {0x00730, 0x0074A, WBP::Extend},
    {0x0074D, 0x007A5, WBP::ALetter},
    {0x007A6, 0x007B0, WBP::Extend},
    {0x007B1, 0x007B1, WBP::ALetter},
    {0x007C0, 0x007C9, WBP::Numeric},
    {0x007CA, 0x007EA, WBP::ALetter},
    {0x007EB, 0x007F3, WBP::Extend},
    {0x007F4, 0x007F5, WBP::ALetter},
    {0x007F8, 0x007F8, WBP::MidNum},
    {0x007FA, 0x007FA, WBP::ALetter},
    {0x007FD, 0x007FD, WBP::Extend},
    {0x00800, 0x00815, WBP::ALetter},
    {0x00816, 0x00819, WBP::Extend},
    {0x0081A, 0x0081A, WBP::ALetter},
    {0x0081B, 0x00823, WBP::Extend},
    {0x00824, 0x00824, WBP::ALetter},
    {0x00825, 0x00827, WBP::Extend},
    {0x00828, 0x00828, WBP::ALetter},
    {0x00829, 0x0082D, WBP::Extend},
    {0x00840, 0x00858, WBP::ALetter},
    {0x00859, 0x0085B, WBP::Extend},
    {0x00860, 0x0086A, WBP::ALetter},
    {0x008A0, 0x008B4, WBP::ALetter},
    {0x008B6, 0x008C7, WBP::ALetter},
    {0x008D3, 0x008E1, WBP::Extend},
    {0x008E2, 0x008E2, WBP::Format},
    {0x008E3, 0x00902, WBP::Extend},
    {0x00903, 0x00903, WBP::Extend},
    {0x00904, 0x00939, WBP::ALetter},
    {0x0093A, 0x0093A, WBP::Extend},
    {0x0093B, 0x0093B, WBP::Extend},
    {0x0093C, 0x0093C, WBP::Extend},
    {0x0093D, 0x0093D, WBP::ALetter},
    {0x0093E, 0x00940, WBP::Extend},
    {0x00941, 0x00948, WBP::Extend},
    {0x00949, 0x0094C, WBP::Extend},
    {0x0094D, 0x0094D, WBP::Extend},
    {0x0094E, 0x0094F, WBP::Extend},
    {0x00950, 0x00950, WBP::ALetter},
    {0x00951, 0x00957, WBP::Extend},
    {0x00958, 0x00961, WBP::ALetter},
    {0x00962, 0x00963, WBP::Extend},
    {0x00966, 0x0096F, WBP::Numeric},
    {0x00971, 0x00971, WBP::ALetter},
    {0x00972, 0x00980, WBP::ALetter},
    {0x00981, 0x00981, WBP::Extend},
    {0x00982, 0x00983, WBP::Extend},
    {0x00985, 0x0098C, WBP::ALetter},
    {0x0098F, 0x00990, WBP::ALetter},
    {0x00993, 0x009A8, WBP::ALetter},
    {0x009AA, 0x009B0, WBP::ALetter},
    {0x009B2, 0x009B2, WBP::ALetter},
    {0x009B6, 0x009B9, WBP::ALetter},
    {0x009BC, 0x009BC, WBP::Extend},
    {0x009BD, 0x009BD, WBP::ALetter},
    {0x009BE, 0x009C0, WBP::Extend},
    {0x009C1, 0x009C4, WBP::Extend},
    {0x009C7, 0x009C8, WBP::Extend},
    {0x009CB, 0x009CC, WBP::Extend},
    {0x009CD, 0x009CD, WBP::Extend},
    {0x009CE, 0x009CE, WBP::ALetter},
    {0x009D7, 0x009D7, WBP::Extend},
    {0x009DC, 0x009DD, WBP::ALetter},
    {0x009DF, 0x009E1, WBP::ALetter},
    {0x009E2, 0x009E3, WBP::Extend},
    {0x009E6, 0x009EF, WBP::Numeric},
    {0x009F0, 0x009F1, WBP::ALetter},
    {0x009FC, 0x009FC, WBP::ALetter},
    {0x009FE, 0x009FE, WBP::Extend},
    {0x00A01, 0x00A02, WBP::Extend},
    {0x00A03, 0x00A03, WBP::Extend},
    {0x00A05, 0x00A0A, WBP::ALetter},
    {0x00A0F, 0x00A10, WBP::ALetter},
    {0x00A13, 0x00A28, WBP::ALetter},
    {0x00A2A, 0x00A30, WBP::ALetter},
    {0x00A32, 0x00A33, WBP::ALetter},
    {0x00A35, 0x00A36, WBP::ALetter},
    {0x00A38, 0x00A39, WBP::ALetter},
    {0x00A3C, 0x00A3C, WBP::Extend},
    {0x00A3E, 0x00A40, WBP::Extend},
    {0x00A41, 0x00A42, WBP::Extend},
    {0x00A47, 0x00A48, WBP::Extend},
    {0x00A4B, 0x00A4D, WBP::Extend},
    {0x00A51, 0x00A51, WBP::Extend},
    {0x00A59, 0x00A5C, WBP::ALetter},
    {0x00A5E, 0x00A5E, WBP::ALetter},
    {0x00A66, 0x00A6F, WBP::Numeric},
    {0x00A70, 0x00A71, WBP::Extend},
    {0x00A72, 0x00A74, WBP::ALetter},
    {0x00A75, 0x00A75, WBP::Extend},
    {0x00A81, 0x00A82, WBP::Extend},
    {0x00A83, 0x00A83, WBP::Extend},
    {0x00A85, 0x00A8D, WBP::ALetter},
    {0x00A8F, 0x00A91, WBP::ALetter},
    {0x00A93, 0x00AA8, WBP::ALetter},
    {0x00AAA, 0x00AB0, WBP::ALetter},
    {0x00AB2, 0x00AB3, WBP::ALetter},
    {0x00AB5, 0x00AB9, WBP::ALetter},
    {0x00ABC, 0x00ABC, WBP::Extend},
    {0x00ABD, 0x00ABD, WBP::ALetter},
    {0x00ABE, 0x00AC0, WBP::Extend},
    {0x00AC1, 0x00AC5, WBP::Extend},
    {0x00AC7, 0x00AC8, WBP::Extend},
    {0x00AC9, 0x00AC9, WBP::Extend},
    {0x00ACB, 0x00ACC, WBP::Extend},
    {0x00ACD, 0x00ACD, WBP::Extend},
    {0x00AD0, 0x00AD0, WBP::ALetter},
    {0x00AE0, 0x00AE1, WBP::ALetter},
    {0x00AE2, 0x00AE3, WBP::Extend},
    {0x00AE6, 0x00AEF, WBP::Numeric},
    {0x00AF9, 0x00AF9, WBP::ALetter},
    {0x00AFA, 0x00AFF, WBP::Extend},
    {0x00B01, 0x00B01, WBP::Extend},
    {0x00B02, 0x00B03, WBP::Extend},
    {0x00B05, 0x00B0C, WBP::ALetter},
    {0x00B0F, 0x00B10, WBP::ALetter},
    {0x00B13, 0x00B28, WBP::ALetter},
    {0x00B2A, 0x00B30, WBP::ALetter},
    {0x00B32, 0x00B33, WBP::ALetter},
    {0x00B35, 0x00B39, WBP::ALetter},
    {0x00B3C, 0x00B3C, WBP::Extend},
    {0x00B3D, 0x00B3D, WBP::ALetter},
    {0x00B3E, 0x00B3E, WBP::Extend},
    {0x00B3F, 0x00B3F, WBP::Extend},
    {0x00B40, 0x00B40, WBP::Extend},
    {0x00B41, 0x00B44, WBP::Extend},
    {0x00B47, 0x00B48, WBP::Extend},
    {0x00B4B, 0x00B4C, WBP::Extend},
    {0x00B4D, 0x00B4D, WBP::Extend},
    {0x00B55, 0x00B56, WBP::Extend},
    {0x00B57, 0x00B57, WBP::Extend},
    {0x00B5C, 0x00B5D, WBP::ALetter},
    {0x00B5F, 0x00B61, WBP::ALetter},
    {0x00B62, 0x00B63, WBP::Extend},
    {0x00B66, 0x00B6F, WBP::Numeric},
    {0x00B71, 0x00B71, WBP::ALetter},
    {0x00B82, 0x00B82, WBP::Extend},
    {0x00B83, 0x00B83, WBP::ALetter},
    {0x00B85, 0x00B8A, WBP::ALetter},
    {0x00B8E, 0x00B90, WBP::ALetter},
    {0x00B92, 0x00B95, WBP::ALetter},
    {0x00B99, 0x00B9A, WBP::ALetter},
    {0x00B9C, 0x00B9C, WBP::ALetter},
    {0x00B9E, 0x00B9F, WBP::ALetter},
    {0x00BA3, 0x00BA4, WBP::ALetter},
    {0x00BA8, 0x00BAA, WBP::ALetter},
    {0x00BAE, 0x00BB9, WBP::ALetter},
    {0x00BBE, 0x00BBF, WBP::Extend},
    {0x00BC0, 0x00BC0, WBP::Extend},
    {0x00BC1, 0x00BC2, WBP::Extend},
    {0x00BC6, 0x00BC8, WBP::Extend},
    {0x00BCA, 0x00BCC, WBP::Extend},
    {0x00BCD, 0x00BCD, WBP::Extend},
    {0x00BD0, 0x00BD0, WBP::ALetter},
    {0x00BD7, 0x00BD7, WBP::Extend},
    {0x00BE6, 0x00BEF, WBP::Numeric},
    {0x00C00, 0x00C00, WBP::Extend},
    {0x00C01, 0x00C03, WBP::Extend},
    {0x00C04, 0x00C04, WBP::Extend},
    {0x00C05, 0x00C0C, WBP::ALetter},
    {0x00C0E, 0x00C10, WBP::ALetter},
    {0x00C12, 0x00C28, WBP::ALetter},
    {0x00C2A, 0x00C39, WBP::ALetter},
    {0x00C3D, 0x00C3D, WBP::ALetter},
    {0x00C3E, 0x00C40, WBP::Extend},
    {0x00C41, 0x00C44, WBP::Extend},
    {0x00C46, 0x00C48, WBP::Extend},
    {0x00C4A, 0x00C4D, WBP::Extend},
    {0x00C55, 0x00C56, WBP::Extend},
    {0x00C58, 0x00C5A, WBP::ALetter},
    {0x00C60, 0x00C61, WBP::ALetter},
    {0x00C62, 0x00C63, WBP::Extend},
    {0x00C66, 0x00C6F, WBP::Numeric},
    {0x00C80, 0x00C80, WBP::ALetter},
    {0x00C81, 0x00C81, WBP::Extend},
    {0x00C82, 0x00C83, WBP::Extend},
    {0x00C85, 0x00C8C, WBP::ALetter},
    {0x00C8E, 0x00C90, WBP::ALetter},
    {0x00C92, 0x00CA8, WBP::ALetter},
    {0x00CAA, 0x00CB3, WBP::ALetter},
    {0x00CB5, 0x00CB9, WBP::ALetter},
    {0x00CBC, 0x00CBC, WBP::Extend},
    {0x00CBD, 0x00CBD, WBP::ALetter},
    {0x00CBE, 0x00CBE, WBP::Extend},
    {0x00CBF, 0x00CBF, WBP::Extend},
    {0x00CC0, 0x00CC4, WBP::Extend},
    {0x00CC6, 0x00CC6, WBP::Extend},
    {0x00CC7, 0x00CC8, WBP::Extend},
    {0x00CCA, 0x00CCB, WBP::Extend},
    {0x00CCC, 0x00CCD, WBP::Extend},
    {0x00CD5, 0x00CD6, WBP::Extend},
    {0x00CDE, 0x00CDE, WBP::ALetter},
    {0x00CE0, 0x00CE1, WBP::ALetter},
    {0x00CE2, 0x00CE3, WBP::Extend},
    {0x00CE6, 0x00CEF, WBP::Numeric},
    {0x00CF1, 0x00CF2, WBP::ALetter},
    {0x00D00, 0x00D01, WBP::Extend},
    {0x00D02, 0x00D03, WBP::Extend},
    {0x00D04, 0x00D0C, WBP::ALetter},
    {0x00D0E, 0x00D10, WBP::ALetter},
    {0x00D12, 0x00D3A, WBP::ALetter},
    {0x00D3B, 0x00D3C, WBP::Extend},
    {0x00D3D, 0x00D3D, WBP::ALetter},
    {0x00D3E, 0x00D40, WBP::Extend},
    {0x00D41, 0x00D44, WBP::Extend},
    {0x00D46, 0x00D48, WBP::Extend},
    {0x00D4A, 0x00D4C, WBP::Extend},
    {0x00D4D, 0x00D4D, WBP::Extend},
    {0x00D4E, 0x00D4E, WBP::ALetter},
    {0x00D54, 0x00D56, WBP::ALetter},
    {0x00D57, 0x00D57, WBP::Extend},
    {0x00D5F, 0x00D61, WBP::ALetter},
    {0x00D62, 0x00D63, WBP::Extend},
    {0x00D66, 0x00D6F, WBP::Numeric},
    {0x00D7A, 0x00D7F, WBP::ALetter},
    {0x00D81, 0x00D81, WBP::Extend},
    {0x00D82, 0x00D83, WBP::Extend},
    {0x00D85, 0x00D96, WBP::ALetter},
    {0x00D9A, 0x00DB1, WBP::ALetter},
    {0x00DB3, 0x00DBB, WBP::ALetter},
    {0x00DBD, 0x00DBD, WBP::ALetter},
    {0x00DC0, 0x00DC6, WBP::ALetter},
    {0x00DCA, 0x00DCA, WBP::Extend},
    {0x00DCF, 0x00DD1, WBP::Extend},
    {0x00DD2, 0x00DD4, WBP::Extend},
    {0x00DD6, 0x00DD6, WBP::Extend},
    {0x00DD8, 0x00DDF, WBP::Extend},
    {0x00DE6, 0x00DEF, WBP::Numeric},
    {0x00DF2, 0x00DF3, WBP::Extend},
    {0x00E31, 0x00E31, WBP::Extend},
    {0x00E34, 0x00E3A, WBP::Extend},
    {0x00E47, 0x00E4E, WBP::Extend},
    {0x00E50, 0x00E59, WBP::Numeric},
    {0x00EB1, 0x00EB1, WBP::Extend},
    {0x00EB4, 0x00EBC, WBP::Extend},
    {0x00EC8, 0x00ECD, WBP::Extend},
    {0x00ED0, 0x00ED9, WBP::Numeric},
    {0x00F00, 0x00F00, WBP::ALetter},
    {0x00F18, 0x00F19, WBP::Extend},
    {0x00F20, 0x00F29, WBP::Numeric},
    {0x00F35, 0x00F35, WBP::Extend},
    {0x00F37, 0x00F37, WBP::Extend},
    {0x00F39, 0x00F39, WBP::Extend},
    {0x00F3E, 0x00F3F, WBP::Extend},
    {0x00F40, 0x00F47, WBP::ALetter},
    {0x00F49, 0x00F6C, WBP::ALetter},
    {0x00F71, 0x00F7E, WBP::Extend},
    {0x00F7F, 0x00F7F, WBP::Extend},
    {0x00F80, 0x00F84, WBP::Extend},
    {0x00F86, 0x00F87, WBP::Extend},
    {0x00F88, 0x00F8C, WBP::ALetter},
    {0x00F8D, 0x00F97, WBP::Extend},
    {0x00F99, 0x00FBC, WBP::Extend},
    {0x00FC6, 0x00FC6, WBP::Extend},
    {0x0102B, 0x0102C, WBP::Extend},
    {0x0102D, 0x01030, WBP::Extend},
    {0x01031, 0x01031, WBP::Extend},
    {0x01032, 0x01037, WBP::Extend},
    {0x01038, 0x01038, WBP::Extend},
    {0x01039, 0x0103A, WBP::Extend},
    {0x0103B, 0x0103C, WBP::Extend},
    {0x0103D, 0x0103E, WBP::Extend},
    {0x01040, 0x01049, WBP::Numeric},
    {0x01056, 0x01057, WBP::Extend},
    {0x01058, 0x01059, WBP::Extend},
    {0x0105E, 0x01060, WBP::Extend},
    {0x01062, 0x01064, WBP::Extend},
    {0x01067, 0x0106D, WBP::Extend},
    {0x01071, 0x01074, WBP::Extend},
    {0x01082, 0x01082, WBP::Extend},
    {0x01083, 0x01084, WBP::Extend},
    {0x01085, 0x01086, WBP::Extend},
    {0x01087, 0x0108C, WBP::Extend},
    {0x0108D, 0x0108D, WBP::Extend},
    {0x0108F, 0x0108F, WBP::Extend},
    {0x01090, 0x01099, WBP::Numeric},
    {0x0109A, 0x0109C, WBP::Extend},
    {0x0109D, 0x0109D, WBP::Extend},
    {0x010A0, 0x010C5, WBP::ALetter},
    {0x010C7, 0x010C7, WBP::ALetter},
    {0x010CD, 0x010CD, WBP::ALetter},
    {0x010D0, 0x010FA, WBP::ALetter},
    {0x010FC, 0x010FC, WBP::ALetter},
    {0x010FD, 0x010FF, WBP::ALetter},
    {0x01100, 0x01248, WBP::ALetter},
    {0x0124A, 0x0124D, WBP::ALetter},
    {0x01250, 0x01256, WBP::ALetter},
    {0x01258, 0x01258, WBP::ALetter},
    {0x0125A, 0x0125D, WBP::ALetter},
    {0x01260, 0x01288, WBP::ALetter},
    {0x0128A, 0x0128D, WBP::ALetter},
    {0x01290, 0x012B0, WBP::ALetter},
    {0x012B2, 0x012B5, WBP::ALetter},
    {0x012B8, 0x012BE, WBP::ALetter},
    {0x012C0, 0x012C0, WBP::ALetter},
    {0x012C2, 0x012C5, WBP::ALetter},
    {0x012C8, 0x012D6, WBP::ALetter},
    {0x012D8, 0x01310, WBP::ALetter},
    {0x01312, 0x01315, WBP::ALetter},
    {0x01318, 0x0135A, WBP::ALetter},
    {0x0135D, 0x0135F, WBP::Extend},
    {0x01380, 0x0138F, WBP::ALetter},
    {0x013A0, 0x013F5, WBP::ALetter},
    {0x013F8, 0x013FD, WBP::ALetter},
    {0x01401, 0x0166C, WBP::ALetter},
    {0x0166F, 0x0167F, WBP::ALetter},
    {0x01680, 0x01680, WBP::WSegSpace},
    {0x01681, 0x0169A, WBP::ALetter},
    {0x016A0, 0x016EA, WBP::ALetter},
    {0x016EE, 0x016F0, WBP::ALetter},
    {0x016F1, 0x016F8, WBP::ALetter},
    {0x01700, 0x0170C, WBP::ALetter},
    {0x0170E, 0x01711, WBP::ALetter},
    {0x01712, 0x01714, WBP::Extend},
    {0x01720, 0x01731, WBP::ALetter},
    {0x01732, 0x01734, WBP::Extend},
    {0x01740, 0x01751, WBP::ALetter},
    {0x01752, 0x01753, WBP::Extend},
    {0x01760, 0x0176C, WBP::ALetter},
    {0x0176E, 0x01770, WBP::ALetter},
    {0x01772, 0x01773, WBP::Extend},
    {0x017B4, 0x017B5, WBP::Extend},
    {0x017B6, 0x017B6, WBP::Extend},
    {0x017B7, 0x017BD, WBP::Extend},
    {0x017BE, 0x017C5, WBP::Extend},
    {0x017C6, 0x017C6, WBP::Extend},
    {0x017C7, 0x017C8, WBP::Extend},
    {0x017C9, 0x017D3, WBP::Extend},
    {0x017DD, 0x017DD, WBP::Extend},
    {0x017E0, 0x017E9, WBP::Numeric},
    {0x0180B, 0x0180D, WBP::Extend},
    {0x0180E, 0x0180E, WBP::Format},
    {0x01810, 0x01819, WBP::Numeric},
    {0x01820, 0x01842, WBP::ALetter},
    {0x01843, 0x01843, WBP::ALetter},
    {0x01844, 0x01878, WBP::ALetter},
    {0x01880, 0x01884, WBP::ALetter},
    {0x01885, 0x01886, WBP::Extend},
    {0x01887, 0x018A8, WBP::ALetter},
    {0x018A9, 0x018A9, WBP::Extend},
    {0x018AA, 0x018AA, WBP::ALetter},
    {0x018B0, 0x018F5, WBP::ALetter},
    {0x01900, 0x0191E, WBP::ALetter},
    {0x01920, 0x01922, WBP::Extend},
    {0x01923, 0x01926, WBP::Extend},
    {0x01927, 0x01928, WBP::Extend},
    {0x01929, 0x0192B, WBP::Extend},
    {0x01930, 0x01931, WBP::Extend},
    {0x01932, 0x01932, WBP::Extend},
    {0x01933, 0x01938, WBP::Extend},
    {0x01939, 0x0193B, WBP::Extend},
    {0x01946, 0x0194F, WBP::Numeric},
    {0x019D0, 0x019D9, WBP::Numeric},
    {0x01A00, 0x01A16, WBP::ALetter},
    {0x01A17, 0x01A18, WBP::Extend},
    {0x01A19, 0x01A1A, WBP::Extend},
    {0x01A1B, 0x01A1B, WBP::Extend},
    {0x01A55, 0x01A55, WBP::Extend},
    {0x01A56, 0x01A56, WBP::Extend},
    {0x01A57, 0x01A57, WBP::Extend},
    {0x01A58, 0x01A5E, WBP::Extend},
    {0x01A60, 0x01A60, WBP::Extend},
    {0x01A61, 0x01A61, WBP::Extend},
    {0x01A62, 0x01A62, WBP::Extend},
    {0x01A63, 0x01A64, WBP::Extend},
    {0x01A65, 0x01A6C, WBP::Extend},
    {0x01A6D, 0x01A72, WBP::Extend},
    {0x01A73, 0x01A7C, WBP::Extend},
    {0x01A7F, 0x01A7F, WBP::Extend},
    {0x01A80, 0x01A89, WBP::Numeric},
    {0x01A90, 0x01A99, WBP::Numeric},
    {0x01AB0, 0x01ABD, WBP::Extend},
    {0x01ABE, 0x01ABE, WBP::Extend},
    {0x01ABF, 0x01AC0, WBP::Extend},
    {0x01B00, 0x01B03, WBP::Extend},
    {0x01B04, 0x01B04, WBP::Extend},
    {0x01B05, 0x01B33, WBP::ALetter},
    {0x01B34, 0x01B34, WBP::Extend},
    {0x01B35, 0x01B35, WBP::Extend},
    {0x01B36, 0x01B3A, WBP::Extend},
    {0x01B3B, 0x01B3B, WBP::Extend},
    {0x01B3C, 0x01B3C, WBP::Extend},
    {0x01B3D, 0x01B41, WBP::Extend},
    {0x01B42, 0x01B42, WBP::Extend},
    {0x01B43, 0x01B44, WBP::Extend},
    {0x01B45, 0x01B4B, WBP::ALetter},
    {0x01B50, 0x01B59, WBP::Numeric},
    {0x01B6B, 0x01B73, WBP::Extend},
    {0x01B80, 0x01B81, WBP::Extend},
    {0x01B82, 0x01B82, WBP::Extend},
    {0x01B83, 0x01BA0, WBP::ALetter},
    {0x01BA1, 0x01BA1, WBP::Extend},
    {0x01BA2, 0x01BA5, WBP::Extend},
    {0x01BA6, 0x01BA7, WBP::Extend},
    {0x01BA8, 0x01BA9, WBP::Extend},
    {0x01BAA, 0x01BAA, WBP::Extend},
    {0x01BAB, 0x01BAD, WBP::Extend},
    {0x01BAE, 0x01BAF, WBP::ALetter},
    {0x01BB0, 0x01BB9, WBP::Numeric},
    {0x01BBA, 0x01BE5, WBP::ALetter},
    {0x01BE6, 0x01BE6, WBP::Extend},
    {0x01BE7, 0x01BE7, WBP::Extend},
    {0x01BE8, 0x01BE9, WBP::Extend},
    {0x01BEA, 0x01BEC, WBP::Extend},
    {0x01BED, 0x01BED, WBP::Extend},
    {0x01BEE, 0x01BEE, WBP::Extend},
    {0x01BEF, 0x01BF1, WBP::Extend},
    {0x01BF2, 0x01BF3, WBP::Extend},
    {0x01C00, 0x01C23, WBP::ALetter},
    {0x01C24, 0x01C2B, WBP::Extend},
    {0x01C2C, 0x01C33, WBP::Extend},
    {0x01C34, 0x01C35, WBP::Extend},
    {0x01C36, 0x01C37, WBP::Extend},
    {0x01C40, 0x01C49, WBP::Numeric},
    {0x01C4D, 0x01C4F, WBP::ALetter},
    {0x01C50, 0x01C59, WBP::Numeric},
    {0x01C5A, 0x01C77, WBP::ALetter},
    {0x01C78, 0x01C7D, WBP::ALetter},
    {0x01C80, 0x01C88, WBP::ALetter},
    {0x01C90, 0x01CBA, WBP::ALetter},
    {0x01CBD, 0x01CBF, WBP::ALetter},
    {0x01CD0, 0x01CD2, WBP::Extend},
    {0x01CD4, 0x01CE0, WBP::Extend},
    {0x01CE1, 0x01CE1, WBP::Extend},
    {0x01CE2, 0x01CE8, WBP::Extend},
    {0x01CE9, 0x01CEC, WBP::ALetter},
    {0x01CED, 0x01CED, WBP::Extend},
    {0x01CEE, 0x01CF3, WBP::ALetter},
    {0x01CF4, 0x01CF4, WBP::Extend},
    {0x01CF5, 0x01CF6, WBP::ALetter},
    {0x01CF7, 0x01CF7, WBP::Extend},
    {0x01CF8, 0x01CF9, WBP::Extend},
    {0x01CFA, 0x01CFA, WBP::ALetter},
    {0x01D00, 0x01D2B, WBP::ALetter},
    {0x01D2C, 0x01D6A, WBP::ALetter},
    {0x01D6B, 0x01D77, WBP::ALetter},
    {0x01D78, 0x01D78, WBP::ALetter},
    {0x01D79, 0x01D9A, WBP::ALetter},
    {0x01D9B, 0x01DBF, WBP::ALetter},
    {0x01DC0, 0x01DF9, WBP::Extend},
    {0x01DFB, 0x01DFF, WBP::Extend},
    {0x01E00, 0x01F15, WBP::ALetter},
    {0x01F18, 0x01F1D, WBP::ALetter},
    {0x01F20, 0x01F45, WBP::ALetter},
    {0x01F48, 0x01F4D, WBP::ALetter},
    {0x01F50, 0x01F57, WBP::ALetter},
    {0x01F59, 0x01F59, WBP::ALetter},
    {0x01F5B, 0x01F5B, WBP::ALetter},
    {0x01F5D, 0x01F5D, WBP::ALetter},
    {0x01F5F, 0x01F7D, WBP::ALetter},
    {0x01F80, 0x01FB4, WBP::ALetter},
    {0x01FB6, 0x01FBC, WBP::ALetter},
    {0x01FBE, 0x01FBE, WBP::ALetter},
    {0x01FC2, 0x01FC4, WBP::ALetter},
    {0x01FC6, 0x01FCC, WBP::ALetter},
    {0x01FD0, 0x01FD3, WBP::ALetter},
    {0x01FD6, 0x01FDB, WBP::ALetter},
    {0x01FE0, 0x01FEC, WBP::ALetter},
    {0x01FF2, 0x01FF4, WBP::ALetter},
    {0x01FF6, 0x01FFC, WBP::ALetter},
    {0x02000, 0x02006, WBP::WSegSpace},
    {0x02008, 0x0200A, WBP::WSegSpace},
    {0x0200C, 0x0200C, WBP::Extend},
    {0x0200D, 0x0200D, WBP::ZWJ},
    {0x0200E, 0x0200F, WBP::Format},
    {0x02018, 0x02018, WBP::MidNumLet},
    {0x02019, 0x02019, WBP::MidNumLet},
    {0x02024, 0x02024, WBP::MidNumLet},
    {0x02027, 0x02027, WBP::MidLetter},
    {0x02028, 0x02028, WBP::Newline},
    {0x02029, 0x02029, WBP::Newline},
    {0x0202A, 0x0202E, WBP::Format},
    {0x0202F, 0x0202F, WBP::ExtendNumLet},
    {0x0203F, 0x02040, WBP::ExtendNumLet},
    {0x02044, 0x02044, WBP::MidNum},
    {0x02054, 0x02054, WBP::ExtendNumLet},
    {0x0205F, 0x0205F, WBP::WSegSpace},
    {0x02060, 0x02064, WBP::Format},
    {0x02066, 0x0206F, WBP::Format},
    {0x02071, 0x02071, WBP::ALetter},
    {0x0207F, 0x0207F, WBP::ALetter},
    {0x02090, 0x0209C, WBP::ALetter},
    {0x020D0, 0x020DC, WBP::Extend},
    {0x020DD, 0x020E0, WBP::Extend},
    {0x020E1, 0x020E1, WBP::Extend},
    {0x020E2, 0x020E4, WBP::Extend},
    {0x020E5, 0x020F0, WBP::Extend},
    {0x02102, 0x02102, WBP::ALetter},
    {0x02107, 0x02107, WBP::ALetter},
    {0x0210A, 0x02113, WBP::ALetter},
    {0x02115, 0x02115, WBP::ALetter},
    {0x02119, 0x0211D, WBP::ALetter},
    {0x02124, 0x02124, WBP::ALetter},
    {0x02126, 0x02126, WBP::ALetter},
    {0x02128, 0x02128, WBP::ALetter},
    {0x0212A, 0x0212D, WBP::ALetter},
    {0x0212F, 0x02134, WBP::ALetter},
    {0x02135, 0x02138, WBP::ALetter},
    {0x02139, 0x02139, WBP::ALetter},
    {0x0213C, 0x0213F, WBP::ALetter},
    {0x02145, 0x02149, WBP::ALetter},
    {0x0214E, 0x0214E, WBP::ALetter},
    {0x02160, 0x02182, WBP::ALetter},
    {0x02183, 0x02184, WBP::ALetter},
    {0x02185, 0x02188, WBP::ALetter},
    {0x024B6, 0x024E9, WBP::ALetter},
    {0x02C00, 0x02C2E, WBP::ALetter},
    {0x02C30, 0x02C5E, WBP::ALetter},
    {0x02C60, 0x02C7B, WBP::ALetter},
    {0x02C7C, 0x02C7D, WBP::ALetter},
    {0x02C7E, 0x02CE4, WBP::ALetter},
    {0x02CEB, 0x02CEE, WBP::ALetter},
    {0x02CEF, 0x02CF1, WBP::Extend},
    {0x02CF2, 0x02CF3, WBP::ALetter},
    {0x02D00, 0x02D25, WBP::ALetter},
    {0x02D27, 0x02D27, WBP::ALetter},
    {0x02D2D, 0x02D2D, WBP::ALetter},
    {0x02D30, 0x02D67, WBP::ALetter},
    {0x02D6F, 0x02D6F, WBP::ALetter},
    {0x02D7F, 0x02D7F, WBP::Extend},
    {0x02D80, 0x02D96, WBP::ALetter},
    {0x02DA0, 0x02DA6, WBP::ALetter},
    {0x02DA8, 0x02DAE, WBP::ALetter},
    {0x02DB0, 0x02DB6, WBP::ALetter},
    {0x02DB8, 0x02DBE, WBP::ALetter},
    {0x02DC0, 0x02DC6, WBP::ALetter},
    {0x02DC8, 0x02DCE, WBP::ALetter},
    {0x02DD0, 0x02DD6, WBP::ALetter},
    {0x02DD8, 0x02DDE, WBP::ALetter},
    {0x02DE0, 0x02DFF, WBP::Extend},
    {0x02E2F, 0x02E2F, WBP::ALetter},
    {0x03000, 0x03000, WBP::WSegSpace},
    {0x03005, 0x03005, WBP::ALetter},
    {0x0302A, 0x0302D, WBP::Extend},
    {0x0302E, 0x0302F, WBP::Extend},
    {0x03031, 0x03035, WBP::Katakana},
    {0x0303B, 0x0303B, WBP::ALetter},
    {0x0303C, 0x0303C, WBP::ALetter},
    {0x03099, 0x0309A, WBP::Extend},
    {0x0309B, 0x0309C, WBP::Katakana},
    {0x030A0, 0x030A0, WBP::Katakana},
    {0x030A1, 0x030FA, WBP::Katakana},
    {0x030FC, 0x030FE, WBP::Katakana},
    {0x030FF, 0x030FF, WBP::Katakana},
    {0x03105, 0x0312F, WBP::ALetter},
    {0x03131, 0x0318E, WBP::ALetter},
    {0x031A0, 0x031BF, WBP::ALetter},
    {0x031F0, 0x031FF, WBP::Katakana},
    {0x032D0, 0x032FE, WBP::Katakana},
    {0x03300, 0x03357, WBP::Katakana},
    {0x0A000, 0x0A014, WBP::ALetter},
    {0x0A015, 0x0A015, WBP::ALetter},
    {0x0A016, 0x0A48C, WBP::ALetter},
    {0x0A4D0, 0x0A4F7, WBP::ALetter},
    {0x0A4F8, 0x0A4FD, WBP::ALetter},
    {0x0A500, 0x0A60B, WBP::ALetter},
    {0x0A60C, 0x0A60C, WBP::ALetter},
    {0x0A610, 0x0A61F, WBP::ALetter},
    {0x0A620, 0x0A629, WBP::Numeric},
    {0x0A62A, 0x0A62B, WBP::ALetter},
    {0x0A640, 0x0A66D, WBP::ALetter},
    {0x0A66E, 0x0A66E, WBP::ALetter},
    {0x0A66F, 0x0A66F, WBP::Extend},
    {0x0A670, 0x0A672, WBP::Extend},
    {0x0A674, 0x0A67D, WBP::Extend},
    {0x0A67F, 0x0A67F, WBP::ALetter},
    {0x0A680, 0x0A69B, WBP::ALetter},
    {0x0A69C, 0x0A69D, WBP::ALetter},
    {0x0A69E, 0x0A69F, WBP::Extend},
    {0x0A6A0, 0x0A6E5, WBP::ALetter},
    {0x0A6E6, 0x0A6EF, WBP::ALetter},
    {0x0A6F0, 0x0A6F1, WBP::Extend},
    {0x0A708, 0x0A716, WBP::ALetter},
    {0x0A717, 0x0A71F, WBP::ALetter},
    {0x0A720, 0x0A721, WBP::ALetter},
    {0x0A722, 0x0A76F, WBP::ALetter},
    {0x0A770, 0x0A770, WBP::ALetter},
    {0x0A771, 0x0A787, WBP::ALetter},
    {0x0A788, 0x0A788, WBP::ALetter},
    {0x0A789, 0x0A78A, WBP::ALetter},
    {0x0A78B, 0x0A78E, WBP::ALetter},
    {0x0A78F, 0x0A78F, WBP::ALetter},
    {0x0A790, 0x0A7BF, WBP::ALetter},
    {0x0A7C2, 0x0A7CA, WBP::ALetter},
    {0x0A7F5, 0x0A7F6, WBP::ALetter},
    {0x0A7F7, 0x0A7F7, WBP::ALetter},
    {0x0A7F8, 0x0A7F9, WBP::ALetter},
    {0x0A7FA, 0x0A7FA, WBP::ALetter},
    {0x0A7FB, 0x0A801, WBP::ALetter},
    {0x0A802, 0x0A802, WBP::Extend},
    {0x0A803, 0x0A805, WBP::ALetter},
    {0x0A806, 0x0A806, WBP::Extend},
    {0x0A807, 0x0A80A, WBP::ALetter},
    {0x0A80B, 0x0A80B, WBP::Extend},
    {0x0A80C, 0x0A822, WBP::ALetter},
    {0x0A823, 0x0A824, WBP::Extend},
    {0x0A825, 0x0A826, WBP::Extend},
    {0x0A827, 0x0A827, WBP::Extend},
    {0x0A82C, 0x0A82C, WBP::Extend},
    {0x0A840, 0x0A873, WBP::ALetter},
    {0x0A880, 0x0A881, WBP::Extend},
    {0x0A882, 0x0A8B3, WBP::ALetter},
    {0x0A8B4, 0x0A8C3, WBP::Extend},
    {0x0A8C4, 0x0A8C5, WBP::Extend},
    {0x0A8D0, 0x0A8D9, WBP::Numeric},
    {0x0A8E0, 0x0A8F1, WBP::Extend},
    {0x0A8F2, 0x0A8F7, WBP::ALetter},
    {0x0A8FB, 0x0A8FB, WBP::ALetter},
    {0x0A8FD, 0x0A8FE, WBP::ALetter},
    {0x0A8FF, 0x0A8FF, WBP::Extend},
    {0x0A900, 0x0A909, WBP::Numeric},
    {0x0A90A, 0x0A925, WBP::ALetter},
    {0x0A926, 0x0A92D, WBP::Extend},
    {0x0A930, 0x0A946, WBP::ALetter},
    {0x0A947, 0x0A951, WBP::Extend},
    {0x0A952, 0x0A953, WBP::Extend},
    {0x0A960, 0x0A97C, WBP::ALetter},
    {0x0A980, 0x0A982, WBP::Extend},
    {0x0A983, 0x0A983, WBP::Extend},
    {0x0A984, 0x0A9B2, WBP::ALetter},
    {0x0A9B3, 0x0A9B3, WBP::Extend},
    {0x0A9B4, 0x0A9B5, WBP::Extend},
    {0x0A9B6, 0x0A9B9, WBP::Extend},
    {0x0A9BA, 0x0A9BB, WBP::Extend},
    {0x0A9BC, 0x0A9BD, WBP::Extend},
    {0x0A9BE, 0x0A9C0, WBP::Extend},
    {0x0A9CF, 0x0A9CF, WBP::ALetter},
    {0x0A9D0, 0x0A9D9, WBP::Numeric},
    {0x0A9E5, 0x0A9E5, WBP::Extend},
    {0x0A9F0, 0x0A9F9, WBP::Numeric},
    {0x0AA00, 0x0AA28, WBP::ALetter},
    {0x0AA29, 0x0AA2E, WBP::Extend},
    {0x0AA2F, 0x0AA30, WBP::Extend},
    {0x0AA31, 0x0AA32, WBP::Extend},
    {0x0AA33, 0x0AA34, WBP::Extend},
    {0x0AA35, 0x0AA36, WBP::Extend},
    {0x0AA40, 0x0AA42, WBP::ALetter},
    {0x0AA43, 0x0AA43, WBP::Extend},
    {0x0AA44, 0x0AA4B, WBP::ALetter},
    {0x0AA4C, 0x0AA4C, WBP::Extend},
    {0x0AA4D, 0x0AA4D, WBP::Extend},
    {0x0AA50, 0x0AA59, WBP::Numeric},
    {0x0AA7B, 0x0AA7B, WBP::Extend},
    {0x0AA7C, 0x0AA7C, WBP::Extend},
    {0x0AA7D, 0x0AA7D, WBP::Extend},
    {0x0AAB0, 0x0AAB0, WBP::Extend},
    {0x0AAB2, 0x0AAB4, WBP::Extend},
    {0x0AAB7, 0x0AAB8, WBP::Extend},
    {0x0AABE, 0x0AABF, WBP::Extend},
    {0x0AAC1, 0x0AAC1, WBP::Extend},
    {0x0AAE0, 0x0AAEA, WBP::ALetter},
    {0x0AAEB, 0x0AAEB, WBP::Extend},
    {0x0AAEC, 0x0AAED, WBP::Extend},
    {0x0AAEE, 0x0AAEF, WBP::Extend},
    {0x0AAF2, 0x0AAF2, WBP::ALetter},
    {0x0AAF3, 0x0AAF4, WBP::ALetter},
    {0x0AAF5, 0x0AAF5, WBP::Extend},
    {0x0AAF6, 0x0AAF6, WBP::Extend},
    {0x0AB01, 0x0AB06, WBP::ALetter},
    {0x0AB09, 0x0AB0E, WBP::ALetter},
    {0x0AB11, 0x0AB16, WBP::ALetter},
    {0x0AB20, 0x0AB26, WBP::ALetter},
    {0x0AB28, 0x0AB2E, WBP::ALetter},
    {0x0AB30, 0x0AB5A, WBP::ALetter},
    {0x0AB5B, 0x0AB5B, WBP::ALetter},
    {0x0AB5C, 0x0AB5F, WBP::ALetter},
    {0x0AB60, 0x0AB68, WBP::ALetter},
    {0x0AB69, 0x0AB69, WBP::ALetter},
    {0x0AB70, 0x0ABBF, WBP::ALetter},
    {0x0ABC0, 0x0ABE2, WBP::ALetter},
    {0x0ABE3, 0x0ABE4, WBP::Extend},
    {0x0ABE5, 0x0ABE5, WBP::Extend},
    {0x0ABE6, 0x0ABE7, WBP::Extend},
    {0x0ABE8, 0x0ABE8, WBP::Extend},
    {0x0ABE9, 0x0ABEA, WBP::Extend},
    {0x0ABEC, 0x0ABEC, WBP::Extend},
    {0x0ABED, 0x0ABED, WBP::Extend},
    {0x0ABF0, 0x0ABF9, WBP::Numeric},
    {0x0AC00, 0x0D7A3, WBP::ALetter},
    {0x0D7B0, 0x0D7C6, WBP::ALetter},
    {0x0D7CB, 0x0D7FB, WBP::ALetter},
    {0x0FB00, 0x0FB06, WBP::ALetter},
    {0x0FB13, 0x0FB17, WBP::ALetter},
    {0x0FB1D, 0x0FB1D, WBP::Hebrew_Letter},
    {0x0FB1E, 0x0FB1E, WBP::Extend},
    {0x0FB1F, 0x0FB28, WBP::Hebrew_Letter},
    {0x0FB2A, 0x0FB36, WBP::Hebrew_Letter},
    {0x0FB38, 0x0FB3C, WBP::Hebrew_Letter},
    {0x0FB3E, 0x0FB3E, WBP::Hebrew_Letter},
    {0x0FB40, 0x0FB41, WBP::Hebrew_Letter},
    {0x0FB43, 0x0FB44, WBP::Hebrew_Letter},
    {0x0FB46, 0x0FB4F, WBP::Hebrew_Letter},
    {0x0FB50, 0x0FBB1, WBP::ALetter},
    {0x0FBD3, 0x0FD3D, WBP::ALetter},
    {0x0FD50, 0x0FD8F, WBP::ALetter},
    {0x0FD92, 0x0FDC7, WBP::ALetter},
    {0x0FDF0, 0x0FDFB, WBP::ALetter},
    {0x0FE00, 0x0FE0F, WBP::Extend},
    {0x0FE10, 0x0FE10, WBP::MidNum},
    {0x0FE13, 0x0FE13, WBP::MidLetter},
    {0x0FE14, 0x0FE14, WBP::MidNum},
    {0x0FE20, 0x0FE2F, WBP::Extend},
    {0x0FE33, 0x0FE34, WBP::ExtendNumLet},
    {0x0FE4D, 0x0FE4F, WBP::ExtendNumLet},
    {0x0FE50, 0x0FE50, WBP::MidNum},
    {0x0FE52, 0x0FE52, WBP::MidNumLet},
    {0x0FE54, 0x0FE54, WBP::MidNum},
    {0x0FE55, 0x0FE55, WBP::MidLetter},
    {0x0FE70, 0x0FE74, WBP::ALetter},
    {0x0FE76, 0x0FEFC, WBP::ALetter},
    {0x0FEFF, 0x0FEFF, WBP::Format},
    {0x0FF07, 0x0FF07, WBP::MidNumLet},
    {0x0FF0C, 0x0FF0C, WBP::MidNum},
    {0x0FF0E, 0x0FF0E, WBP::MidNumLet},
    {0x0FF10, 0x0FF19, WBP::Numeric},
    {0x0FF1A, 0x0FF1A, WBP::MidLetter},
    {0x0FF1B, 0x0FF1B, WBP::MidNum},
    {0x0FF21, 0x0FF3A, WBP::ALetter},
    {0x0FF3F, 0x0FF3F, WBP::ExtendNumLet},
    {0x0FF41, 0x0FF5A, WBP::ALetter},
    {0x0FF66, 0x0FF6F, WBP::Katakana},
    {0x0FF70, 0x0FF70, WBP::Katakana},
    {0x0FF71, 0x0FF9D, WBP::Katakana},
    {0x0FF9E, 0x0FF9F, WBP::Extend},
    {0x0FFA0, 0x0FFBE, WBP::ALetter},
    {0x0FFC2, 0x0FFC7, WBP::ALetter},
    {0x0FFCA, 0x0FFCF, WBP::ALetter},
    {0x0FFD2, 0x0FFD7, WBP::ALetter},
    {0x0FFDA, 0x0FFDC, WBP::ALetter},
    {0x0FFF9, 0x0FFFB, WBP::Format},
    {0x10000, 0x1000B, WBP::ALetter},
    {0x1000D, 0x10026, WBP::ALetter},
    {0x10028, 0x1003A, WBP::ALetter},
    {0x1003C, 0x1003D, WBP::ALetter},
    {0x1003F, 0x1004D, WBP::ALetter},
    {0x10050, 0x1005D, WBP::ALetter},
    {0x10080, 0x100FA, WBP::ALetter},
    {0x10140, 0x10174, WBP::ALetter},
    {0x101FD, 0x101FD, WBP::Extend},
    {0x10280, 0x1029C, WBP::ALetter},
    {0x102A0, 0x102D0, WBP::ALetter},
    {0x102E0, 0x102E0, WBP::Extend},
    {0x10300, 0x1031F, WBP::ALetter},
    {0x1032D, 0x10340, WBP::ALetter},
    {0x10341, 0x10341, WBP::ALetter},
    {0x10342, 0x10349, WBP::ALetter},
    {0x1034A, 0x1034A, WBP::ALetter},
    {0x10350, 0x10375, WBP::ALetter},
    {0x10376, 0x1037A, WBP::Extend},
    {0x10380, 0x1039D, WBP::ALetter},
    {0x103A0, 0x103C3, WBP::ALetter},
    {0x103C8, 0x103CF, WBP::ALetter},
    {0x103D1, 0x103D5, WBP::ALetter},
    {0x10400, 0x1044F, WBP::ALetter},
    {0x10450, 0x1049D, WBP::ALetter},
    {0x104A0, 0x104A9, WBP::Numeric},
    {0x104B0, 0x104D3, WBP::ALetter},
    {0x104D8, 0x104FB, WBP::ALetter},
    {0x10500, 0x10527, WBP::ALetter},
    {0x10530, 0x10563, WBP::ALetter},
    {0x10600, 0x10736, WBP::ALetter},
    {0x10740, 0x10755, WBP::ALetter},
    {0x10760, 0x10767, WBP::ALetter},
    {0x10800, 0x10805, WBP::ALetter},
    {0x10808, 0x10808, WBP::ALetter},
    {0x1080A, 0x10835, WBP::ALetter},
    {0x10837, 0x10838, WBP::ALetter},
    {0x1083C, 0x1083C, WBP::ALetter},
    {0x1083F, 0x10855, WBP::ALetter},
    {0x10860, 0x10876, WBP::ALetter},
    {0x10880, 0x1089E, WBP::ALetter},
    {0x108E0, 0x108F2, WBP::ALetter},
    {0x108F4, 0x108F5, WBP::ALetter},
    {0x10900, 0x10915, WBP::ALetter},
    {0x10920, 0x10939, WBP::ALetter},
    {0x10980, 0x109B7, WBP::ALetter},
    {0x109BE, 0x109BF, WBP::ALetter},
    {0x10A00, 0x10A00, WBP::ALetter},
    {0x10A01, 0x10A03, WBP::Extend},
    {0x10A05, 0x10A06, WBP::Extend},
    {0x10A0C, 0x10A0F, WBP::Extend},
    {0x10A10, 0x10A13, WBP::ALetter},
    {0x10A15, 0x10A17, WBP::ALetter},
    {0x10A19, 0x10A35, WBP::ALetter},
    {0x10A38, 0x10A3A, WBP::Extend},
    {0x10A3F, 0x10A3F, WBP::Extend},
    {0x10A60, 0x10A7C, WBP::ALetter},
    {0x10A80, 0x10A9C, WBP::ALetter},
    {0x10AC0, 0x10AC7, WBP::ALetter},
    {0x10AC9, 0x10AE4, WBP::ALetter},
    {0x10AE5, 0x10AE6, WBP::Extend},
    {0x10B00, 0x10B35, WBP::ALetter},
    {0x10B40, 0x10B55, WBP::ALetter},
    {0x10B60, 0x10B72, WBP::ALetter},
    {0x10B80, 0x10B91, WBP::ALetter},
    {0x10C00, 0x10C48, WBP::ALetter},
    {0x10C80, 0x10CB2, WBP::ALetter},
    {0x10CC0, 0x10CF2, WBP::ALetter},
    {0x10D00, 0x10D23, WBP::ALetter},
    {0x10D24, 0x10D27, WBP::Extend},
    {0x10D30, 0x10D39, WBP::Numeric},
    {0x10E80, 0x10EA9, WBP::ALetter},
    {0x10EAB, 0x10EAC, WBP::Extend},
    {0x10EB0, 0x10EB1, WBP::ALetter},
    {0x10F00, 0x10F1C, WBP::ALetter},
    {0x10F27, 0x10F27, WBP::ALetter},
    {0x10F30, 0x10F45, WBP::ALetter},
    {0x10F46, 0x10F50, WBP::Extend},
    {0x10FB0, 0x10FC4, WBP::ALetter},
    {0x10FE0, 0x10FF6, WBP::ALetter},
    {0x11000, 0x11000, WBP::Extend},
    {0x11001, 0x11001, WBP::Extend},
    {0x11002, 0x11002, WBP::Extend},
    {0x11003, 0x11037, WBP::ALetter},
    {0x11038, 0x11046, WBP::Extend},
    {0x11066, 0x1106F, WBP::Numeric},
    {0x1107F, 0x11081, WBP::Extend},
    {0x11082, 0x11082, WBP::Extend},
    {0x11083, 0x110AF, WBP::ALetter},
    {0x110B0, 0x110B2, WBP::Extend},
    {0x110B3, 0x110B6, WBP::Extend},
    {0x110B7, 0x110B8, WBP::Extend},
    {0x110B9, 0x110BA, WBP::Extend},
    {0x110BD, 0x110BD, WBP::Format},
    {0x110CD, 0x110CD, WBP::Format},
    {0x110D0, 0x110E8, WBP::ALetter},
    {0x110F0, 0x110F9, WBP::Numeric},
    {0x11100, 0x11102, WBP::Extend},
    {0x11103, 0x11126, WBP::ALetter},
    {0x11127, 0x1112B, WBP::Extend},
    {0x1112C, 0x1112C, WBP::Extend},
    {0x1112D, 0x11134, WBP::Extend},
    {0x11136, 0x1113F, WBP::Numeric},
    {0x11144, 0x11144, WBP::ALetter},
    {0x11145, 0x11146, WBP::Extend},
    {0x11147, 0x11147, WBP::ALetter},
    {0x11150, 0x11172, WBP::ALetter},
    {0x11173, 0x11173, WBP::Extend},
    {0x11176, 0x11176, WBP::ALetter},
    {0x11180, 0x11181, WBP::Extend},
    {0x11182, 0x11182, WBP::Extend},
    {0x11183, 0x111B2, WBP::ALetter},
    {0x111B3, 0x111B5, WBP::Extend},
    {0x111B6, 0x111BE, WBP::Extend},
    {0x111BF, 0x111C0, WBP::Extend},
    {0x111C1, 0x111C4, WBP::ALetter},
    {0x111C9, 0x111CC, WBP::Extend},
    {0x111CE, 0x111CE, WBP::Extend},
    {0x111CF, 0x111CF, WBP::Extend},
    {0x111D0, 0x111D9, WBP::Numeric},
    {0x111DA, 0x111DA, WBP::ALetter},
    {0x111DC, 0x111DC, WBP::ALetter},
    {0x11200, 0x11211, WBP::ALetter},
    {0x11213, 0x1122B, WBP::ALetter},
    {0x1122C, 0x1122E, WBP::Extend},
    {0x1122F, 0x11231, WBP::Extend},
    {0x11232, 0x11233, WBP::Extend},
    {0x11234, 0x11234, WBP::Extend},
    {0x11235, 0x11235, WBP::Extend},
    {0x11236, 0x11237, WBP::Extend},
    {0x1123E, 0x1123E, WBP::Extend},
    {0x11280, 0x11286, WBP::ALetter},
    {0x11288, 0x11288, WBP::ALetter},
    {0x1128A, 0x1128D, WBP::ALetter},
    {0x1128F, 0x1129D, WBP::ALetter},
    {0x1129F, 0x112A8, WBP::ALetter},
    {0x112B0, 0x112DE, WBP::ALetter},
    {0x112DF, 0x112DF, WBP::Extend},
    {0x112E0, 0x112E2, WBP::Extend},
    {0x112E3, 0x112EA, WBP::Extend},
    {0x112F0, 0x112F9, WBP::Numeric},
    {0x11300, 0x11301, WBP::Extend},
    {0x11302, 0x11303, WBP::Extend},
    {0x11305, 0x1130C, WBP::ALetter},
    {0x1130F, 0x11310, WBP::ALetter},
    {0x11313, 0x11328, WBP::ALetter},
    {0x1132A, 0x11330, WBP::ALetter},
    {0x11332, 0x11333, WBP::ALetter},
    {0x11335, 0x11339, WBP::ALetter},
    {0x1133B, 0x1133C, WBP::Extend},
    {0x1133D, 0x1133D, WBP::ALetter},
    {0x1133E, 0x1133F, WBP::Extend},
    {0x11340, 0x11340, WBP::Extend},
    {0x11341, 0x11344, WBP::Extend},
    {0x11347, 0x11348, WBP::Extend},
    {0x1134B, 0x1134D, WBP::Extend},
    {0x11350, 0x11350, WBP::ALetter},
    {0x11357, 0x11357, WBP::Extend},
    {0x1135D, 0x11361, WBP::ALetter},
    {0x11362, 0x11363, WBP::Extend},
    {0x11366, 0x1136C, WBP::Extend},
    {0x11370, 0x11374, WBP::Extend},
    {0x11400, 0x11434, WBP::ALetter},
    {0x11435, 0x11437, WBP::Extend},
    {0x11438, 0x1143F, WBP::Extend},
    {0x11440, 0x11441, WBP::Extend},
    {0x11442, 0x11444, WBP::Extend},
    {0x11445, 0x11445, WBP::Extend},
    {0x11446, 0x11446, WBP::Extend},
    {0x11447, 0x1144A, WBP::ALetter},
    {0x11450, 0x11459, WBP::Numeric},
    {0x1145E, 0x1145E, WBP::Extend},
    {0x1145F, 0x11461, WBP::ALetter},
    {0x11480, 0x114AF, WBP::ALetter},
    {0x114B0, 0x114B2, WBP::Extend},
    {0x114B3, 0x114B8, WBP::Extend},
    {0x114B9, 0x114B9, WBP::Extend},
    {0x114BA, 0x114BA, WBP::Extend},
    {0x114BB, 0x114BE, WBP::Extend},
    {0x114BF, 0x114C0, WBP::Extend},
    {0x114C1, 0x114C1, WBP::Extend},
    {0x114C2, 0x114C3, WBP::Extend},
    {0x114C4, 0x114C5, WBP::ALetter},
    {0x114C7, 0x114C7, WBP::ALetter},
    {0x114D0, 0x114D9, WBP::Numeric},
    {0x11580, 0x115AE, WBP::ALetter},
    {0x115AF, 0x115B1, WBP::Extend},
    {0x115B2, 0x115B5, WBP::Extend},
    {0x115B8, 0x115BB, WBP::Extend},
    {0x115BC, 0x115BD, WBP::Extend},
    {0x115BE, 0x115BE, WBP::Extend},
    {0x115BF, 0x115C0, WBP::Extend},
    {0x115D8, 0x115DB, WBP::ALetter},
    {0x115DC, 0x115DD, WBP::Extend},
    {0x11600, 0x1162F, WBP::ALetter},
    {0x11630, 0x11632, WBP::Extend},
    {0x11633, 0x1163A, WBP::Extend},
    {0x1163B, 0x1163C, WBP::Extend},
    {0x1163D, 0x1163D, WBP::Extend},
    {0x1163E, 0x1163E, WBP::Extend},
    {0x1163F, 0x11640, WBP::Extend},
    {0x11644, 0x11644, WBP::ALetter},
    {0x11650, 0x11659, WBP::Numeric},
    {0x11680, 0x116AA, WBP::ALetter},
    {0x116AB, 0x116AB, WBP::Extend},
    {0x116AC, 0x116AC, WBP::Extend},
    {0x116AD, 0x116AD, WBP::Extend},
    {0x116AE, 0x116AF, WBP::Extend},
    {0x116B0, 0x116B5, WBP::Extend},
    {0x116B6, 0x116B6, WBP::Extend},
    {0x116B7, 0x116B7, WBP::Extend},
    {0x116B8, 0x116B8, WBP::ALetter},
    {0x116C0, 0x116C9, WBP::Numeric},
    {0x1171D, 0x1171F, WBP::Extend},
    {0x11720, 0x11721, WBP::Extend},
    {0x11722, 0x11725, WBP::Extend},
    {0x11726, 0x11726, WBP::Extend},
    {0x11727, 0x1172B, WBP::Extend},
    {0x11730, 0x11739, WBP::Numeric},
    {0x11800, 0x1182B, WBP::ALetter},
    {0x1182C, 0x1182E, WBP::Extend},
    {0x1182F, 0x11837, WBP::Extend},
    {0x11838, 0x11838, WBP::Extend},
    {0x11839, 0x1183A, WBP::Extend},
    {0x118A0, 0x118DF, WBP::ALetter},
    {0x118E0, 0x118E9, WBP::Numeric},
    {0x118FF, 0x11906, WBP::ALetter},
    {0x11909, 0x11909, WBP::ALetter},
    {0x1190C, 0x11913, WBP::ALetter},
    {0x11915, 0x11916, WBP::ALetter},
    {0x11918, 0x1192F, WBP::ALetter},
    {0x11930, 0x11935, WBP::Extend},
    {0x11937, 0x11938, WBP::Extend},
    {0x1193B, 0x1193C, WBP::Extend},
    {0x1193D, 0x1193D, WBP::Extend},
    {0x1193E, 0x1193E, WBP::Extend},
    {0x1193F, 0x1193F, WBP::ALetter},
    {0x11940, 0x11940, WBP::Extend},
    {0x11941, 0x11941, WBP::ALetter},
    {0x11942, 0x11942, WBP::Extend},
    {0x11943, 0x11943, WBP::Extend},
    {0x11950, 0x11959, WBP::Numeric},
    {0x119A0, 0x119A7, WBP::ALetter},
    {0x119AA, 0x119D0, WBP::ALetter},
    {0x119D1, 0x119D3, WBP::Extend},
    {0x119D4, 0x119D7, WBP::Extend},
    {0x119DA, 0x119DB, WBP::Extend},
    {0x119DC, 0x119DF, WBP::Extend},
    {0x119E0, 0x119E0, WBP::Extend},
    {0x119E1, 0x119E1, WBP::ALetter},
    {0x119E3, 0x119E3, WBP::ALetter},
    {0x119E4, 0x119E4, WBP::Extend},
    {0x11A00, 0x11A00, WBP::ALetter},
    {0x11A01, 0x11A0A, WBP::Extend},
    {0x11A0B, 0x11A32, WBP::ALetter},
    {0x11A33, 0x11A38, WBP::Extend},
    {0x11A39, 0x11A39, WBP::Extend},
    {0x11A3A, 0x11A3A, WBP::ALetter},
    {0x11A3B, 0x11A3E, WBP::Extend},
    {0x11A47, 0x11A47, WBP::Extend},
    {0x11A50, 0x11A50, WBP::ALetter},
    {0x11A51, 0x11A56, WBP::Extend},
    {0x11A57, 0x11A58, WBP::Extend},
    {0x11A59, 0x11A5B, WBP::Extend},
    {0x11A5C, 0x11A89, WBP::ALetter},
    {0x11A8A, 0x11A96, WBP::Extend},
    {0x11A97, 0x11A97, WBP::Extend},
    {0x11A98, 0x11A99, WBP::Extend},
    {0x11A9D, 0x11A9D, WBP::ALetter},
    {0x11AC0, 0x11AF8, WBP::ALetter},
    {0x11C00, 0x11C08, WBP::ALetter},
    {0x11C0A, 0x11C2E, WBP::ALetter},
    {0x11C2F, 0x11C2F, WBP::Extend},
    {0x11C30, 0x11C36, WBP::Extend},
    {0x11C38, 0x11C3D, WBP::Extend},
    {0x11C3E, 0x11C3E, WBP::Extend},
    {0x11C3F, 0x11C3F, WBP::Extend},
    {0x11C40, 0x11C40, WBP::ALetter},
    {0x11C50, 0x11C59, WBP::Numeric},
    {0x11C72, 0x11C8F, WBP::ALetter},
    {0x11C92, 0x11CA7, WBP::Extend},
    {0x11CA9, 0x11CA9, WBP::Extend},
    {0x11CAA, 0x11CB0, WBP::Extend},
    {0x11CB1, 0x11CB1, WBP::Extend},
    {0x11CB2, 0x11CB3, WBP::Extend},
    {0x11CB4, 0x11CB4, WBP::Extend},
    {0x11CB5, 0x11CB6, WBP::Extend},
    {0x11D00, 0x11D06, WBP::ALetter},
    {0x11D08, 0x11D09, WBP::ALetter},
    {0x11D0B, 0x11D30, WBP::ALetter},
    {0x11D31, 0x11D36, WBP::Extend},
    {0x11D3A, 0x11D3A, WBP::Extend},
    {0x11D3C, 0x11D3D, WBP::Extend},
    {0x11D3F, 0x11D45, WBP::Extend},
    {0x11D46, 0x11D46, WBP::ALetter},
    {0x11D47, 0x11D47, WBP::Extend},
    {0x11D50, 0x11D59, WBP::Numeric},
    {0x11D60, 0x11D65, WBP::ALetter},
    {0x11D67, 0x11D68, WBP::ALetter},
    {0x11D6A, 0x11D89, WBP::ALetter},
    {0x11D8A, 0x11D8E, WBP::Extend},
    {0x11D90, 0x11D91, WBP::Extend},
    {0x11D93, 0x11D94, WBP::Extend},
    {0x11D95, 0x11D95, WBP::Extend},
    {0x11D96, 0x11D96, WBP::Extend},
    {0x11D97, 0x11D97, WBP::Extend},
    {0x11D98, 0x11D98, WBP::ALetter},
    {0x11DA0, 0x11DA9, WBP::Numeric},
    {0x11EE0, 0x11EF2, WBP::ALetter},
    {0x11EF3, 0x11EF4, WBP::Extend},
    {0x11EF5, 0x11EF6, WBP::Extend},
    {0x11FB0, 0x11FB0, WBP::ALetter},
    {0x12000, 0x12399, WBP::ALetter},
    {0x12400, 0x1246E, WBP::ALetter},
    {0x12480, 0x12543, WBP::ALetter},
    {0x13000, 0x1342E, WBP::ALetter},
    {0x13430, 0x13438, WBP::Format},
    {0x14400, 0x14646, WBP::ALetter},
    {0x16800, 0x16A38, WBP::ALetter},
    {0x16A40, 0x16A5E, WBP::ALetter},
    {0x16A60, 0x16A69, WBP::Numeric},
    {0x16AD0, 0x16AED, WBP::ALetter},
    {0x16AF0, 0x16AF4, WBP::Extend},
    {0x16B00, 0x16B2F, WBP::ALetter},
    {0x16B30, 0x16B36, WBP::Extend},
    {0x16B40, 0x16B43, WBP::ALetter},
    {0x16B50, 0x16B59, WBP::Numeric},
    {0x16B63, 0x16B77, WBP::ALetter},
    {0x16B7D, 0x16B8F, WBP::ALetter},
    {0x16E40, 0x16E7F, WBP::ALetter},
    {0x16F00, 0x16F4A, WBP::ALetter},
    {0x16F4F, 0x16F4F, WBP::Extend},
    {0x16F50, 0x16F50, WBP::ALetter},
    {0x16F51, 0x16F87, WBP::Extend},
    {0x16F8F, 0x16F92, WBP::Extend},
    {0x16F93, 0x16F9F, WBP::ALetter},
    {0x16FE0, 0x16FE1, WBP::ALetter},
    {0x16FE3, 0x16FE3, WBP::ALetter},
    {0x16FE4, 0x16FE4, WBP::Extend},
    {0x16FF0, 0x16FF1, WBP::Extend},
    {0x1B000, 0x1B000, WBP::Katakana},
    {0x1B164, 0x1B167, WBP::Katakana},
    {0x1BC00, 0x1BC6A, WBP::ALetter},
    {0x1BC70, 0x1BC7C, WBP::ALetter},
    {0x1BC80, 0x1BC88, WBP::ALetter},
    {0x1BC90, 0x1BC99, WBP::ALetter},
    {0x1BC9D, 0x1BC9E, WBP::Extend},
    {0x1BCA0, 0x1BCA3, WBP::Format},
    {0x1D165, 0x1D166, WBP::Extend},
    {0x1D167, 0x1D169, WBP::Extend},
    {0x1D16D, 0x1D172, WBP::Extend},
    {0x1D173, 0x1D17A, WBP::Format},
    {0x1D17B, 0x1D182, WBP::Extend},
    {0x1D185, 0x1D18B, WBP::Extend},
    {0x1D1AA, 0x1D1AD, WBP::Extend},
    {0x1D242, 0x1D244, WBP::Extend},
    {0x1D400, 0x1D454, WBP::ALetter},
    {0x1D456, 0x1D49C, WBP::ALetter},
    {0x1D49E, 0x1D49F, WBP::ALetter},
    {0x1D4A2, 0x1D4A2, WBP::ALetter},
    {0x1D4A5, 0x1D4A6, WBP::ALetter},
    {0x1D4A9, 0x1D4AC, WBP::ALetter},
    {0x1D4AE, 0x1D4B9, WBP::ALetter},
    {0x1D4BB, 0x1D4BB, WBP::ALetter},
    {0x1D4BD, 0x1D4C3, WBP::ALetter},
    {0x1D4C5, 0x1D505, WBP::ALetter},
    {0x1D507, 0x1D50A, WBP::ALetter},
    {0x1D50D, 0x1D514, WBP::ALetter},
    {0x1D516, 0x1D51C, WBP::ALetter},
    {0x1D51E, 0x1D539, WBP::ALetter},
    {0x1D53B, 0x1D53E, WBP::ALetter},
    {0x1D540, 0x1D544, WBP::ALetter},
    {0x1D546, 0x1D546, WBP::ALetter},
    {0x1D54A, 0x1D550, WBP::ALetter},
    {0x1D552, 0x1D6A5, WBP::ALetter},
    {0x1D6A8, 0x1D6C0, WBP::ALetter},
    {0x1D6C2, 0x1D6DA, WBP::ALetter},
    {0x1D6DC, 0x1D6FA, WBP::ALetter},
    {0x1D6FC, 0x1D714, WBP::ALetter},
    {0x1D716, 0x1D734, WBP::ALetter},
    {0x1D736, 0x1D74E, WBP::ALetter},
    {0x1D750, 0x1D76E, WBP::ALetter},
    {0x1D770, 0x1D788, WBP::ALetter},
    {0x1D78A, 0x1D7A8, WBP::ALetter},
    {0x1D7AA, 0x1D7C2, WBP::ALetter},
    {0x1D7C4, 0x1D7CB, WBP::ALetter},
    {0x1D7CE, 0x1D7FF, WBP::Numeric},
    {0x1DA00, 0x1DA36, WBP::Extend},
    {0x1DA3B, 0x1DA6C, WBP::Extend},
    {0x1DA75, 0x1DA75, WBP::Extend},
    {0x1DA84, 0x1DA84, WBP::Extend},
    {0x1DA9B, 0x1DA9F, WBP::Extend},
    {0x1DAA1, 0x1DAAF, WBP::Extend},
    {0x1E000, 0x1E006, WBP::Extend},
    {0x1E008, 0x1E018, WBP::Extend},
    {0x1E01B, 0x1E021, WBP::Extend},
    {0x1E023, 0x1E024, WBP::Extend},
    {0x1E026, 0x1E02A, WBP::Extend},
    {0x1E100, 0x1E12C, WBP::ALetter},
    {0x1E130, 0x1E136, WBP::Extend},
    {0x1E137, 0x1E13D, WBP::ALetter},
    {0x1E140, 0x1E149, WBP::Numeric},
    {0x1E14E, 0x1E14E, WBP::ALetter},
    {0x1E2C0, 0x1E2EB, WBP::ALetter},
    {0x1E2EC, 0x1E2EF, WBP::Extend},
    {0x1E2F0, 0x1E2F9, WBP::Numeric},
    {0x1E800, 0x1E8C4, WBP::ALetter},
    {0x1E8D0, 0x1E8D6, WBP::Extend},
    {0x1E900, 0x1E943, WBP::ALetter},
    {0x1E944, 0x1E94A, WBP::Extend},
    {0x1E94B, 0x1E94B, WBP::ALetter},
    {0x1E950, 0x1E959, WBP::Numeric},
    {0x1EE00, 0x1EE03, WBP::ALetter},
    {0x1EE05, 0x1EE1F, WBP::ALetter},
    {0x1EE21, 0x1EE22, WBP::ALetter},
    {0x1EE24, 0x1EE24, WBP::ALetter},
    {0x1EE27, 0x1EE27, WBP::ALetter},
    {0x1EE29, 0x1EE32, WBP::ALetter},
    {0x1EE34, 0x1EE37, WBP::ALetter},
    {0x1EE39, 0x1EE39, WBP::ALetter},
    {0x1EE3B, 0x1EE3B, WBP::ALetter},
    {0x1EE42, 0x1EE42, WBP::ALetter},
    {0x1EE47, 0x1EE47, WBP::ALetter},
    {0x1EE49, 0x1EE49, WBP::ALetter},
    {0x1EE4B, 0x1EE4B, WBP::ALetter},
    {0x1EE4D, 0x1EE4F, WBP::ALetter},
    {0x1EE51, 0x1EE52, WBP::ALetter},
    {0x1EE54, 0x1EE54, WBP::ALetter},
    {0x1EE57, 0x1EE57, WBP::ALetter},
    {0x1EE59, 0x1EE59, WBP::ALetter},
    {0x1EE5B, 0x1EE5B, WBP::ALetter},
    {0x1EE5D, 0x1EE5D, WBP::ALetter},
    {0x1EE5F, 0x1EE5F, WBP::ALetter},
    {0x1EE61, 0x1EE62, WBP::ALetter},
    {0x1EE64, 0x1EE64, WBP::ALetter},
    {0x1EE67, 0x1EE6A, WBP::ALetter},
    {0x1EE6C, 0x1EE72, WBP::ALetter},
    {0x1EE74, 0x1EE77, WBP::ALetter},
    {0x1EE79, 0x1EE7C, WBP::ALetter},
    {0x1EE7E, 0x1EE7E, WBP::ALetter},
    {0x1EE80, 0x1EE89, WBP::ALetter},
    {0x1EE8B, 0x1EE9B, WBP::ALetter},
    {0x1EEA1, 0x1EEA3, WBP::ALetter},
    {0x1EEA5, 0x1EEA9, WBP::ALetter},
    {0x1EEAB, 0x1EEBB, WBP::ALetter},
    {0x1F130, 0x1F149, WBP::ALetter},
    {0x1F150, 0x1F169, WBP::ALetter},
    {0x1F170, 0x1F189, WBP::ALetter},
    {0x1F1E6, 0x1F1FF, WBP::Regional_Indicator},
    {0x1F3FB, 0x1F3FF, WBP::Extend},
    {0x1FBF0, 0x1FBF9, WBP::Numeric},
    {0xE0001, 0xE0001, WBP::Format},
    {0xE0020, 0xE007F, WBP::Extend},
    {0xE0100, 0xE01EF, WBP::Extend},
}};

// Find a codepoint inside a sorted list of Interval.
template <size_t N>
bool Bisearch(uint32_t ucs, const std::array<Interval, N> table) {
  if (ucs < table.front().first || ucs > table.back().last) {  // NOLINT
    return false;
  }

  int min = 0;
  int max = N - 1;
  while (max >= min) {
    const int mid = (min + max) / 2;
    if (ucs > table[mid].last) {  // NOLINT
      min = mid + 1;
    } else if (ucs < table[mid].first) {  // NOLINT
      max = mid - 1;
    } else {
      return true;
    }
  }

  return false;
}

// Find a value inside a sorted list of Interval + property.
template <class C, size_t N>
bool Bisearch(uint32_t ucs, const std::array<C, N> table, C* out) {
  if (ucs < table.front().first || ucs > table.back().last) {  // NOLINT
    return false;
  }

  int min = 0;
  int max = N - 1;
  while (max >= min) {
    const int mid = (min + max) / 2;
    if (ucs > table[mid].last) {  // NOLINT
      min = mid + 1;
    } else if (ucs < table[mid].first) {  // NOLINT
      max = mid - 1;
    } else {
      *out = table[mid];  // NOLINT
      return true;
    }
  }

  return false;
}

bool IsCombining(uint32_t ucs) {
  return Bisearch(ucs, g_combining_characters);
}

bool IsFullWidth(uint32_t ucs) {
  if (ucs < 0x0300)  // Quick path: // NOLINT
    return false;

  return Bisearch(ucs, g_full_width_characters);
}

bool IsControl(uint32_t ucs) {
  if (ucs == 0) {
    return true;
  }
  if (ucs < 32) {  // NOLINT
    return true;
  }
  if (ucs >= 0x7f && ucs < 0xa0) {  // NOLINT
    return true;
  }
  return false;
}

int codepoint_width(uint32_t ucs) {
  if (IsControl(ucs)) {
    return -1;
  }

  if (IsCombining(ucs)) {
    return 0;
  }

  if (IsFullWidth(ucs)) {
    return 2;
  }

  return 1;
}

// From UTF8 encoded string |input|, eat in between 1 and 4 byte representing
// one codepoint. Put the codepoint into |ucs|. Start at |start| and update
// |end| to represent the beginning of the next byte to eat for consecutive
// executions.
bool EatCodePoint(const std::string& input,
                  size_t start,
                  size_t* end,
                  uint32_t* ucs) {
  if (start >= input.size()) {
    *end = start + 1;
    return false;
  }
  const uint8_t C0 = input[start];

  // 1 byte string.
  if ((C0 & 0x80) == 0x0) {  // NOLINT
    *ucs = C0 & 0x7f;                // NOLINT
    *end = start + 1;
    return true;
  }

  // 2 byte string.
  if ((C0 & 0xe0) == 0xc0 &&  // NOLINT
      start + 1 < input.size()) {
    const uint8_t C1 = input[start + 1];
    *ucs = 0;
    *ucs += C0 & 0x1f;  // NOLINT
    *ucs <<= 6;                // NOLINT
    *ucs += C1 & 0x3f;  // NOLINT
    *end = start + 2;
    return true;
  }

  // 3 byte string.
  if ((C0 & 0xf0) == 0xe0 &&  // NOLINT
      start + 2 < input.size()) {
    const uint8_t C1 = input[start + 1];
    const uint8_t C2 = input[start + 2];
    *ucs = 0;
    *ucs += C0 & 0xf;  // NOLINT
    *ucs <<= 6;                // NOLINT
    *ucs += C1 & 0x3f;  // NOLINT
    *ucs <<= 6;                // NOLINT
    *ucs += C2 & 0x3f;  // NOLINT
    *end = start + 3;
    return true;
  }

  // 4 byte string.
  if ((C0 & 0xf8) == 0xf0 &&  // NOLINT
      start + 3 < input.size()) {
    const uint8_t C1 = input[start + 1];
    const uint8_t C2 = input[start + 2];
    const uint8_t C3 = input[start + 3];
    *ucs = 0;
    *ucs += C0 & 0x7;  // NOLINT
    *ucs <<= 6;                // NOLINT
    *ucs += C1 & 0x3f;  // NOLINT
    *ucs <<= 6;                // NOLINT
    *ucs += C2 & 0x3f;  // NOLINT
    *ucs <<= 6;                // NOLINT
    *ucs += C3 & 0x3f;  // NOLINT
    *end = start + 4;
    return true;
  }

  *end = start + 1;
  return false;
}

// From UTF16 encoded string |input|, eat in between 1 and 4 byte representing
// one codepoint. Put the codepoint into |ucs|. Start at |start| and update
// |end| to represent the beginning of the next byte to eat for consecutive
// executions.
bool EatCodePoint(const std::wstring& input,
                  size_t start,
                  size_t* end,
                  uint32_t* ucs) {
  if (start >= input.size()) {
    *end = start + 1;
    return false;
  }

  // On linux wstring uses the UTF32 encoding:
  if (sizeof(wchar_t) == 4) {
    *ucs = input[start];  // NOLINT
    *end = start + 1;
    return true;
  }

  // On windows, wstring uses the UTF16 encoding:
  int32_t C0 = input[start];  // NOLINT

  // 1 word size:
  if (C0 < 0xd800 || C0 >= 0xdc00) {  // NOLINT
    *ucs = C0;
    *end = start + 1;
    return true;
  }

  // 2 word size:
  if (start + 1 >= input.size()) {
    *end = start + 2;
    return false;
  }

  int32_t C1 = input[start + 1];                         // NOLINT
  *ucs = ((C0 & 0x3ff) << 10) + (C1 & 0x3ff) + 0x10000;  // NOLINT
  *end = start + 2;
  return true;
}

}  // namespace

namespace ftxui {
int wchar_width(wchar_t ucs) {
  return codepoint_width(uint32_t(ucs));
}

int wstring_width(const std::wstring& text) {
  int width = 0;

  for (const wchar_t& it : text) {
    const int w = wchar_width(it);
    if (w < 0) {
      return -1;
    }
    width += w;
  }
  return width;
}

int string_width(const std::string& input) {
  int width = 0;
  size_t start = 0;
  while (start < input.size()) {
    uint32_t codepoint = 0;
    if (!EatCodePoint(input, start, &start, &codepoint)) {
      continue;
    }

    if (IsControl(codepoint)) {
      continue;
    }

    if (IsCombining(codepoint)) {
      continue;
    }

    if (IsFullWidth(codepoint)) {
      width += 2;
      continue;
    }

    width += 1;
  }
  return width;
}

std::vector<std::string> Utf8ToGlyphs(const std::string& input) {
  std::vector<std::string> out;
  const std::string current;
  out.reserve(input.size());
  size_t start = 0;
  size_t end = 0;
  while (start < input.size()) {
    uint32_t codepoint = 0;
    if (!EatCodePoint(input, start, &end, &codepoint)) {
      start = end;
      continue;
    }

    const std::string append = input.substr(start, end - start);
    start = end;

    // Ignore control characters.
    if (IsControl(codepoint)) {
      continue;
    }

    // Combining characters are put with the previous glyph they are modifying.
    if (IsCombining(codepoint)) {
      if (!out.empty()) {
        out.back() += append;
      }
      continue;
    }

    // Fullwidth characters take two cells. The second is made of the empty
    // string to reserve the space the first is taking.
    if (IsFullWidth(codepoint)) {
      out.push_back(append);
      out.emplace_back("");
      continue;
    }

    // Normal characters:
    out.push_back(append);
  }
  return out;
}

int GlyphPosition(const std::string& input, size_t glyph_index, size_t start) {
  if (glyph_index <= 0) {
    return 0;
  }
  size_t end = 0;
  while (start < input.size()) {
    uint32_t codepoint = 0;
    const bool eaten = EatCodePoint(input, start, &end, &codepoint);

    // Ignore invalid, control characters and combining characters.
    if (!eaten || IsControl(codepoint) || IsCombining(codepoint)) {
      start = end;
      continue;
    }

    // We eat the beginning of the next glyph. If we are eating the one
    // requested, return its start position immediately.
    if (glyph_index == 0) {
      return static_cast<int>(start);
    }

    // Otherwise, skip this glyph and iterate:
    glyph_index--;
    start = end;
  }
  return static_cast<int>(input.size());
}

std::vector<int> CellToGlyphIndex(const std::string& input) {
  int x = -1;
  std::vector<int> out;
  out.reserve(input.size());
  size_t start = 0;
  size_t end = 0;
  while (start < input.size()) {
    uint32_t codepoint = 0;
    const bool eaten = EatCodePoint(input, start, &end, &codepoint);
    start = end;

    // Ignore invalid / control characters.
    if (!eaten || IsControl(codepoint)) {
      continue;
    }

    // Combining characters are put with the previous glyph they are modifying.
    if (IsCombining(codepoint)) {
      if (x == -1) {
        ++x;
        out.push_back(x);
      }
      continue;
    }

    // Fullwidth characters take two cells. The second is made of the empty
    // string to reserve the space the first is taking.
    if (IsFullWidth(codepoint)) {
      ++x;
      out.push_back(x);
      out.push_back(x);
      continue;
    }

    // Normal characters:
    ++x;
    out.push_back(x);
  }
  return out;
}

int GlyphCount(const std::string& input) {
  int size = 0;
  size_t start = 0;
  size_t end = 0;
  while (start < input.size()) {
    uint32_t codepoint = 0;
    const bool eaten = EatCodePoint(input, start, &end, &codepoint);
    start = end;

    // Ignore invalid characters:
    if (!eaten || IsControl(codepoint)) {
      continue;
    }

    // Ignore combining characters, except when they don't have a preceding to
    // combine with.
    if (IsCombining(codepoint)) {
      if (size == 0) {
        size++;
      }
      continue;
    }

    size++;
  }
  return size;
}

std::vector<WordBreakProperty> Utf8ToWordBreakProperty(
    const std::string& input) {
  std::vector<WordBreakProperty> out;
  out.reserve(input.size());
  size_t start = 0;
  size_t end = 0;
  while (start < input.size()) {
    uint32_t codepoint = 0;
    if (!EatCodePoint(input, start, &end, &codepoint)) {
      start = end;
      continue;
    }
    start = end;

    // Ignore control characters.
    if (IsControl(codepoint)) {
      continue;
    }

    // Ignore combining characters.
    if (IsCombining(codepoint)) {
      continue;
    }

    WordBreakPropertyInterval interval = {0, 0, WBP::ALetter};
    std::ignore = Bisearch(codepoint, g_word_break_intervals, &interval);
    out.push_back(interval.property);
  }
  return out;
}

/// Convert a UTF8 std::string into a std::wstring.
std::string to_string(const std::wstring& s) {
  std::string out;

  size_t i = 0;
  uint32_t codepoint = 0;
  while (EatCodePoint(s, i, &i, &codepoint)) {
    // Code point <-> UTF-8 conversion
    //
    // ┏━━━━━━━━┳━━━━━━━━┳━━━━━━━━┳━━━━━━━━┓
    // ┃Byte 1  ┃Byte 2  ┃Byte 3  ┃Byte 4  ┃
    // ┡━━━━━━━━╇━━━━━━━━╇━━━━━━━━╇━━━━━━━━┩
    // │0xxxxxxx│        │        │        │
    // ├────────┼────────┼────────┼────────┤
    // │110xxxxx│10xxxxxx│        │        │
    // ├────────┼────────┼────────┼────────┤
    // │1110xxxx│10xxxxxx│10xxxxxx│        │
    // ├────────┼────────┼────────┼────────┤
    // │11110xxx│10xxxxxx│10xxxxxx│10xxxxxx│
    // └────────┴────────┴────────┴────────┘

    // 1 byte UTF8
    if (codepoint <= 0x7f) {  // NOLINT
      const uint8_t p1 = codepoint;
      out.push_back(p1);  // NOLINT
      continue;
    }

    // 2 bytes UTF8
    if (codepoint <= 0x7ff) {  // NOLINT
      uint8_t p2 = codepoint & 0x3f;      // NOLINT
      codepoint >>= 6;                        // NOLINT
      uint8_t p1 = codepoint;                 // NOLINT
      out.push_back(0xc0 + p1);         // NOLINT
      out.push_back(0x80 + p2);         // NOLINT
      continue;
    }

    // 3 bytes UTF8
    if (codepoint <= 0xffff) {  // NOLINT
      uint8_t p3 = codepoint & 0x3f;       // NOLINT
      codepoint >>= 6;                         // NOLINT
      uint8_t p2 = codepoint & 0x3f;       // NOLINT
      codepoint >>= 6;                         // NOLINT
      uint8_t p1 = codepoint;                  // NOLINT
      out.push_back(0xe0 + p1);          // NOLINT
      out.push_back(0x80 + p2);          // NOLINT
      out.push_back(0x80 + p3);          // NOLINT
      continue;
    }

    // 4 bytes UTF8
    if (codepoint <= 0x10ffff) {  // NOLINT
      uint8_t p4 = codepoint & 0x3f;              // NOLINT
      codepoint >>= 6;                                // NOLINT
      uint8_t p3 = codepoint & 0x3f;              // NOLINT
      codepoint >>= 6;                                // NOLINT
      uint8_t p2 = codepoint & 0x3f;              // NOLINT
      codepoint >>= 6;                                // NOLINT
      uint8_t p1 = codepoint;                         // NOLINT
      out.push_back(0xf0 + p1);                 // NOLINT
      out.push_back(0x80 + p2);                 // NOLINT
      out.push_back(0x80 + p3);                 // NOLINT
      out.push_back(0x80 + p4);                 // NOLINT
      continue;
    }

    // Something else?
  }
  return out;
}

/// Convert a std::wstring into a UTF8 std::string.
std::wstring to_wstring(const std::string& s) {
  std::wstring out;

  size_t i = 0;
  uint32_t codepoint = 0;
  while (EatCodePoint(s, i, &i, &codepoint)) {
    // On linux wstring are UTF32 encoded:
    if (sizeof(wchar_t) == 4) {
      out.push_back(codepoint);  // NOLINT
      continue;
    }

    // On Windows, wstring are UTF16 encoded:

    // Codepoint encoded using 1 word:
    // NOLINTNEXTLINE
    if (codepoint < 0xD800 || (codepoint > 0xDFFF && codepoint < 0x10000)) {
      uint16_t p0 = codepoint;  // NOLINT
      out.push_back(p0);        // NOLINT
      continue;
    }

    // Codepoint encoded using 2 words:
    codepoint -= 0x010000;                               // NOLINT
    uint16_t p0 = (((codepoint << 12) >> 22) + 0xD800);  // NOLINT
    uint16_t p1 = (((codepoint << 22) >> 22) + 0xDC00);  // NOLINT
    out.push_back(p0);                                   // NOLINT
    out.push_back(p1);                                   // NOLINT
  }
  return out;
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <cstdlib>  // for getenv
#include <string>   // for string, allocator

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#else
#include <sys/ioctl.h>  // for winsize, ioctl, TIOCGWINSZ
#include <unistd.h>     // for STDOUT_FILENO
#endif

namespace ftxui {

namespace {

bool g_cached = false;                     // NOLINT
Terminal::Color g_cached_supported_color;  // NOLINT

Dimensions& FallbackSize() {
#if defined(__EMSCRIPTEN__)
  // This dimension was chosen arbitrarily to be able to display:
  // https://arthursonzogni.com/FTXUI/examples
  // This will have to be improved when someone has time to implement and need
  // it.
  constexpr int fallback_width = 140;
  constexpr int fallback_height = 43;
#else
  // The terminal size in VT100 was 80x24. It is still used nowadays by
  // default in many terminal emulator. That's a good choice for a fallback
  // value.
  constexpr int fallback_width = 80;
  constexpr int fallback_height = 24;
#endif
  static Dimensions g_fallback_size{
      fallback_width,
      fallback_height,
  };
  return g_fallback_size;
}

const char* Safe(const char* c) {
  return (c != nullptr) ? c : "";
}

bool Contains(const std::string& s, const char* key) {
  return s.find(key) != std::string::npos;
}

Terminal::Color ComputeColorSupport() {
#if defined(__EMSCRIPTEN__)
  return Terminal::Color::TrueColor;
#endif

  std::string COLORTERM = Safe(std::getenv("COLORTERM"));  // NOLINT
  if (Contains(COLORTERM, "24bit") || Contains(COLORTERM, "truecolor")) {
    return Terminal::Color::TrueColor;
  }

  std::string TERM = Safe(std::getenv("TERM"));  // NOLINT
  if (Contains(COLORTERM, "256") || Contains(TERM, "256")) {
    return Terminal::Color::Palette256;
  }

#if defined(FTXUI_MICROSOFT_TERMINAL_FALLBACK)
  // Microsoft terminals do not properly declare themselve supporting true
  // colors: https://github.com/microsoft/terminal/issues/1040
  // As a fallback, assume microsoft terminal are the ones not setting those
  // variables, and enable true colors.
  if (TERM == "" && COLORTERM == "") {
    return Terminal::Color::TrueColor;
  }
#endif

  return Terminal::Color::Palette16;
}

}  // namespace

namespace Terminal {
Dimensions Size() {
#if defined(__EMSCRIPTEN__)
  // This dimension was chosen arbitrarily to be able to display:
  // https://arthursonzogni.com/FTXUI/examples
  // This will have to be improved when someone has time to implement and need
  // it.
  return FallbackSize();
#elif defined(_WIN32)
  CONSOLE_SCREEN_BUFFER_INFO csbi;

  if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
    return Dimensions{csbi.srWindow.Right - csbi.srWindow.Left + 1,
                      csbi.srWindow.Bottom - csbi.srWindow.Top + 1};
  }

  return FallbackSize();
#else
  winsize w{};
  const int status = ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);  // NOLINT
  // The ioctl return value result should be checked. Some operating systems
  // don't support TIOCGWINSZ.
  if (w.ws_col == 0 || w.ws_row == 0 || status < 0) {
    return FallbackSize();
  }
  return Dimensions{w.ws_col, w.ws_row};
#endif
}

/// @brief Override terminal size in case auto-detection fails
/// @param fallbackSize Terminal dimensions to fallback to
void SetFallbackSize(const Dimensions& fallbackSize) {
  FallbackSize() = fallbackSize;
}

Color ColorSupport() {
  if (!g_cached) {
    g_cached = true;
    g_cached_supported_color = ComputeColorSupport();
  }
  return g_cached_supported_color;
}

void SetColorSupport(Color color) {
  g_cached = true;
  g_cached_supported_color = color;
}

}  // namespace Terminal
}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <memory>   // for make_shared
#include <utility>  // for move

#ifndef FTXUI_DOM_ELEMENTS_HPP
#define FTXUI_DOM_ELEMENTS_HPP

#include <functional>
#include <memory>

#ifndef FTXUI_DOM_CANVAS_HPP
#define FTXUI_DOM_CANVAS_HPP

#include <cstddef>        // for size_t
#include <functional>     // for function
#include <string>         // for string
#include <unordered_map>  // for unordered_map

#ifdef DrawText
// Workaround for WinUsr.h (via Windows.h) defining macros that break things.
// https://docs.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-drawtext
#undef DrawText
#endif

namespace ftxui {

struct Canvas {
 public:
  Canvas() = default;
  Canvas(int width, int height);

  // Getters:
  int width() const { return width_; }
  int height() const { return height_; }
  Pixel GetPixel(int x, int y) const;

  using Stylizer = std::function<void(Pixel&)>;

  // Draws using braille characters --------------------------------------------
  void DrawPointOn(int x, int y);
  void DrawPointOff(int x, int y);
  void DrawPointToggle(int x, int y);
  void DrawPoint(int x, int y, bool value);
  void DrawPoint(int x, int y, bool value, const Stylizer& s);
  void DrawPoint(int x, int y, bool value, const Color& color);
  void DrawPointLine(int x1, int y1, int x2, int y2);
  void DrawPointLine(int x1, int y1, int x2, int y2, const Stylizer& s);
  void DrawPointLine(int x1, int y1, int x2, int y2, const Color& color);
  void DrawPointCircle(int x, int y, int radius);
  void DrawPointCircle(int x, int y, int radius, const Stylizer& s);
  void DrawPointCircle(int x, int y, int radius, const Color& color);
  void DrawPointCircleFilled(int x, int y, int radius);
  void DrawPointCircleFilled(int x, int y, int radius, const Stylizer& s);
  void DrawPointCircleFilled(int x, int y, int radius, const Color& color);
  void DrawPointEllipse(int x, int y, int r1, int r2);
  void DrawPointEllipse(int x, int y, int r1, int r2, const Color& color);
  void DrawPointEllipse(int x, int y, int r1, int r2, const Stylizer& s);
  void DrawPointEllipseFilled(int x, int y, int r1, int r2);
  void DrawPointEllipseFilled(int x, int y, int r1, int r2, const Color& color);
  void DrawPointEllipseFilled(int x, int y, int r1, int r2, const Stylizer& s);

  // Draw using box characters -------------------------------------------------
  // Block are of size 1x2. y is considered to be a multiple of 2.
  void DrawBlockOn(int x, int y);
  void DrawBlockOff(int x, int y);
  void DrawBlockToggle(int x, int y);
  void DrawBlock(int x, int y, bool value);
  void DrawBlock(int x, int y, bool value, const Stylizer& s);
  void DrawBlock(int x, int y, bool value, const Color& color);
  void DrawBlockLine(int x1, int y1, int x2, int y2);
  void DrawBlockLine(int x1, int y1, int x2, int y2, const Stylizer& s);
  void DrawBlockLine(int x1, int y1, int x2, int y2, const Color& color);
  void DrawBlockCircle(int x1, int y1, int radius);
  void DrawBlockCircle(int x1, int y1, int radius, const Stylizer& s);
  void DrawBlockCircle(int x1, int y1, int radius, const Color& color);
  void DrawBlockCircleFilled(int x1, int y1, int radius);
  void DrawBlockCircleFilled(int x1, int y1, int radius, const Stylizer& s);
  void DrawBlockCircleFilled(int x1, int y1, int radius, const Color& color);
  void DrawBlockEllipse(int x1, int y1, int r1, int r2);
  void DrawBlockEllipse(int x1, int y1, int r1, int r2, const Stylizer& s);
  void DrawBlockEllipse(int x1, int y1, int r1, int r2, const Color& color);
  void DrawBlockEllipseFilled(int x1, int y1, int r1, int r2);
  void DrawBlockEllipseFilled(int x1,
                              int y1,
                              int r1,
                              int r2,
                              const Stylizer& s);
  void DrawBlockEllipseFilled(int x1,
                              int y1,
                              int r1,
                              int r2,
                              const Color& color);

  // Draw using normal characters ----------------------------------------------
  // Draw using character of size 2x4 at position (x,y)
  // x is considered to be a multiple of 2.
  // y is considered to be a multiple of 4.
  void DrawText(int x, int y, const std::string& value);
  void DrawText(int x, int y, const std::string& value, const Color& color);
  void DrawText(int x, int y, const std::string& value, const Stylizer& style);

  // Decorator:
  // x is considered to be a multiple of 2.
  // y is considered to be a multiple of 4.
  void Style(int x, int y, const Stylizer& style);

 private:
  bool IsIn(int x, int y) const {
    return x >= 0 && x < width_ && y >= 0 && y < height_;
  }
  enum CellType {
    kBraille,
    kBlock,
    kText,
  };
  struct Cell {
    CellType type = kText;
    Pixel content;
  };
  struct XY {
    int x;
    int y;
    bool operator==(const XY& other) const {
      return x == other.x && y == other.y;
    }
  };

  struct XYHash {
    size_t operator()(const XY& xy) const {
      constexpr size_t shift = 1024;
      return size_t(xy.x) * shift + size_t(xy.y);
    }
  };

  int width_ = 0;
  int height_ = 0;
  std::unordered_map<XY, Cell, XYHash> storage_;
};

}  // namespace ftxui

#endif  // FTXUI_DOM_CANVAS_HPP

// Copyright 2021 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#ifndef FTXUI_DOM_FLEXBOX_CONFIG_HPP
#define FTXUI_DOM_FLEXBOX_CONFIG_HPP

/*
  This replicate the CSS flexbox model.
  See guide for documentation:
  https://css-tricks.com/snippets/css/a-guide-to-flexbox/
*/

namespace ftxui {

struct FlexboxConfig {
  /// This establishes the main-axis, thus defining the direction flex items are
  /// placed in the flex container. Flexbox is (aside wrapping) single-direction
  /// layout concept. Think of flex items as primarily laying out either in
  /// horizontal rows or vertical columns.
  enum class Direction {
    Row,            ///< Flex items are laid out in a row.
    RowInversed,    ///< Flex items are laid out in a row, but in reverse order.
    Column,         ///< Flex items are laid out in a column.
    ColumnInversed  ///< Flex items are laid out in a column, but in reverse
                    ///< order.
  };
  Direction direction = Direction::Row;

  /// By default, flex items will all try to fit onto one line. You can change
  /// that and allow the items to wrap as needed with this property.
  enum class Wrap {
    NoWrap,        ///< Flex items will all try to fit onto one line.
    Wrap,          ///< Flex items will wrap onto multiple lines.
    WrapInversed,  ///< Flex items will wrap onto multiple lines, but in reverse
                   ///< order.
  };
  Wrap wrap = Wrap::Wrap;

  /// This defines the alignment along the main axis. It helps distribute extra
  /// free space leftover when either all the flex items on a line are
  /// inflexible, or are flexible but have reached their maximum size. It also
  /// exerts some control over the alignment of items when they overflow the
  /// line.
  enum class JustifyContent {
    /// Items are aligned to the start of flexbox's direction.
    FlexStart,
    /// Items are aligned to the end of flexbox's direction.
    FlexEnd,
    /// Items are centered along the line.
    Center,
    /// Items are stretched to fill the line.
    Stretch,
    /// Items are evenly distributed in the line; first item is on the start
    // line, last item on the end line
    SpaceBetween,
    /// Items are evenly distributed in the line with equal space around them.
    /// Note that visually the spaces aren’t equal, since all the items have
    /// equal space on both sides. The first item will have one unit of space
    /// against the container edge, but two units of space between the next item
    /// because that next item has its own spacing that applies.
    SpaceAround,
    /// Items are distributed so that the spacing between any two items (and the
    /// space to the edges) is equal.
    SpaceEvenly,
  };
  JustifyContent justify_content = JustifyContent::FlexStart;

  /// This defines the default behavior for how flex items are laid out along
  /// the cross axis on the current line. Think of it as the justify-content
  /// version for the cross-axis (perpendicular to the main-axis).
  enum class AlignItems {
    FlexStart,  ///< items are placed at the start of the cross axis.
    FlexEnd,    ///< items are placed at the end of the cross axis.
    Center,     ///< items are centered along the cross axis.
    Stretch,    ///< items are stretched to fill the cross axis.
  };
  AlignItems align_items = AlignItems::FlexStart;

  // This aligns a flex container’s lines within when there is extra space in
  // the cross-axis, similar to how justify-content aligns individual items
  // within the main-axis.
  enum class AlignContent {
    FlexStart,     ///< items are placed at the start of the cross axis.
    FlexEnd,       ///< items are placed at the end of the cross axis.
    Center,        ///< items are centered along the cross axis.
    Stretch,       ///< items are stretched to fill the cross axis.
    SpaceBetween,  ///< items are evenly distributed in the cross axis.
    SpaceAround,   ///< tems evenly distributed with equal space around each
                   ///< line.
    SpaceEvenly,  ///< items are evenly distributed in the cross axis with equal
                  ///< space around them.
  };
  AlignContent align_content = AlignContent::FlexStart;

  int gap_x = 0;
  int gap_y = 0;

  // Constructor pattern. For chained use like:
  // ```
  // FlexboxConfig()
  //    .Set(FlexboxConfig::Direction::Row)
  //    .Set(FlexboxConfig::Wrap::Wrap);
  // ```
  FlexboxConfig& Set(FlexboxConfig::Direction);
  FlexboxConfig& Set(FlexboxConfig::Wrap);
  FlexboxConfig& Set(FlexboxConfig::JustifyContent);
  FlexboxConfig& Set(FlexboxConfig::AlignItems);
  FlexboxConfig& Set(FlexboxConfig::AlignContent);
  FlexboxConfig& SetGap(int gap_x, int gap_y);
};

}  // namespace ftxui

#endif  // FTXUI_DOM_FLEXBOX_CONFIG_HPP

// Copyright 2021 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#ifndef FTXUI_DOM_LINEAR_GRADIENT_HPP
#define FTXUI_DOM_LINEAR_GRADIENT_HPP

// #include <optional>  // provided by cxx11_compat.hpp
#include <vector>

namespace ftxui {

/// @brief A class representing the settings for linear-gradient color effect.
///
/// Example:
/// ```cpp
/// LinearGradient()
///    .Angle(45)
///    .Stop(Color::Red, 0.0)
///    .Stop(Color::Green, 0.5)
///    .Stop(Color::Blue, 1.0);
/// ```
///
/// There are also shorthand constructors:
/// ```cpp
/// LinearGradient(Color::Red, Color::Blue);
/// LinearGradient(45, Color::Red, Color::Blue);
/// ```
struct LinearGradient {
  float angle = 0.f;
  struct Stop {
    Color color = Color::Default;
    ftxui::optional<float> position;

    Stop() = default;
    Stop(Color c, ftxui::optional<float> p) : color(c), position(p) {}
  };
  std::vector<Stop> stops;

  // Simple constructor
  LinearGradient();
  LinearGradient(Color begin, Color end);
  LinearGradient(float angle, Color begin, Color end);

  // Modifier using the builder pattern.
  LinearGradient& Angle(float angle);
  LinearGradient& Stop(Color color, float position);
  LinearGradient& Stop(Color color);
};

}  // namespace ftxui

#endif  // FTXUI_DOM_LINEAR_GRADIENT_HPP

// Copyright 2023 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#ifndef FTXUI_DOM_NODE_HPP
#define FTXUI_DOM_NODE_HPP

#include <memory>  // for shared_ptr
#include <vector>  // for vector

#ifndef FTXUI_DOM_REQUIREMENT_HPP
#define FTXUI_DOM_REQUIREMENT_HPP

namespace ftxui {

struct Requirement {
  // The required size to fully draw the element.
  int min_x = 0;
  int min_y = 0;

  // How much flexibility is given to the component.
  int flex_grow_x = 0;
  int flex_grow_y = 0;
  int flex_shrink_x = 0;
  int flex_shrink_y = 0;

  // Focus management to support the frame/focus/select element.
  enum Selection {
    NORMAL = 0,
    SELECTED = 1,
    FOCUSED = 2,
  };
  Selection selection = NORMAL;
  Box selected_box;
};

}  // namespace ftxui

#endif  // FTXUI_DOM_REQUIREMENT_HPP

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

namespace ftxui {

class Node;
class Screen;

using Element = std::shared_ptr<Node>;
using Elements = std::vector<Element>;

class Node {
 public:
  Node();
  Node(Elements children);
  Node(const Node&) = delete;
  Node(const Node&&) = delete;
  Node& operator=(const Node&) = delete;
  Node& operator=(const Node&&) = delete;

  virtual ~Node();

  // Step 1: Compute layout requirement. Tell parent what dimensions this
  //         element wants to be.
  //         Propagated from Children to Parents.
  virtual void ComputeRequirement();
  Requirement requirement() { return requirement_; }

  // Step 2: Assign this element its final dimensions.
  //         Propagated from Parents to Children.
  virtual void SetBox(Box box);

  // Step 3: Draw this element.
  virtual void Render(Screen& screen);

  // Layout may not resolve within a single iteration for some elements. This
  // allows them to request additionnal iterations. This signal must be
  // forwarded to children at least once.
  struct Status {
    int iteration = 0;
    bool need_iteration = false;
  };
  virtual void Check(Status* status);

 protected:
  Elements children_;
  Requirement requirement_;
  Box box_;
};

void Render(Screen& screen, const Element& element);
void Render(Screen& screen, Node* node);

}  // namespace ftxui

#endif  // FTXUI_DOM_NODE_HPP

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#ifndef FTXUI_UTIL_REF_HPP
#define FTXUI_UTIL_REF_HPP

#include <string>

namespace ftxui {

/// @brief An adapter. Own or reference an immutable object.
template <typename T>
class ConstRef {
 public:
  ConstRef() {}
  ConstRef(T t) : owned_(t) {}
  ConstRef(const T* t) : address_(t) {}
  const T& operator*() const { return address_ ? *address_ : owned_; }
  const T& operator()() const { return address_ ? *address_ : owned_; }
  const T* operator->() const { return address_ ? address_ : &owned_; }

 private:
  T owned_;
  const T* address_ = nullptr;
};

/// @brief An adapter. Own or reference an mutable object.
template <typename T>
class Ref {
 public:
  Ref() {}
  Ref(const T& t) : owned_(t) {}
  Ref(T&& t) : owned_(std::forward<T>(t)) {}
  Ref(T* t) : address_(t) {}
  T& operator*() { return address_ ? *address_ : owned_; }
  T& operator()() { return address_ ? *address_ : owned_; }
  T* operator->() { return address_ ? address_ : &owned_; }

 private:
  T owned_;
  T* address_ = nullptr;
};

/// @brief An adapter. Own or reference a constant string. For convenience, this
/// class convert multiple mutable string toward a shared representation.
class StringRef {
 public:
  StringRef(std::string* ref) : address_(ref) {}
  StringRef(std::string ref) : owned_(std::move(ref)) {}
  StringRef(const wchar_t* ref) : StringRef(to_string(std::wstring(ref))) {}
  StringRef(const char* ref) : StringRef(std::string(ref)) {}
  std::string& operator*() { return address_ ? *address_ : owned_; }
  std::string* operator->() { return address_ ? address_ : &owned_; }

 private:
  std::string owned_;
  std::string* address_ = nullptr;
};

/// @brief An adapter. Own or reference a constant string. For convenience, this
/// class convert multiple immutable string toward a shared representation.
class ConstStringRef {
 public:
  ConstStringRef(const std::string* ref) : address_(ref) {}
  ConstStringRef(const std::wstring* ref) : ConstStringRef(to_string(*ref)) {}
  ConstStringRef(std::string ref) : owned_(std::move(ref)) {}
  ConstStringRef(std::wstring ref) : ConstStringRef(to_string(ref)) {}
  ConstStringRef(const wchar_t* ref) : ConstStringRef(std::wstring(ref)) {}
  ConstStringRef(const char* ref)
      : ConstStringRef(to_wstring(std::string(ref))) {}
  const std::string& operator()() const {
    return address_ ? *address_ : owned_;
  }
  const std::string& operator*() const { return address_ ? *address_ : owned_; }
  const std::string* operator->() const {
    return address_ ? address_ : &owned_;
  }

 private:
  const std::string owned_;
  const std::string* address_ = nullptr;
};

/// @brief An adapter. Reference a list of strings.
class ConstStringListRef {
 public:
  ConstStringListRef(const std::vector<std::string>* ref) : ref_(ref) {}
  ConstStringListRef(const std::vector<std::wstring>* ref) : ref_wide_(ref) {}

  size_t size() const { return ref_ ? ref_->size() : ref_wide_->size(); }
  std::string operator[](size_t i) const {
    return ref_ ? (*ref_)[i] : to_string((*ref_wide_)[i]);
  }

 private:
  const std::vector<std::string>* ref_ = nullptr;
  const std::vector<std::wstring>* ref_wide_ = nullptr;
};

}  // namespace ftxui

#endif /* end of include guard: FTXUI_UTIL_REF_HPP */

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

namespace ftxui {
class Node;
using Element = std::shared_ptr<Node>;
using Elements = std::vector<Element>;
using Decorator = std::function<Element(Element)>;
using GraphFunction = std::function<std::vector<int>(int, int)>;

enum BorderStyle {
  LIGHT,
  DASHED,
  HEAVY,
  DOUBLE,
  ROUNDED,
  EMPTY,
};

enum class GaugeDirection { Left, Up, Right, Down };

// Pipe elements into decorator togethers.
// For instance the next lines are equivalents:
// -> text("ftxui") | bold | underlined
// -> underlined(bold(text("FTXUI")))
Element operator|(Element, Decorator);
Element& operator|=(Element&, Decorator);
Elements operator|(Elements, Decorator);
Decorator operator|(Decorator, Decorator);

// --- Widget ---
Element text(std::string text);
Element vtext(std::string text);
Element separator();
Element separatorLight();
Element separatorDashed();
Element separatorHeavy();
Element separatorDouble();
Element separatorEmpty();
Element separatorStyled(BorderStyle);
Element separator(Pixel);
Element separatorCharacter(std::string);
Element separatorHSelector(float left,
                           float right,
                           Color unselected_color,
                           Color selected_color);
Element separatorVSelector(float up,
                           float down,
                           Color unselected_color,
                           Color selected_color);
Element gauge(float progress);
Element gaugeLeft(float progress);
Element gaugeRight(float progress);
Element gaugeUp(float progress);
Element gaugeDown(float progress);
Element gaugeDirection(float progress, GaugeDirection);
Element border(Element);
Element borderLight(Element);
Element borderDashed(Element);
Element borderHeavy(Element);
Element borderDouble(Element);
Element borderRounded(Element);
Element borderEmpty(Element);
Decorator borderStyled(BorderStyle);
Decorator borderStyled(BorderStyle, Color);
Decorator borderStyled(Color);
Decorator borderWith(const Pixel&);
Element window(Element title, Element content);
Element spinner(int charset_index, size_t image_index);
Element paragraph(const std::string& text);
Element paragraphAlignLeft(const std::string& text);
Element paragraphAlignRight(const std::string& text);
Element paragraphAlignCenter(const std::string& text);
Element paragraphAlignJustify(const std::string& text);
Element graph(GraphFunction);
Element emptyElement();
Element canvas(ConstRef<Canvas>);
Element canvas(int width, int height, std::function<void(Canvas&)>);
Element canvas(std::function<void(Canvas&)>);

// -- Decorator ---
Element bold(Element);
Element dim(Element);
Element inverted(Element);
Element underlined(Element);
Element underlinedDouble(Element);
Element blink(Element);
Element strikethrough(Element);
Decorator color(Color);
Decorator bgcolor(Color);
Decorator color(const LinearGradient&);
Decorator bgcolor(const LinearGradient&);
Element color(Color, Element);
Element bgcolor(Color, Element);
Element color(const LinearGradient&, Element);
Element bgcolor(const LinearGradient&, Element);
Decorator focusPosition(int x, int y);
Decorator focusPositionRelative(float x, float y);
Element automerge(Element child);

// --- Layout is
// Horizontal, Vertical or stacked set of elements.
Element hbox(Elements);
Element vbox(Elements);
Element dbox(Elements);
Element flexbox(Elements, FlexboxConfig config = FlexboxConfig());
Element gridbox(std::vector<Elements> lines);

Element hflow(Elements);  // Helper: default flexbox with row direction.
Element vflow(Elements);  // Helper: default flexbox with column direction.

// -- Flexibility ---
// Define how to share the remaining space when not all of it is used inside a
// container.
Element flex(Element);         // Expand/Minimize if possible/needed.
Element flex_grow(Element);    // Expand element if possible.
Element flex_shrink(Element);  // Minimize element if needed.

Element xflex(Element);         // Expand/Minimize if possible/needed on X axis.
Element xflex_grow(Element);    // Expand element if possible on X axis.
Element xflex_shrink(Element);  // Minimize element if needed on X axis.

Element yflex(Element);         // Expand/Minimize if possible/needed on Y axis.
Element yflex_grow(Element);    // Expand element if possible on Y axis.
Element yflex_shrink(Element);  // Minimize element if needed on Y axis.

Element notflex(Element);  // Reset the flex attribute.
Element filler();          // A blank expandable element.

// -- Size override;
enum Direction { WIDTH, HEIGHT };
enum Constraint { LESS_THAN, EQUAL, GREATER_THAN };
Decorator size(Direction, Constraint, int value);

// --- Frame ---
// A frame is a scrollable area. The internal area is potentially larger than
// the external one. The internal area is scrolled in order to make visible the
// focused element.
Element frame(Element);
Element xframe(Element);
Element yframe(Element);
Element focus(Element);
Element select(Element);

// --- Cursor ---
// Those are similar to `focus`, but also change the shape of the cursor.
Element focusCursorBlock(Element);
Element focusCursorBlockBlinking(Element);
Element focusCursorBar(Element);
Element focusCursorBarBlinking(Element);
Element focusCursorUnderline(Element);
Element focusCursorUnderlineBlinking(Element);

// --- Misc ---
Element vscroll_indicator(Element);
Decorator reflect(Box& box);
// Before drawing the |element| clear the pixel below. This is useful in
// combinaison with dbox.
Element clear_under(Element element);

// --- Util --------------------------------------------------------------------
Element hcenter(Element);
Element vcenter(Element);
Element center(Element);
Element align_right(Element);
Element nothing(Element element);

namespace Dimension {
Dimensions Fit(Element&);
}  // namespace Dimension

}  // namespace ftxui

// Make container able to take any number of children as input.
#ifndef FTXUI_DOM_TAKE_ANY_ARGS_HPP
#define FTXUI_DOM_TAKE_ANY_ARGS_HPP

// IWYU pragma: private, include "ftxui/dom/elements.hpp"
#include <type_traits>

namespace ftxui {

template <class T>
void Merge(Elements& /*container*/, T /*element*/) {}

template <>
inline void Merge(Elements& container, Element element) {
  container.push_back(std::move(element));
}

template <>
inline void Merge(Elements& container, Elements elements) {
  for (auto& element : elements)
    container.push_back(std::move(element));
}

// Turn a set of arguments into a vector.
template <class... Args>
Elements unpack(Args... args) {
  std::vector<Element> vec;
  (Merge(vec, std::move(args)), ...);
  return vec;
}

// Make |container| able to take any number of argments.
#define TAKE_ANY_ARGS(container)                               \
  template <class... Args>                                     \
  Element container(Args... children) {                        \
    return container(unpack(std::forward<Args>(children)...)); \
  }

TAKE_ANY_ARGS(vbox)
TAKE_ANY_ARGS(hbox)
TAKE_ANY_ARGS(dbox)
TAKE_ANY_ARGS(hflow)
}  // namespace ftxui

#endif  // FTXUI_DOM_TAKE_ANY_ARGS_HPP

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

// Include old definitions using wstring.
#ifndef FTXUI_DOM_DEPRECATED_HPP
#define FTXUI_DOM_DEPRECATED_HPP

namespace ftxui {
Element text(std::wstring text);
Element vtext(std::wstring text);
Elements paragraph(std::wstring text);
}  // namespace ftxui

#endif  // FTXUI_DOM_DEPRECATED_HPP

// Copyright 2021 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#endif  // FTXUI_DOM_ELEMENTS_HPP

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#ifndef FTXUI_DOM_NODE_DECORATOR_H_
#define FTXUI_DOM_NODE_DECORATOR_H_

#include <utility>  // for move

namespace ftxui {
struct Box;

// Helper class.
class NodeDecorator : public Node {
 public:
  NodeDecorator(Element child) : Node(unpack(std::move(child))) {}
  void ComputeRequirement() override;
  void SetBox(Box box) override;
};

}  // namespace ftxui

#endif /* end of include guard: FTXUI_DOM_NODE_DECORATOR_H_ */

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

namespace ftxui {

/// @brief Enable character to be automatically merged with others nearby.
/// @ingroup dom
Element automerge(Element child) {
  class Impl : public NodeDecorator {
   public:
    using NodeDecorator::NodeDecorator;

    void Render(Screen& screen) override {
      for (int y = box_.y_min; y <= box_.y_max; ++y) {
        for (int x = box_.x_min; x <= box_.x_max; ++x) {
          screen.PixelAt(x, y).automerge = true;
        }
      }
      Node::Render(screen);
    }
  };

  return std::make_shared<Impl>(std::move(child));
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <memory>   // for make_shared
#include <utility>  // for move

namespace ftxui {

class Blink : public NodeDecorator {
 public:
  using NodeDecorator::NodeDecorator;

  void Render(Screen& screen) override {
    Node::Render(screen);
    for (int y = box_.y_min; y <= box_.y_max; ++y) {
      for (int x = box_.x_min; x <= box_.x_max; ++x) {
        screen.PixelAt(x, y).blink = true;
      }
    }
  }
};

/// @brief The text drawn alternates in between visible and hidden.
/// @ingroup dom
Element blink(Element child) {
  return std::make_shared<Blink>(std::move(child));
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <memory>   // for make_shared
#include <utility>  // for move

namespace ftxui {

class Bold : public NodeDecorator {
 public:
  using NodeDecorator::NodeDecorator;

  void Render(Screen& screen) override {
    for (int y = box_.y_min; y <= box_.y_max; ++y) {
      for (int x = box_.x_min; x <= box_.x_max; ++x) {
        screen.PixelAt(x, y).bold = true;
      }
    }
    Node::Render(screen);
  }
};

/// @brief Use a bold font, for elements with more emphasis.
/// @ingroup dom
Element bold(Element child) {
  return std::make_shared<Bold>(std::move(child));
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <algorithm>               // for max
#include <array>                   // for array
#include <memory>    // for allocator, make_shared, __shared_ptr_access
// #include <optional>  // provided by cxx11_compat.hpp
#include <string>    // for basic_string, string
#include <utility>   // for move
#include <vector>    // for __alloc_traits<>::value_type

namespace ftxui {

using Charset = std::array<std::string, 6>;  // NOLINT
using Charsets = std::array<Charset, 6>;     // NOLINT
// NOLINTNEXTLINE
static Charsets simple_border_charset = {
    Charset{"┌", "┐", "└", "┘", "─", "│"},  // LIGHT
    Charset{"┏", "┓", "┗", "┛", "╍", "╏"},  // DASHED
    Charset{"┏", "┓", "┗", "┛", "━", "┃"},  // HEAVY
    Charset{"╔", "╗", "╚", "╝", "═", "║"},  // DOUBLE
    Charset{"╭", "╮", "╰", "╯", "─", "│"},  // ROUNDED
    Charset{" ", " ", " ", " ", " ", " "},  // EMPTY
};

// For reference, here is the charset for normal border:
class Border : public Node {
 public:
  Border(Elements children,
         BorderStyle style,
         ftxui::optional<Color> foreground_color = ftxui::nullopt)
      : Node(std::move(children)),
        charset_(simple_border_charset[style]),
        foreground_color_(foreground_color) {}  // NOLINT

  const Charset& charset_;  // NOLINT
  ftxui::optional<Color> foreground_color_;

  void ComputeRequirement() override {
    Node::ComputeRequirement();
    requirement_ = children_[0]->requirement();
    requirement_.min_x += 2;
    requirement_.min_y += 2;
    if (children_.size() == 2) {
      requirement_.min_x =
          std::max(requirement_.min_x, children_[1]->requirement().min_x + 2);
    }
    requirement_.selected_box.x_min++;
    requirement_.selected_box.x_max++;
    requirement_.selected_box.y_min++;
    requirement_.selected_box.y_max++;
  }

  void SetBox(Box box) override {
    Node::SetBox(box);
    if (children_.size() == 2) {
      Box title_box;
      title_box.x_min = box.x_min + 1;
      title_box.x_max = box.x_max - 1;
      title_box.y_min = box.y_min;
      title_box.y_max = box.y_min;
      children_[1]->SetBox(title_box);
    }
    box.x_min++;
    box.x_max--;
    box.y_min++;
    box.y_max--;
    children_[0]->SetBox(box);
  }

  void Render(Screen& screen) override {
    // Draw content.
    children_[0]->Render(screen);

    // Draw the border.
    if (box_.x_min >= box_.x_max || box_.y_min >= box_.y_max) {
      return;
    }

    screen.at(box_.x_min, box_.y_min) = charset_[0];  // NOLINT
    screen.at(box_.x_max, box_.y_min) = charset_[1];  // NOLINT
    screen.at(box_.x_min, box_.y_max) = charset_[2];  // NOLINT
    screen.at(box_.x_max, box_.y_max) = charset_[3];  // NOLINT

    for (int x = box_.x_min + 1; x < box_.x_max; ++x) {
      Pixel& p1 = screen.PixelAt(x, box_.y_min);
      Pixel& p2 = screen.PixelAt(x, box_.y_max);
      p1.character = charset_[4];  // NOLINT
      p2.character = charset_[4];  // NOLINT
      p1.automerge = true;
      p2.automerge = true;
    }
    for (int y = box_.y_min + 1; y < box_.y_max; ++y) {
      Pixel& p3 = screen.PixelAt(box_.x_min, y);
      Pixel& p4 = screen.PixelAt(box_.x_max, y);
      p3.character = charset_[5];  // NOLINT
      p4.character = charset_[5];  // NOLINT
      p3.automerge = true;
      p4.automerge = true;
    }

    // Draw title.
    if (children_.size() == 2) {
      children_[1]->Render(screen);
    }

    // Draw the border color.
    if (foreground_color_) {
      for (int x = box_.x_min; x <= box_.x_max; ++x) {
        screen.PixelAt(x, box_.y_min).foreground_color = *foreground_color_;
        screen.PixelAt(x, box_.y_max).foreground_color = *foreground_color_;
      }
      for (int y = box_.y_min; y <= box_.y_max; ++y) {
        screen.PixelAt(box_.x_min, y).foreground_color = *foreground_color_;
        screen.PixelAt(box_.x_max, y).foreground_color = *foreground_color_;
      }
    }
  }
};

// For reference, here is the charset for normal border:
class BorderPixel : public Node {
 public:
  BorderPixel(Elements children, Pixel pixel)
      : Node(std::move(children)), pixel_(std::move(pixel)) {}

 private:
  Pixel pixel_;

  void ComputeRequirement() override {
    Node::ComputeRequirement();
    requirement_ = children_[0]->requirement();
    requirement_.min_x += 2;
    requirement_.min_y += 2;
    if (children_.size() == 2) {
      requirement_.min_x =
          std::max(requirement_.min_x, children_[1]->requirement().min_x + 2);
    }
    requirement_.selected_box.x_min++;
    requirement_.selected_box.x_max++;
    requirement_.selected_box.y_min++;
    requirement_.selected_box.y_max++;
  }

  void SetBox(Box box) override {
    Node::SetBox(box);
    if (children_.size() == 2) {
      Box title_box;
      title_box.x_min = box.x_min + 1;
      title_box.x_max = box.x_max - 1;
      title_box.y_min = box.y_min;
      title_box.y_max = box.y_min;
      children_[1]->SetBox(title_box);
    }
    box.x_min++;
    box.x_max--;
    box.y_min++;
    box.y_max--;
    children_[0]->SetBox(box);
  }

  void Render(Screen& screen) override {
    // Draw content.
    children_[0]->Render(screen);

    // Draw the border.
    if (box_.x_min >= box_.x_max || box_.y_min >= box_.y_max) {
      return;
    }

    screen.PixelAt(box_.x_min, box_.y_min) = pixel_;
    screen.PixelAt(box_.x_max, box_.y_min) = pixel_;
    screen.PixelAt(box_.x_min, box_.y_max) = pixel_;
    screen.PixelAt(box_.x_max, box_.y_max) = pixel_;

    for (int x = box_.x_min + 1; x < box_.x_max; ++x) {
      screen.PixelAt(x, box_.y_min) = pixel_;
      screen.PixelAt(x, box_.y_max) = pixel_;
    }
    for (int y = box_.y_min + 1; y < box_.y_max; ++y) {
      screen.PixelAt(box_.x_min, y) = pixel_;
      screen.PixelAt(box_.x_max, y) = pixel_;
    }
  }
};

/// @brief Draw a border around the element.
/// @ingroup dom
/// @see border
/// @see borderLight
/// @see borderDashed
/// @see borderDouble
/// @see borderHeavy
/// @see borderEmpty
/// @see borderRounded
/// @see borderStyled
/// @see borderWith
///
/// Add a border around an element
///
/// ### Example
///
/// ```cpp
/// // Use 'border' as a function...
/// Element document = border(text("The element"));
///
/// // ...Or as a 'pipe'.
/// Element document = text("The element") | border;
/// ```
///
/// ### Output
///
/// ```bash
/// ┌───────────┐
/// │The element│
/// └───────────┘
/// ```
Element border(Element child) {
  return std::make_shared<Border>(unpack(std::move(child)), ROUNDED);
}

/// @brief Same as border but with a constant Pixel around the element.
/// @ingroup dom
/// @see border
Decorator borderWith(const Pixel& pixel) {
  return [pixel](Element child) {
    return std::make_shared<BorderPixel>(unpack(std::move(child)), pixel);
  };
}

/// @brief Same as border but with different styles.
/// @ingroup dom
/// @see border
Decorator borderStyled(BorderStyle style) {
  return [style](Element child) {
    return std::make_shared<Border>(unpack(std::move(child)), style);
  };
}

/// @brief Same as border but with a foreground color.
/// @ingroup dom
/// @see border
Decorator borderStyled(Color foreground_color) {
  return [foreground_color](Element child) {
    return std::make_shared<Border>(unpack(std::move(child)), ROUNDED,
                                    foreground_color);
  };
}

/// @brief Same as border but with a foreground color and a different style
/// @ingroup dom
/// @see border
Decorator borderStyled(BorderStyle style, Color foreground_color) {
  return [style, foreground_color](Element child) {
    return std::make_shared<Border>(unpack(std::move(child)), style,
                                    foreground_color);
  };
}

/// @brief Draw a light border around the element.
/// @ingroup dom
/// @see border
/// @see borderLight
/// @see borderDashed
/// @see borderDouble
/// @see borderHeavy
/// @see borderRounded
/// @see borderEmpty
/// @see borderStyled
/// @see borderWith
///
/// Add a border around an element
///
/// ### Example
///
/// ```cpp
/// // Use 'borderDash' as a function...
/// Element document = borderDash(text("The element"));
///
/// // ...Or as a 'pipe'.
/// Element document = text("The element") | borderDAsh;
/// ```
///
/// ### Output
///
/// ```bash
/// ┏╍╍╍╍╍╍╍╍╍╍╍╍╍╍┓
/// ╏The element   ╏
/// ┗╍╍╍╍╍╍╍╍╍╍╍╍╍╍┛
/// ```
Element borderDashed(Element child) {
  return std::make_shared<Border>(unpack(std::move(child)), DASHED);
}

/// @brief Draw a dashed border around the element.
/// @ingroup dom
/// @see border
/// @see borderLight
/// @see borderDashed
/// @see borderDouble
/// @see borderHeavy
/// @see borderRounded
/// @see borderEmpty
/// @see borderStyled
/// @see borderWith
///
/// Add a border around an element
///
/// ### Example
///
/// ```cpp
/// // Use 'borderLight' as a function...
/// Element document = borderLight(text("The element"));
///
/// // ...Or as a 'pipe'.
/// Element document = text("The element") | borderLight;
/// ```
///
/// ### Output
///
/// ```bash
/// ┌──────────────┐
/// │The element   │
/// └──────────────┘
/// ```
Element borderLight(Element child) {
  return std::make_shared<Border>(unpack(std::move(child)), LIGHT);
}

/// @brief Draw a heavy border around the element.
/// @ingroup dom
/// @see border
/// @see borderLight
/// @see borderDashed
/// @see borderDouble
/// @see borderHeavy
/// @see borderRounded
/// @see borderEmpty
/// @see borderStyled
/// @see borderWith
///
/// Add a border around an element
///
/// ### Example
///
/// ```cpp
/// // Use 'borderHeavy' as a function...
/// Element document = borderHeavy(text("The element"));
///
/// // ...Or as a 'pipe'.
/// Element document = text("The element") | borderHeavy;
/// ```
///
/// ### Output
///
/// ```bash
/// ┏━━━━━━━━━━━━━━┓
/// ┃The element   ┃
/// ┗━━━━━━━━━━━━━━┛
/// ```
Element borderHeavy(Element child) {
  return std::make_shared<Border>(unpack(std::move(child)), HEAVY);
}

/// @brief Draw a double border around the element.
/// @ingroup dom
/// @see border
/// @see borderLight
/// @see borderDashed
/// @see borderDouble
/// @see borderHeavy
/// @see borderRounded
/// @see borderEmpty
/// @see borderStyled
/// @see borderWith
///
/// Add a border around an element
///
/// ### Example
///
/// ```cpp
/// // Use 'borderDouble' as a function...
/// Element document = borderDouble(text("The element"));
///
/// // ...Or as a 'pipe'.
/// Element document = text("The element") | borderDouble;
/// ```
///
/// ### Output
///
/// ```bash
/// ╔══════════════╗
/// ║The element   ║
/// ╚══════════════╝
/// ```
Element borderDouble(Element child) {
  return std::make_shared<Border>(unpack(std::move(child)), DOUBLE);
}

/// @brief Draw a rounded border around the element.
/// @ingroup dom
/// @see border
/// @see borderLight
/// @see borderDashed
/// @see borderDouble
/// @see borderHeavy
/// @see borderRounded
/// @see borderEmpty
/// @see borderStyled
/// @see borderWith
///
/// Add a border around an element
///
/// ### Example
///
/// ```cpp
/// // Use 'borderRounded' as a function...
/// Element document = borderRounded(text("The element"));
///
/// // ...Or as a 'pipe'.
/// Element document = text("The element") | borderRounded;
/// ```
///
/// ### Output
///
/// ```bash
/// ╭──────────────╮
/// │The element   │
/// ╰──────────────╯
/// ```
Element borderRounded(Element child) {
  return std::make_shared<Border>(unpack(std::move(child)), ROUNDED);
}

/// @brief Draw an empty border around the element.
/// @ingroup dom
/// @see border
/// @see borderLight
/// @see borderDashed
/// @see borderDouble
/// @see borderHeavy
/// @see borderRounded
/// @see borderEmpty
/// @see borderStyled
/// @see borderWith
///
/// Add a border around an element
///
/// ### Example
///
/// ```cpp
/// // Use 'borderRounded' as a function...
/// Element document = borderRounded(text("The element"));
///
/// // ...Or as a 'pipe'.
/// Element document = text("The element") | borderRounded;
/// ```
///
/// ### Output
///
/// ```bash
///
///  The element
///
/// ```
Element borderEmpty(Element child) {
  return std::make_shared<Border>(unpack(std::move(child)), EMPTY);
}

/// @brief Draw window with a title and a border around the element.
/// @param title The title of the window.
/// @param content The element to be wrapped.
/// @ingroup dom
/// @see border
///
/// ### Example
///
/// ```cpp
/// Element document = window(text("Title"),
///                           text("content")
///                    );
/// ```
///
/// ### Output
///
/// ```bash
/// ┌Title──┐
/// │content│
/// └───────┘
/// ```
Element window(Element title, Element content) {
  return std::make_shared<Border>(unpack(std::move(content), std::move(title)),
                                  ROUNDED);
}
}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#ifndef FTXUI_DOM_BOX_HELPER_HPP
#define FTXUI_DOM_BOX_HELPER_HPP

#include <vector>

namespace ftxui {
namespace box_helper {

struct Element {
  // Input:
  int min_size = 0;
  int flex_grow = 0;
  int flex_shrink = 0;

  // Output;
  int size = 0;
};

void Compute(std::vector<Element>* elements, int target_size);

}  // namespace box_helper
}  // namespace ftxui

#endif /* end of include guard: FTXUI_DOM_BOX_HELPER_HPP */

// Copyright 2021 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.line.

#include <algorithm>  // for max

namespace ftxui::box_helper {

namespace {
// Called when the size allowed is greater than the requested size. This
// distributes the extra spaces toward the flexible elements, in relative
// proportions.
void ComputeGrow(std::vector<Element>* elements,
                 int extra_space,
                 int flex_grow_sum) {
  for (Element& element : *elements) {
    const int added_space =
        extra_space * element.flex_grow / std::max(flex_grow_sum, 1);
    extra_space -= added_space;
    flex_grow_sum -= element.flex_grow;
    element.size = element.min_size + added_space;
  }
}

// Called when the size allowed is lower than the requested size, and the
// shrinkable element can absorbe the (negative) extra_space. This distribute
// the extra_space toward those.
void ComputeShrinkEasy(std::vector<Element>* elements,
                       int extra_space,
                       int flex_shrink_sum) {
  for (Element& element : *elements) {
    const int added_space = extra_space * element.min_size *
                            element.flex_shrink / std::max(flex_shrink_sum, 1);
    extra_space -= added_space;
    flex_shrink_sum -= element.flex_shrink * element.min_size;
    element.size = element.min_size + added_space;
  }
}

// Called when the size allowed is lower than the requested size, and the
// shrinkable element can not absorbe the (negative) extra_space. This assign
// zero to shrinkable elements and distribute the remaining (negative)
// extra_space toward the other non shrinkable elements.
void ComputeShrinkHard(std::vector<Element>* elements,
                       int extra_space,
                       int size) {
  for (Element& element : *elements) {
    if (element.flex_shrink != 0) {
      element.size = 0;
      continue;
    }

    const int added_space = extra_space * element.min_size / std::max(1, size);
    extra_space -= added_space;
    size -= element.min_size;

    element.size = element.min_size + added_space;
  }
}

}  // namespace

void Compute(std::vector<Element>* elements, int target_size) {
  int size = 0;
  int flex_grow_sum = 0;
  int flex_shrink_sum = 0;
  int flex_shrink_size = 0;

  for (auto& element : *elements) {
    flex_grow_sum += element.flex_grow;
    flex_shrink_sum += element.min_size * element.flex_shrink;
    if (element.flex_shrink != 0) {
      flex_shrink_size += element.min_size;
    }
    size += element.min_size;
  }

  const int extra_space = target_size - size;
  if (extra_space >= 0) {
    ComputeGrow(elements, extra_space, flex_grow_sum);
  } else if (flex_shrink_size + extra_space >= 0) {
    ComputeShrinkEasy(elements, extra_space, flex_shrink_sum);

  } else {
    ComputeShrinkHard(elements, extra_space + flex_shrink_size,
                      size - flex_shrink_size);
  }
}

}  // namespace ftxui::box_helper

// Copyright 2021 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

#include <algorithm>               // for max, min
#include <cmath>                   // for abs
#include <cstdint>                 // for uint8_t
#include <cstdlib>                 // for abs
#include <map>                     // for map
#include <memory>                  // for make_shared
#include <utility>                 // for move, pair
#include <vector>                  // for vector

namespace ftxui {

namespace {

// Base UTF8 pattern:
// 11100010 10100000 10000000 // empty

// Pattern for the individuel dots:
// ┌──────┬───────┐
// │dot1  │ dot4  │
// ├──────┼───────┤
// │dot2  │ dot5  │
// ├──────┼───────┤
// │dot3  │ dot6  │
// ├──────┼───────┤
// │dot0-1│ dot0-2│
// └──────┴───────┘
// 11100010 10100000 10000001 // dot1
// 11100010 10100000 10000010 // dot2
// 11100010 10100000 10000100 // dot3
// 11100010 10100001 10000000 // dot0-1
// 11100010 10100000 10001000 // dot4
// 11100010 10100000 10010000 // dot5
// 11100010 10100000 10100000 // dot6
// 11100010 10100010 10000000 // dot0-2

// NOLINTNEXTLINE
uint8_t g_map_braille[2][4][2] = {
    {
        {0x0, 0x1},  // NOLINT | dot1
        {0x0, 0x2},  // NOLINT | dot2
        {0x0, 0x4},  // NOLINT | dot3
        {0x1, 0x0},  // NOLINT | dot0-1
    },
    {
        {0x0, 0x8},  // NOLINT | dot4
        {0x0, 0x10},  // NOLINT | dot5
        {0x0, 0x20},  // NOLINT | dot6
        {0x2, 0x0},  // NOLINT | dot0-2
    },
};

// NOLINTNEXTLINE
std::vector<std::string> g_map_block = {
    " ", "▘", "▖", "▌", "▝", "▀", "▞", "▛",
    "▗", "▚", "▄", "▙", "▐", "▜", "▟", "█",
};

// NOLINTNEXTLINE
const std::map<std::string, uint8_t> g_map_block_inversed = {
    {" ", 0x0}, {"▘", 0x1}, {"▖", 0x2}, {"▌", 0x3},
    {"▝", 0x4}, {"▀", 0x5}, {"▞", 0x6}, {"▛", 0x7},
    {"▗", 0x8}, {"▚", 0x9}, {"▄", 0xa}, {"▙", 0xb},
    {"▐", 0xc}, {"▜", 0xd}, {"▟", 0xe}, {"█", 0xf},
};

auto nostyle = [](Pixel& /*pixel*/) {};

}  // namespace

/// @brief Constructor.
/// @param width the width of the canvas. A cell is a 2x8 braille dot.
/// @param height the height of the canvas. A cell is a 2x8 braille dot.
Canvas::Canvas(int width, int height)
    : width_(width),
      height_(height),
      storage_(width_ * height_ / 8 /* NOLINT */) {}

/// @brief Get the content of a cell.
/// @param x the x coordinate of the cell.
/// @param y the y coordinate of the cell.
Pixel Canvas::GetPixel(int x, int y) const {
  auto it = storage_.find(XY{x, y});
  return (it == storage_.end()) ? Pixel{} : it->second.content;
}

/// @brief Draw a braille dot.
/// @param x the x coordinate of the dot.
/// @param y the y coordinate of the dot.
/// @param value whether the dot is filled or not.
void Canvas::DrawPoint(int x, int y, bool value) {
  DrawPoint(x, y, value, [](Pixel& /*pixel*/) {});
}

/// @brief Draw a braille dot.
/// @param x the x coordinate of the dot.
/// @param y the y coordinate of the dot.
/// @param value whether the dot is filled or not.
/// @param color the color of the dot.
void Canvas::DrawPoint(int x, int y, bool value, const Color& color) {
  DrawPoint(x, y, value, [color](Pixel& p) { p.foreground_color = color; });
}

/// @brief Draw a braille dot.
/// @param x the x coordinate of the dot.
/// @param y the y coordinate of the dot.
/// @param value whether the dot is filled or not.
/// @param style the style of the cell.
void Canvas::DrawPoint(int x, int y, bool value, const Stylizer& style) {
  Style(x, y, style);
  if (value) {
    DrawPointOn(x, y);
  } else {
    DrawPointOff(x, y);
  }
}

/// @brief Draw a braille dot.
/// @param x the x coordinate of the dot.
/// @param y the y coordinate of the dot.
void Canvas::DrawPointOn(int x, int y) {
  if (!IsIn(x, y)) {
    return;
  }
  Cell& cell = storage_[XY{x / 2, y / 4}];
  if (cell.type != CellType::kBraille) {
    cell.content.character = "⠀";  // 3 bytes.
    cell.type = CellType::kBraille;
  }

  cell.content.character[1] |= g_map_braille[x % 2][y % 4][0];  // NOLINT
  cell.content.character[2] |= g_map_braille[x % 2][y % 4][1];  // NOLINT
}

/// @brief Erase a braille dot.
/// @param x the x coordinate of the dot.
/// @param y the y coordinate of the dot.
void Canvas::DrawPointOff(int x, int y) {
  if (!IsIn(x, y)) {
    return;
  }
  Cell& cell = storage_[XY{x / 2, y / 4}];
  if (cell.type != CellType::kBraille) {
    cell.content.character = "⠀";  // 3 byt
    cell.type = CellType::kBraille;
  }

  cell.content.character[1] &= ~(g_map_braille[x % 2][y % 4][0]);  // NOLINT
  cell.content.character[2] &= ~(g_map_braille[x % 2][y % 4][1]);  // NOLINT
}

/// @brief Toggle a braille dot. A filled one will be erased, and the other will
/// be drawn.
/// @param x the x coordinate of the dot.
/// @param y the y coordinate of the dot.
void Canvas::DrawPointToggle(int x, int y) {
  if (!IsIn(x, y)) {
    return;
  }
  Cell& cell = storage_[XY{x / 2, y / 4}];
  if (cell.type != CellType::kBraille) {
    cell.content.character = "⠀";  // 3 byt
    cell.type = CellType::kBraille;
  }

  cell.content.character[1] ^= g_map_braille[x % 2][y % 4][0];  // NOLINT
  cell.content.character[2] ^= g_map_braille[x % 2][y % 4][1];  // NOLINT
}

/// @brief Draw a line made of braille dots.
/// @param x1 the x coordinate of the first dot.
/// @param y1 the y coordinate of the first dot.
/// @param x2 the x coordinate of the second dot.
/// @param y2 the y coordinate of the second dot.
void Canvas::DrawPointLine(int x1, int y1, int x2, int y2) {
  DrawPointLine(x1, y1, x2, y2, [](Pixel& /*pixel*/) {});
}

/// @brief Draw a line made of braille dots.
/// @param x1 the x coordinate of the first dot.
/// @param y1 the y coordinate of the first dot.
/// @param x2 the x coordinate of the second dot.
/// @param y2 the y coordinate of the second dot.
/// @param color the color of the line.
void Canvas::DrawPointLine(int x1, int y1, int x2, int y2, const Color& color) {
  DrawPointLine(x1, y1, x2, y2,
                [color](Pixel& p) { p.foreground_color = color; });
}

/// @brief Draw a line made of braille dots.
/// @param x1 the x coordinate of the first dot.
/// @param y1 the y coordinate of the first dot.o
/// @param x2 the x coordinate of the second dot.
/// @param y2 the y coordinate of the second dot.
/// @param style the style of the line.
void Canvas::DrawPointLine(int x1,
                           int y1,
                           int x2,
                           int y2,
                           const Stylizer& style) {
  const int dx = std::abs(x2 - x1);
  const int dy = std::abs(y2 - y1);
  const int sx = x1 < x2 ? 1 : -1;
  const int sy = y1 < y2 ? 1 : -1;
  const int length = std::max(dx, dy);

  if (!IsIn(x1, y1) && !IsIn(x2, y2)) {
    return;
  }
  if (dx + dx > width_ * height_) {
    return;
  }

  int error = dx - dy;
  for (int i = 0; i < length; ++i) {
    DrawPoint(x1, y1, true, style);
    if (2 * error >= -dy) {
      error -= dy;
      x1 += sx;
    }
    if (2 * error <= dx) {
      error += dx;
      y1 += sy;
    }
  }
  DrawPoint(x2, y2, true, style);
}

/// @brief Draw a circle made of braille dots.
/// @param x the x coordinate of the center of the circle.
/// @param y the y coordinate of the center of the circle.
/// @param radius the radius of the circle.
void Canvas::DrawPointCircle(int x, int y, int radius) {
  DrawPointCircle(x, y, radius, [](Pixel& /*pixel*/) {});
}

/// @brief Draw a circle made of braille dots.
/// @param x the x coordinate of the center of the circle.
/// @param y the y coordinate of the center of the circle.
/// @param radius the radius of the circle.
/// @param color the color of the circle.
void Canvas::DrawPointCircle(int x, int y, int radius, const Color& color) {
  DrawPointCircle(x, y, radius,
                  [color](Pixel& p) { p.foreground_color = color; });
}

/// @brief Draw a circle made of braille dots.
/// @param x the x coordinate of the center of the circle.
/// @param y the y coordinate of the center of the circle.
/// @param radius the radius of the circle.
/// @param style the style of the circle.
void Canvas::DrawPointCircle(int x, int y, int radius, const Stylizer& style) {
  DrawPointEllipse(x, y, radius, radius, style);
}

/// @brief Draw a filled circle made of braille dots.
/// @param x the x coordinate of the center of the circle.
/// @param y the y coordinate of the center of the circle.
/// @param radius the radius of the circle.
void Canvas::DrawPointCircleFilled(int x, int y, int radius) {
  DrawPointCircleFilled(x, y, radius, [](Pixel& /*pixel*/) {});
}

/// @brief Draw a filled circle made of braille dots.
/// @param x the x coordinate of the center of the circle.
/// @param y the y coordinate of the center of the circle.
/// @param radius the radius of the circle.
/// @param color the color of the circle.
void Canvas::DrawPointCircleFilled(int x,
                                   int y,
                                   int radius,
                                   const Color& color) {
  DrawPointCircleFilled(x, y, radius,
                        [color](Pixel& p) { p.foreground_color = color; });
}

/// @brief Draw a filled circle made of braille dots.
/// @param x the x coordinate of the center of the circle.
/// @param y the y coordinate of the center of the circle.
/// @param radius the radius of the circle.
/// @param style the style of the circle.
void Canvas::DrawPointCircleFilled(int x,
                                   int y,
                                   int radius,
                                   const Stylizer& style) {
  DrawPointEllipseFilled(x, y, radius, radius, style);
}

/// @brief Draw an ellipse made of braille dots.
/// @param x the x coordinate of the center of the ellipse.
/// @param y the y coordinate of the center of the ellipse.
/// @param r1 the radius of the ellipse along the x axis.
/// @param r2 the radius of the ellipse along the y axis.
void Canvas::DrawPointEllipse(int x, int y, int r1, int r2) {
  DrawPointEllipse(x, y, r1, r2, [](Pixel& /*pixel*/) {});
}

/// @brief Draw an ellipse made of braille dots.
/// @param x the x coordinate of the center of the ellipse.
/// @param y the y coordinate of the center of the ellipse.
/// @param r1 the radius of the ellipse along the x axis.
/// @param r2 the radius of the ellipse along the y axis.
/// @param color the color of the ellipse.
void Canvas::DrawPointEllipse(int x,
                              int y,
                              int r1,
                              int r2,
                              const Color& color) {
  DrawPointEllipse(x, y, r1, r2,
                   [color](Pixel& p) { p.foreground_color = color; });
}

/// @brief Draw an ellipse made of braille dots.
/// @param x the x coordinate of the center of the ellipse.
/// @param y the y coordinate of the center of the ellipse.
/// @param r1 the radius of the ellipse along the x axis.
/// @param r2 the radius of the ellipse along the y axis.
/// @param style the style of the ellipse.
void Canvas::DrawPointEllipse(int x1,
                              int y1,
                              int r1,
                              int r2,
                              const Stylizer& s) {
  int x = -r1;
  int y = 0;
  int e2 = r2;
  int dx = (1 + 2 * x) * e2 * e2;
  int dy = x * x;
  int err = dx + dy;

  do {
    DrawPoint(x1 - x, y1 + y, true, s);
    DrawPoint(x1 + x, y1 + y, true, s);
    DrawPoint(x1 + x, y1 - y, true, s);
    DrawPoint(x1 - x, y1 - y, true, s);
    e2 = 2 * err;
    if (e2 >= dx) {
      x++;
      err += dx += 2 * r2 * r2;
    }
    if (e2 <= dy) {
      y++;
      err += dy += 2 * r1 * r1;
    }
  } while (x <= 0);

  while (y++ < r2) {
    DrawPoint(x1, y1 + y, true, s);
    DrawPoint(x1, y1 - y, true, s);
  }
}

/// @brief Draw a filled ellipse made of braille dots.
/// @param x the x coordinate of the center of the ellipse.
/// @param y the y coordinate of the center of the ellipse.
/// @param r1 the radius of the ellipse along the x axis.
/// @param r2 the radius of the ellipse along the y axis.
void Canvas::DrawPointEllipseFilled(int x1, int y1, int r1, int r2) {
  DrawPointEllipseFilled(x1, y1, r1, r2, [](Pixel& /*pixel*/) {});
}

/// @brief Draw a filled ellipse made of braille dots.
/// @param x the x coordinate of the center of the ellipse.
/// @param y the y coordinate of the center of the ellipse.
/// @param r1 the radius of the ellipse along the x axis.
/// @param r2 the radius of the ellipse along the y axis.
/// @param color the color of the ellipse.
void Canvas::DrawPointEllipseFilled(int x1,
                                    int y1,
                                    int r1,
                                    int r2,
                                    const Color& color) {
  DrawPointEllipseFilled(x1, y1, r1, r2,
                         [color](Pixel& p) { p.foreground_color = color; });
}

/// @brief Draw a filled ellipse made of braille dots.
/// @param x the x coordinate of the center of the ellipse.
/// @param y the y coordinate of the center of the ellipse.
/// @param r1 the radius of the ellipse along the x axis.
/// @param r2 the radius of the ellipse along the y axis.
/// @param style the style of the ellipse.
void Canvas::DrawPointEllipseFilled(int x1,
                                    int y1,
                                    int r1,
                                    int r2,
                                    const Stylizer& s) {
  int x = -r1;
  int y = 0;
  int e2 = r2;
  int dx = (1 + 2 * x) * e2 * e2;
  int dy = x * x;
  int err = dx + dy;

  do {
    for (int xx = x1 + x; xx <= x1 - x; ++xx) {
      DrawPoint(xx, y1 + y, true, s);
      DrawPoint(xx, y1 - y, true, s);
    }
    e2 = 2 * err;
    if (e2 >= dx) {
      x++;
      err += dx += 2 * (long)r2 * r2;  // NOLINT
    }
    if (e2 <= dy) {
      y++;
      err += dy += 2 * (long)r1 * r1;  // NOLINT
    }
  } while (x <= 0);

  while (y++ < r2) {
    for (int yy = y1 - y; yy <= y1 + y; ++yy) {
      DrawPoint(x1, yy, true, s);
    }
  }
}

/// @brief Draw a block.
/// @param x the x coordinate of the block.
/// @param y the y coordinate of the block.
/// @param value whether the block is filled or not.
void Canvas::DrawBlock(int x, int y, bool value) {
  DrawBlock(x, y, value, [](Pixel& /*pixel*/) {});
}

/// @brief Draw a block.
/// @param x the x coordinate of the block.
/// @param y the y coordinate of the block.
/// @param value whether the block is filled or not.
/// @param color the color of the block.
void Canvas::DrawBlock(int x, int y, bool value, const Color& color) {
  DrawBlock(x, y, value, [color](Pixel& p) { p.foreground_color = color; });
}

/// @brief Draw a block.
/// @param x the x coordinate of the block.
/// @param y the y coordinate of the block.
/// @param value whether the block is filled or not.
/// @param style the style of the block.
void Canvas::DrawBlock(int x, int y, bool value, const Stylizer& style) {
  Style(x, y, style);
  if (value) {
    DrawBlockOn(x, y);
  } else {
    DrawBlockOff(x, y);
  }
}

/// @brief Draw a block.
/// @param x the x coordinate of the block.
/// @param y the y coordinate of the block.
void Canvas::DrawBlockOn(int x, int y) {
  if (!IsIn(x, y)) {
    return;
  }
  y /= 2;
  Cell& cell = storage_[XY{x / 2, y / 2}];
  if (cell.type != CellType::kBlock) {
    cell.content.character = " ";
    cell.type = CellType::kBlock;
  }

  const uint8_t bit = (x % 2) * 2 + y % 2;
  uint8_t value = g_map_block_inversed.at(cell.content.character);
  value |= 1U << bit;
  cell.content.character = g_map_block[value];
}

/// @brief Erase a block.
/// @param x the x coordinate of the block.
/// @param y the y coordinate of the block.
void Canvas::DrawBlockOff(int x, int y) {
  if (!IsIn(x, y)) {
    return;
  }
  Cell& cell = storage_[XY{x / 2, y / 4}];
  if (cell.type != CellType::kBlock) {
    cell.content.character = " ";
    cell.type = CellType::kBlock;
  }
  y /= 2;

  const uint8_t bit = (y % 2) * 2 + x % 2;
  uint8_t value = g_map_block_inversed.at(cell.content.character);
  value &= ~(1U << bit);
  cell.content.character = g_map_block[value];
}

/// @brief Toggle a block. If it is filled, it will be erased. If it is empty,
/// it will be filled.
/// @param x the x coordinate of the block.
/// @param y the y coordinate of the block.
void Canvas::DrawBlockToggle(int x, int y) {
  if (!IsIn(x, y)) {
    return;
  }
  Cell& cell = storage_[XY{x / 2, y / 4}];
  if (cell.type != CellType::kBlock) {
    cell.content.character = " ";
    cell.type = CellType::kBlock;
  }
  y /= 2;

  const uint8_t bit = (y % 2) * 2 + x % 2;
  uint8_t value = g_map_block_inversed.at(cell.content.character);
  value ^= 1U << bit;
  cell.content.character = g_map_block[value];
}

/// @brief Draw a line made of block characters.
/// @param x1 the x coordinate of the first point of the line.
/// @param y1 the y coordinate of the first point of the line.
/// @param x2 the x coordinate of the second point of the line.
/// @param y2 the y coordinate of the second point of the line.
void Canvas::DrawBlockLine(int x1, int y1, int x2, int y2) {
  DrawBlockLine(x1, y1, x2, y2, [](Pixel& /*pixel*/) {});
}

/// @brief Draw a line made of block characters.
/// @param x1 the x coordinate of the first point of the line.
/// @param y1 the y coordinate of the first point of the line.
/// @param x2 the x coordinate of the second point of the line.
/// @param y2 the y coordinate of the second point of the line.
/// @param color the color of the line.
void Canvas::DrawBlockLine(int x1, int y1, int x2, int y2, const Color& color) {
  DrawBlockLine(x1, y1, x2, y2,
                [color](Pixel& p) { p.foreground_color = color; });
}

/// @brief Draw a line made of block characters.
/// @param x1 the x coordinate of the first point of the line.
/// @param y1 the y coordinate of the first point of the line.
/// @param x2 the x coordinate of the second point of the line.
/// @param y2 the y coordinate of the second point of the line.
/// @param style the style of the line.
void Canvas::DrawBlockLine(int x1,
                           int y1,
                           int x2,
                           int y2,
                           const Stylizer& style) {
  y1 /= 2;
  y2 /= 2;

  const int dx = std::abs(x2 - x1);
  const int dy = std::abs(y2 - y1);
  const int sx = x1 < x2 ? 1 : -1;
  const int sy = y1 < y2 ? 1 : -1;
  const int length = std::max(dx, dy);

  if (!IsIn(x1, y1) && !IsIn(x2, y2)) {
    return;
  }
  if (dx + dx > width_ * height_) {
    return;
  }

  int error = dx - dy;
  for (int i = 0; i < length; ++i) {
    DrawBlock(x1, y1 * 2, true, style);
    if (2 * error >= -dy) {
      error -= dy;
      x1 += sx;
    }
    if (2 * error <= dx) {
      error += dx;
      y1 += sy;
    }
  }
  DrawBlock(x2, y2 * 2, true, style);
}

/// @brief Draw a circle made of block characters.
/// @param x the x coordinate of the center of the circle.
/// @param y the y coordinate of the center of the circle.
/// @param radius the radius of the circle.
void Canvas::DrawBlockCircle(int x, int y, int radius) {
  DrawBlockCircle(x, y, radius, nostyle);
}

/// @brief Draw a circle made of block characters.
/// @param x the x coordinate of the center of the circle.
/// @param y the y coordinate of the center of the circle.
/// @param radius the radius of the circle.
/// @param color the color of the circle.
void Canvas::DrawBlockCircle(int x, int y, int radius, const Color& color) {
  DrawBlockCircle(x, y, radius,
                  [color](Pixel& p) { p.foreground_color = color; });
}

/// @brief Draw a circle made of block characters.
/// @param x the x coordinate of the center of the circle.
/// @param y the y coordinate of the center of the circle.
/// @param radius the radius of the circle.
/// @param style the style of the circle.
void Canvas::DrawBlockCircle(int x, int y, int radius, const Stylizer& style) {
  DrawBlockEllipse(x, y, radius, radius, style);
}

/// @brief Draw a filled circle made of block characters.
/// @param x the x coordinate of the center of the circle.
/// @param y the y coordinate of the center of the circle.
/// @param radius the radius of the circle.
void Canvas::DrawBlockCircleFilled(int x, int y, int radius) {
  DrawBlockCircleFilled(x, y, radius, nostyle);
}

/// @brief Draw a filled circle made of block characters.
/// @param x the x coordinate of the center of the circle.
/// @param y the y coordinate of the center of the circle.
/// @param radius the radius of the circle.
/// @param color the color of the circle.
void Canvas::DrawBlockCircleFilled(int x,
                                   int y,
                                   int radius,
                                   const Color& color) {
  DrawBlockCircleFilled(x, y, radius,
                        [color](Pixel& p) { p.foreground_color = color; });
}

/// @brief Draw a filled circle made of block characters.
/// @param x the x coordinate of the center of the circle.
/// @param y the y coordinate of the center of the circle.
/// @param radius the radius of the circle.
/// @param style the style of the circle.
void Canvas::DrawBlockCircleFilled(int x,
                                   int y,
                                   int radius,
                                   const Stylizer& s) {
  DrawBlockEllipseFilled(x, y, radius, radius, s);
}

/// @brief Draw an ellipse made of block characters.
/// @param x the x coordinate of the center of the ellipse.
/// @param y the y coordinate of the center of the ellipse.
/// @param r1 the radius of the ellipse along the x axis.
/// @param r2 the radius of the ellipse along the y axis.
void Canvas::DrawBlockEllipse(int x, int y, int r1, int r2) {
  DrawBlockEllipse(x, y, r1, r2, nostyle);
}

/// @brief Draw an ellipse made of block characters.
/// @param x the x coordinate of the center of the ellipse.
/// @param y the y coordinate of the center of the ellipse.
/// @param r1 the radius of the ellipse along the x axis.
/// @param r2 the radius of the ellipse along the y axis.
/// @param color the color of the ellipse.
void Canvas::DrawBlockEllipse(int x,
                              int y,
                              int r1,
                              int r2,
                              const Color& color) {
  DrawBlockEllipse(x, y, r1, r2,
                   [color](Pixel& p) { p.foreground_color = color; });
}

/// @brief Draw an ellipse made of block characters.
/// @param x the x coordinate of the center of the ellipse.
/// @param y the y coordinate of the center of the ellipse.
/// @param r1 the radius of the ellipse along the x axis.
/// @param r2 the radius of the ellipse along the y axis.
/// @param style the style of the ellipse.
void Canvas::DrawBlockEllipse(int x1,
                              int y1,
                              int r1,
                              int r2,
                              const Stylizer& s) {
  y1 /= 2;
  r2 /= 2;
  int x = -r1;
  int y = 0;
  int e2 = r2;
  int dx = (1 + 2 * x) * e2 * e2;
  int dy = x * x;
  int err = dx + dy;

  do {
    DrawBlock(x1 - x, 2 * (y1 + y), true, s);
    DrawBlock(x1 + x, 2 * (y1 + y), true, s);
    DrawBlock(x1 + x, 2 * (y1 - y), true, s);
    DrawBlock(x1 - x, 2 * (y1 - y), true, s);
    e2 = 2 * err;
    if (e2 >= dx) {
      x++;
      err += dx += 2 * r2 * r2;
    }
    if (e2 <= dy) {
      y++;
      err += dy += 2 * r1 * r1;
    }
  } while (x <= 0);

  while (y++ < r2) {
    DrawBlock(x1, 2 * (y1 + y), true, s);
    DrawBlock(x1, 2 * (y1 - y), true, s);
  }
}

/// @brief Draw a filled ellipse made of block characters.
/// @param x the x coordinate of the center of the ellipse.
/// @param y the y coordinate of the center of the ellipse.
/// @param r1 the radius of the ellipse along the x axis.
/// @param r2 the radius of the ellipse along the y axis.
void Canvas::DrawBlockEllipseFilled(int x, int y, int r1, int r2) {
  DrawBlockEllipseFilled(x, y, r1, r2, nostyle);
}

/// @brief Draw a filled ellipse made of block characters.
/// @param x the x coordinate of the center of the ellipse.
/// @param y the y coordinate of the center of the ellipse.
/// @param r1 the radius of the ellipse along the x axis.
/// @param r2 the radius of the ellipse along the y axis.
/// @param color the color of the ellipse.
void Canvas::DrawBlockEllipseFilled(int x,
                                    int y,
                                    int r1,
                                    int r2,
                                    const Color& color) {
  DrawBlockEllipseFilled(x, y, r1, r2,
                         [color](Pixel& p) { p.foreground_color = color; });
}

/// @brief Draw a filled ellipse made of block characters.
/// @param x the x coordinate of the center of the ellipse.
/// @param y the y coordinate of the center of the ellipse.
/// @param r1 the radius of the ellipse along the x axis.
/// @param r2 the radius of the ellipse along the y axis.
/// @param style the style of the ellipse.
void Canvas::DrawBlockEllipseFilled(int x1,
                                    int y1,
                                    int r1,
                                    int r2,
                                    const Stylizer& s) {
  y1 /= 2;
  r2 /= 2;
  int x = -r1;
  int y = 0;
  int e2 = r2;
  int dx = (1 + 2 * x) * e2 * e2;
  int dy = x * x;
  int err = dx + dy;

  do {
    for (int xx = x1 + x; xx <= x1 - x; ++xx) {
      DrawBlock(xx, 2 * (y1 + y), true, s);
      DrawBlock(xx, 2 * (y1 - y), true, s);
    }
    e2 = 2 * err;
    if (e2 >= dx) {
      x++;
      err += dx += 2 * r2 * r2;
    }
    if (e2 <= dy) {
      y++;
      err += dy += 2 * r1 * r1;
    }
  } while (x <= 0);

  while (y++ < r2) {
    for (int yy = y1 + y; yy <= y1 - y; ++yy) {
      DrawBlock(x1, 2 * yy, true, s);
    }
  }
}

/// @brief Draw a piece of text.
/// @param x the x coordinate of the text.
/// @param y the y coordinate of the text.
/// @param value the text to draw.
void Canvas::DrawText(int x, int y, const std::string& value) {
  DrawText(x, y, value, nostyle);
}

/// @brief Draw a piece of text.
/// @param x the x coordinate of the text.
/// @param y the y coordinate of the text.
/// @param value the text to draw.
/// @param color the color of the text.
void Canvas::DrawText(int x,
                      int y,
                      const std::string& value,
                      const Color& color) {
  DrawText(x, y, value, [color](Pixel& p) { p.foreground_color = color; });
}

/// @brief Draw a piece of text.
/// @param x the x coordinate of the text.
/// @param y the y coordinate of the text.
/// @param value the text to draw.
/// @param style the style of the text.
void Canvas::DrawText(int x,
                      int y,
                      const std::string& value,
                      const Stylizer& style) {
  for (const auto& it : Utf8ToGlyphs(value)) {
    if (!IsIn(x, y)) {
      x += 2;
      continue;
    }
    Cell& cell = storage_[XY{x / 2, y / 4}];
    cell.type = CellType::kText;
    cell.content.character = it;
    style(cell.content);
    x += 2;
  }
}

/// @brief Modify a pixel at a given location.
/// @param style a function that modifies the pixel.
void Canvas::Style(int x, int y, const Stylizer& style) {
  if (IsIn(x, y)) {
    style(storage_[XY{x / 2, y / 4}].content);
  }
}

namespace {

class CanvasNodeBase : public Node {
 public:
  CanvasNodeBase() = default;

  void Render(Screen& screen) override {
    const Canvas& c = canvas();
    const int y_max = std::min(c.height() / 4, box_.y_max - box_.y_min + 1);
    const int x_max = std::min(c.width() / 2, box_.x_max - box_.x_min + 1);
    for (int y = 0; y < y_max; ++y) {
      for (int x = 0; x < x_max; ++x) {
        screen.PixelAt(box_.x_min + x, box_.y_min + y) = c.GetPixel(x, y);
      }
    }
  }

  virtual const Canvas& canvas() = 0;
};

}  // namespace

/// @brief Produce an element from a Canvas, or a reference to a Canvas.
Element canvas(ConstRef<Canvas> canvas) {
  class Impl : public CanvasNodeBase {
   public:
    explicit Impl(ConstRef<Canvas> canvas) : canvas_(std::move(canvas)) {
      requirement_.min_x = (canvas_->width() + 1) / 2;
      requirement_.min_y = (canvas_->height() + 3) / 4;
    }
    const Canvas& canvas() final { return *canvas_; }
    ConstRef<Canvas> canvas_;
  };
  return std::make_shared<Impl>(std::move(canvas));
}

/// @brief Produce an element drawing a canvas of requested size.
/// @param width the width of the canvas.
/// @param height the height of the canvas.
/// @param fn a function drawing the canvas.
Element canvas(int width, int height, std::function<void(Canvas&)> fn) {
  class Impl : public CanvasNodeBase {
   public:
    Impl(int width, int height, std::function<void(Canvas&)> fn)
        : width_(width), height_(height), fn_(std::move(fn)) {}

    void ComputeRequirement() final {
      requirement_.min_x = (width_ + 1) / 2;
      requirement_.min_y = (height_ + 3) / 4;
    }

    void Render(Screen& screen) final {
      const int width = (box_.x_max - box_.x_min + 1) * 2;
      const int height = (box_.y_max - box_.y_min + 1) * 4;
      canvas_ = Canvas(width, height);
      fn_(canvas_);
      CanvasNodeBase::Render(screen);
    }

    const Canvas& canvas() final { return canvas_; }
    Canvas canvas_;
    int width_;
    int height_;
    std::function<void(Canvas&)> fn_;
  };
  return std::make_shared<Impl>(width, height, std::move(fn));
}

/// @brief Produce an element drawing a canvas.
/// @param fn a function drawing the canvas.
Element canvas(std::function<void(Canvas&)> fn) {
  const int default_dim = 12;
  return canvas(default_dim, default_dim, std::move(fn));
}

}  // namespace ftxui

// Copyright 2021 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <memory>   // for make_shared
#include <utility>  // for move

namespace ftxui {

using ftxui::Screen;

class ClearUnder : public NodeDecorator {
 public:
  using NodeDecorator::NodeDecorator;

  void Render(Screen& screen) override {
    for (int y = box_.y_min; y <= box_.y_max; ++y) {
      for (int x = box_.x_min; x <= box_.x_max; ++x) {
        screen.PixelAt(x, y) = Pixel();
      }
    }
    Node::Render(screen);
  }
};

/// @brief Before drawing |child|, clear the pixels below. This is useful in
//         combinaison with dbox.
/// @see ftxui::dbox
/// @ingroup dom
Element clear_under(Element element) {
  return std::make_shared<ClearUnder>(std::move(element));
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <memory>   // for make_shared
#include <utility>  // for move

namespace ftxui {

class BgColor : public NodeDecorator {
 public:
  BgColor(Element child, Color color)
      : NodeDecorator(std::move(child)), color_(color) {}

  void Render(Screen& screen) override {
    for (int y = box_.y_min; y <= box_.y_max; ++y) {
      for (int x = box_.x_min; x <= box_.x_max; ++x) {
        screen.PixelAt(x, y).background_color = color_;
      }
    }
    NodeDecorator::Render(screen);
  }

  Color color_;
};

class FgColor : public NodeDecorator {
 public:
  FgColor(Element child, Color color)
      : NodeDecorator(std::move(child)), color_(color) {}

  void Render(Screen& screen) override {
    for (int y = box_.y_min; y <= box_.y_max; ++y) {
      for (int x = box_.x_min; x <= box_.x_max; ++x) {
        screen.PixelAt(x, y).foreground_color = color_;
      }
    }
    NodeDecorator::Render(screen);
  }

  Color color_;
};

/// @brief Set the foreground color of an element.
/// @param color The color of the output element.
/// @param child The input element.
/// @return The output element colored.
/// @ingroup dom
///
/// ### Example
///
/// ```cpp
/// Element document = color(Color::Green, text("Success")),
/// ```
Element color(Color color, Element child) {
  return std::make_shared<FgColor>(std::move(child), color);
}

/// @brief Set the background color of an element.
/// @param color The color of the output element.
/// @param child The input element.
/// @return The output element colored.
/// @ingroup dom
///
/// ### Example
///
/// ```cpp
/// Element document = bgcolor(Color::Green, text("Success")),
/// ```
Element bgcolor(Color color, Element child) {
  return std::make_shared<BgColor>(std::move(child), color);
}

/// @brief Decorate using a foreground color.
/// @param c The foreground color to be applied.
/// @return The Decorator applying the color.
/// @ingroup dom
///
/// ### Example
///
/// ```cpp
/// Element document = text("red") | color(Color::Red);
/// ```
Decorator color(Color c) {
  return [c](Element child) { return color(c, std::move(child)); };
}

/// @brief Decorate using a background color.
/// @param color The background color to be applied.
/// @return The Decorator applying the color.
/// @ingroup dom
///
/// ### Example
///
/// ```cpp
/// Element document = text("red") | bgcolor(Color::Red);
/// ```
Decorator bgcolor(Color color) {
  return [color](Element child) { return bgcolor(color, std::move(child)); };
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

#include <utility>  // for move

namespace ftxui {

/// @brief Center an element horizontally.
/// @param child The decorated element.
/// @return The centered element.
/// @ingroup dom
Element hcenter(Element child) {
  return hbox(filler(), std::move(child), filler());
}

/// @brief Center an element vertically.
/// @param child The decorated element.
/// @return The centered element.
/// @ingroup dom
Element vcenter(Element child) {
  return vbox(filler(), std::move(child), filler());
}

/// @brief Center an element horizontally and vertically.
/// @param child The decorated element.
/// @return The centered element.
/// @ingroup dom
Element center(Element child) {
  return hcenter(vcenter(std::move(child)));
}

/// @brief Align an element on the right side.
/// @param child The decorated element.
/// @return The right aligned element.
/// @ingroup dom
Element align_right(Element child) {
  return hbox(filler(), std::move(child));
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <algorithm>  // for max
#include <memory>     // for __shared_ptr_access, shared_ptr, make_shared
#include <utility>    // for move
#include <vector>     // for vector

namespace ftxui {

class DBox : public Node {
 public:
  explicit DBox(Elements children) : Node(std::move(children)) {}

  void ComputeRequirement() override {
    requirement_.min_x = 0;
    requirement_.min_y = 0;
    requirement_.flex_grow_x = 0;
    requirement_.flex_grow_y = 0;
    requirement_.flex_shrink_x = 0;
    requirement_.flex_shrink_y = 0;
    requirement_.selection = Requirement::NORMAL;
    for (auto& child : children_) {
      child->ComputeRequirement();
      requirement_.min_x =
          std::max(requirement_.min_x, child->requirement().min_x);
      requirement_.min_y =
          std::max(requirement_.min_y, child->requirement().min_y);

      if (requirement_.selection < child->requirement().selection) {
        requirement_.selection = child->requirement().selection;
        requirement_.selected_box = child->requirement().selected_box;
      }
    }
  }

  void SetBox(Box box) override {
    Node::SetBox(box);

    for (auto& child : children_) {
      child->SetBox(box);
    }
  }
};

/// @brief Stack several element on top of each other.
/// @param children_ The input element.
/// @return The right aligned element.
/// @ingroup dom
Element dbox(Elements children_) {
  return std::make_shared<DBox>(std::move(children_));
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <memory>   // for make_shared
#include <utility>  // for move

namespace ftxui {

class Dim : public NodeDecorator {
 public:
  using NodeDecorator::NodeDecorator;

  void Render(Screen& screen) override {
    Node::Render(screen);
    for (int y = box_.y_min; y <= box_.y_max; ++y) {
      for (int x = box_.x_min; x <= box_.x_max; ++x) {
        screen.PixelAt(x, y).dim = true;
      }
    }
  }
};

/// @brief Use a light font, for elements with less emphasis.
/// @ingroup dom
Element dim(Element child) {
  return std::make_shared<Dim>(std::move(child));
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <memory>   // for make_shared, __shared_ptr_access
#include <utility>  // for move
#include <vector>   // for __alloc_traits<>::value_type

namespace ftxui {

namespace {

using FlexFunction = void (*)(Requirement&);

void function_flex_grow(Requirement& r) {
  r.flex_grow_x = 1;
  r.flex_grow_y = 1;
}

void function_xflex_grow(Requirement& r) {
  r.flex_grow_x = 1;
}

void function_yflex_grow(Requirement& r) {
  r.flex_grow_y = 1;
}

void function_flex_shrink(Requirement& r) {
  r.flex_shrink_x = 1;
  r.flex_shrink_y = 1;
}

void function_xflex_shrink(Requirement& r) {
  r.flex_shrink_x = 1;
}

void function_yflex_shrink(Requirement& r) {
  r.flex_shrink_y = 1;
}

void function_flex(Requirement& r) {
  r.flex_grow_x = 1;
  r.flex_grow_y = 1;
  r.flex_shrink_x = 1;
  r.flex_shrink_y = 1;
}

void function_xflex(Requirement& r) {
  r.flex_grow_x = 1;
  r.flex_shrink_x = 1;
}

void function_yflex(Requirement& r) {
  r.flex_grow_y = 1;
  r.flex_shrink_y = 1;
}

void function_not_flex(Requirement& r) {
  r.flex_grow_x = 0;
  r.flex_grow_y = 0;
  r.flex_shrink_x = 0;
  r.flex_shrink_y = 0;
}

}  // namespace

class Flex : public Node {
 public:
  explicit Flex(FlexFunction f) : f_(f) {}
  Flex(FlexFunction f, Element child) : Node(unpack(std::move(child))), f_(f) {}
  void ComputeRequirement() override {
    requirement_.min_x = 0;
    requirement_.min_y = 0;
    if (!children_.empty()) {
      children_[0]->ComputeRequirement();
      requirement_ = children_[0]->requirement();
    }
    f_(requirement_);
  }

  void SetBox(Box box) override {
    if (children_.empty()) {
      return;
    }
    children_[0]->SetBox(box);
  }

  FlexFunction f_;
};

/// @brief An element that will take expand proportionnally to the space left in
/// a container.
/// @ingroup dom
Element filler() {
  return std::make_shared<Flex>(function_flex);
}

/// @brief Make a child element to expand proportionnally to the space left in a
/// container.
/// @ingroup dom
///
/// #### Examples:
///
/// ~~~cpp
///   hbox({
///     text("left") | border ,
///     text("middle") | border | flex,
///     text("right") | border,
///   });
/// ~~~
///
/// #### Output:
///
/// ~~~bash
/// ┌────┐┌─────────────────────────────────────────────────────────┐┌─────┐
/// │left││middle                                                   ││right│
/// └────┘└─────────────────────────────────────────────────────────┘└─────┘
/// ~~~
Element flex(Element child) {
  return std::make_shared<Flex>(function_flex, std::move(child));
}

/// @brief Expand/Minimize if possible/needed on the X axis.
/// @ingroup dom
Element xflex(Element child) {
  return std::make_shared<Flex>(function_xflex, std::move(child));
}

/// @brief Expand/Minimize if possible/needed on the Y axis.
/// @ingroup dom
Element yflex(Element child) {
  return std::make_shared<Flex>(function_yflex, std::move(child));
}

/// @brief Expand if possible.
/// @ingroup dom
Element flex_grow(Element child) {
  return std::make_shared<Flex>(function_flex_grow, std::move(child));
}

/// @brief Expand if possible on the X axis.
/// @ingroup dom
Element xflex_grow(Element child) {
  return std::make_shared<Flex>(function_xflex_grow, std::move(child));
}

/// @brief Expand if possible on the Y axis.
/// @ingroup dom
Element yflex_grow(Element child) {
  return std::make_shared<Flex>(function_yflex_grow, std::move(child));
}

/// @brief Minimize if needed.
/// @ingroup dom
Element flex_shrink(Element child) {
  return std::make_shared<Flex>(function_flex_shrink, std::move(child));
}

/// @brief Minimize if needed on the X axis.
/// @ingroup dom
Element xflex_shrink(Element child) {
  return std::make_shared<Flex>(function_xflex_shrink, std::move(child));
}

/// @brief Minimize if needed on the Y axis.
/// @ingroup dom
Element yflex_shrink(Element child) {
  return std::make_shared<Flex>(function_yflex_shrink, std::move(child));
}

/// @brief Make the element not flexible.
/// @ingroup dom
Element notflex(Element child) {
  return std::make_shared<Flex>(function_not_flex, std::move(child));
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <algorithm>  // for min, max
#include <cstddef>    // for size_t
#include <memory>  // for __shared_ptr_access, shared_ptr, allocator_traits<>::value_type, make_shared
#include <utility>  // for move, swap
#include <vector>   // for vector

#ifndef FTXUI_DOM_FLEXBOX_HELPER_HPP
#define FTXUI_DOM_FLEXBOX_HELPER_HPP

#include <vector>

namespace ftxui {
namespace flexbox_helper {

struct Block {
  // Input:
  int min_size_x = 0;
  int min_size_y = 0;
  int flex_grow_x = 0;
  int flex_grow_y = 0;
  int flex_shrink_x = 0;
  int flex_shrink_y = 0;

  // Output:
  int line;
  int line_position;
  int x = 0;
  int y = 0;
  int dim_x = 0;
  int dim_y = 0;
  bool overflow = false;
};

struct Global {
  std::vector<Block> blocks;
  FlexboxConfig config;
  int size_x;
  int size_y;
};

void Compute(Global& global);

}  // namespace flexbox_helper
}  // namespace ftxui

#endif /* end of include guard: FTXUI_DOM_FLEXBOX_HELPER_HPP*/

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

namespace ftxui {

namespace {
void Normalize(FlexboxConfig::Direction& direction) {
  switch (direction) {
    case FlexboxConfig::Direction::Row:
    case FlexboxConfig::Direction::RowInversed: {
      direction = FlexboxConfig::Direction::Row;
    } break;
    case FlexboxConfig::Direction::Column:
    case FlexboxConfig::Direction::ColumnInversed: {
      direction = FlexboxConfig::Direction::Column;
    } break;
  }
}

void Normalize(FlexboxConfig::AlignContent& align_content) {
  align_content = FlexboxConfig::AlignContent::FlexStart;
}

void Normalize(FlexboxConfig::JustifyContent& justify_content) {
  justify_content = FlexboxConfig::JustifyContent::FlexStart;
}

void Normalize(FlexboxConfig::Wrap& wrap) {
  wrap = FlexboxConfig::Wrap::Wrap;
}

FlexboxConfig Normalize(FlexboxConfig config) {
  Normalize(config.direction);
  Normalize(config.wrap);
  Normalize(config.justify_content);
  Normalize(config.align_content);
  return config;
}

class Flexbox : public Node {
 public:
  Flexbox(Elements children, FlexboxConfig config)
      : Node(std::move(children)),
        config_(config),
        config_normalized_(Normalize(config)) {
    requirement_.flex_grow_x = 1;
    requirement_.flex_grow_y = 0;

    if (IsColumnOriented()) {
      std::swap(requirement_.flex_grow_x, requirement_.flex_grow_y);
    }
  }

  bool IsColumnOriented() const {
    return config_.direction == FlexboxConfig::Direction::Column ||
           config_.direction == FlexboxConfig::Direction::ColumnInversed;
  }

  void Layout(flexbox_helper::Global& global,
              bool compute_requirement = false) {
    for (auto& child : children_) {
      flexbox_helper::Block block;
      block.min_size_x = child->requirement().min_x;
      block.min_size_y = child->requirement().min_y;
      if (!compute_requirement) {
        block.flex_grow_x = child->requirement().flex_grow_x;
        block.flex_grow_y = child->requirement().flex_grow_y;
        block.flex_shrink_x = child->requirement().flex_shrink_x;
        block.flex_shrink_y = child->requirement().flex_shrink_y;
      }
      global.blocks.push_back(block);
    }

    flexbox_helper::Compute(global);
  }

  void ComputeRequirement() override {
    for (auto& child : children_) {
      child->ComputeRequirement();
    }
    flexbox_helper::Global global;
    global.config = config_normalized_;
    if (IsColumnOriented()) {
      global.size_x = 100000;  // NOLINT
      global.size_y = asked_;
    } else {
      global.size_x = asked_;
      global.size_y = 100000;  // NOLINT
    }
    Layout(global, true);

    // Reset:
    requirement_.selection = Requirement::Selection::NORMAL;
    requirement_.selected_box = Box();
    requirement_.min_x = 0;
    requirement_.min_y = 0;

    if (global.blocks.empty()) {
      return;
    }

    // Compute the union of all the blocks:
    Box box;
    box.x_min = global.blocks[0].x;
    box.y_min = global.blocks[0].y;
    box.x_max = global.blocks[0].x + global.blocks[0].dim_x;
    box.y_max = global.blocks[0].y + global.blocks[0].dim_y;
    for (auto& b : global.blocks) {
      box.x_min = std::min(box.x_min, b.x);
      box.y_min = std::min(box.y_min, b.y);
      box.x_max = std::max(box.x_max, b.x + b.dim_x);
      box.y_max = std::max(box.y_max, b.y + b.dim_y);
    }
    requirement_.min_x = box.x_max - box.x_min;
    requirement_.min_y = box.y_max - box.y_min;

    // Find the selection:
    for (size_t i = 0; i < children_.size(); ++i) {
      if (requirement_.selection >= children_[i]->requirement().selection) {
        continue;
      }
      requirement_.selection = children_[i]->requirement().selection;
      Box selected_box = children_[i]->requirement().selected_box;

      // Shift |selected_box| according to its position inside this component:
      auto& b = global.blocks[i];
      selected_box.x_min += b.x;
      selected_box.y_min += b.y;
      selected_box.x_max += b.x;
      selected_box.y_max += b.y;
      requirement_.selected_box = Box::Intersection(selected_box, box);
    }
  }

  void SetBox(Box box) override {
    Node::SetBox(box);

    const int asked_previous = asked_;
    asked_ = std::min(asked_, IsColumnOriented() ? box.y_max - box.y_min + 1
                                                 : box.x_max - box.x_min + 1);
    need_iteration_ = (asked_ != asked_previous);

    flexbox_helper::Global global;
    global.config = config_;
    global.size_x = box.x_max - box.x_min + 1;
    global.size_y = box.y_max - box.y_min + 1;
    Layout(global);

    for (size_t i = 0; i < children_.size(); ++i) {
      auto& child = children_[i];
      auto& b = global.blocks[i];

      Box children_box;
      children_box.x_min = box.x_min + b.x;
      children_box.y_min = box.y_min + b.y;
      children_box.x_max = box.x_min + b.x + b.dim_x - 1;
      children_box.y_max = box.y_min + b.y + b.dim_y - 1;

      const Box intersection = Box::Intersection(children_box, box);
      child->SetBox(intersection);

      need_iteration_ |= (intersection != children_box);
    }
  }

  void Check(Status* status) override {
    for (auto& child : children_) {
      child->Check(status);
    }

    if (status->iteration == 0) {
      asked_ = 6000;  // NOLINT
      need_iteration_ = true;
    }

    status->need_iteration |= need_iteration_;
  }

  int asked_ = 6000;  // NOLINT
  bool need_iteration_ = true;
  const FlexboxConfig config_;
  const FlexboxConfig config_normalized_;
};

}  // namespace

/// @brief A container displaying elements on row/columns and capable of
/// wrapping on the next column/row when full.
/// @param children The elements in the container
/// @param config The option
/// @return The container.
///
/// #### Example
///
/// ```cpp
/// flexbox({
///   text("element 1"),
///   text("element 2"),
///   text("element 3"),
/// }, FlexboxConfig()
//       .Set(FlexboxConfig::Direction::Column)
//       .Set(FlexboxConfig::Wrap::WrapInversed)
//       .SetGapMainAxis(1)
//       .SetGapCrossAxis(1)
//  )
/// ```
Element flexbox(Elements children, FlexboxConfig config) {
  return std::make_shared<Flexbox>(std::move(children), config);
}

/// @brief A container displaying elements in rows from left to right. When
/// filled, it starts on a new row below.
/// @param children The elements in the container
/// @return The container.
///
/// #### Example
///
/// ```cpp
/// hflow({
///   text("element 1"),
///   text("element 2"),
///   text("element 3"),
/// });
/// ```
Element hflow(Elements children) {
  return flexbox(std::move(children), FlexboxConfig());
}

/// @brief A container displaying elements in rows from top to bottom. When
/// filled, it starts on a new columns on the right.
/// filled, it starts on a new row.
/// is full, it starts a new row.
/// @param children The elements in the container
/// @return The container.
///
/// #### Example
///
/// ```cpp
/// vflow({
///   text("element 1"),
///   text("element 2"),
///   text("element 3"),
/// });
/// ```
Element vflow(Elements children) {
  return flexbox(std::move(children),
                 FlexboxConfig().Set(FlexboxConfig::Direction::Column));
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

namespace ftxui {

FlexboxConfig& FlexboxConfig::Set(FlexboxConfig::Direction d) {
  this->direction = d;
  return *this;
}

FlexboxConfig& FlexboxConfig::Set(FlexboxConfig::Wrap w) {
  this->wrap = w;
  return *this;
}

FlexboxConfig& FlexboxConfig::Set(FlexboxConfig::JustifyContent j) {
  this->justify_content = j;
  return *this;
}

FlexboxConfig& FlexboxConfig::Set(FlexboxConfig::AlignItems a) {
  this->align_items = a;
  return *this;
}

FlexboxConfig& FlexboxConfig::Set(FlexboxConfig::AlignContent a) {
  this->align_content = a;
  return *this;
}

FlexboxConfig& FlexboxConfig::SetGap(int x, int y) {
  this->gap_x = x;
  this->gap_y = y;
  return *this;
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

#include <algorithm>                     // for max, min
#include <cstddef>                       // for size_t
#include <memory>                        // for allocator_traits<>::value_type
#include <utility>                       // for swap, move

namespace ftxui::flexbox_helper {

namespace {
void SymmetryXY(FlexboxConfig& c) {
  std::swap(c.gap_x, c.gap_y);
  switch (c.direction) {
    case FlexboxConfig::Direction::Row:
      c.direction = FlexboxConfig::Direction::Column;
      break;
    case FlexboxConfig::Direction::RowInversed:
      c.direction = FlexboxConfig::Direction::ColumnInversed;
      break;
    case FlexboxConfig::Direction::Column:
      c.direction = FlexboxConfig::Direction::Row;
      break;
    case FlexboxConfig::Direction::ColumnInversed:
      c.direction = FlexboxConfig::Direction::RowInversed;
      break;
  }
}

void SymmetryX(FlexboxConfig& c) {
  switch (c.direction) {
    case FlexboxConfig::Direction::Row:
      c.direction = FlexboxConfig::Direction::RowInversed;
      break;
    case FlexboxConfig::Direction::RowInversed:
      c.direction = FlexboxConfig::Direction::Row;
      break;
    default:
      break;
  }
}

void SymmetryY(FlexboxConfig& c) {
  switch (c.wrap) {
    case FlexboxConfig::Wrap::NoWrap:
      break;
    case FlexboxConfig::Wrap::Wrap:
      c.wrap = FlexboxConfig::Wrap::WrapInversed;
      break;
    case FlexboxConfig::Wrap::WrapInversed:
      c.wrap = FlexboxConfig::Wrap::Wrap;
      break;
  }
}

void SymmetryXY(Global& g) {
  SymmetryXY(g.config);
  std::swap(g.size_x, g.size_y);
  for (auto& b : g.blocks) {
    std::swap(b.min_size_x, b.min_size_y);
    std::swap(b.flex_grow_x, b.flex_grow_y);
    std::swap(b.flex_shrink_x, b.flex_shrink_y);
    std::swap(b.x, b.y);
    std::swap(b.dim_x, b.dim_y);
  }
}

void SymmetryX(Global& g) {
  SymmetryX(g.config);
  for (auto& b : g.blocks) {
    b.x = g.size_x - b.x - b.dim_x;
  }
}

void SymmetryY(Global& g) {
  SymmetryY(g.config);
  for (auto& b : g.blocks) {
    b.y = g.size_y - b.y - b.dim_y;
  }
}

struct Line {
  std::vector<Block*> blocks;
};

void SetX(Global& global, std::vector<Line> lines) {
  for (auto& line : lines) {
    std::vector<box_helper::Element> elements;
    for (auto* block : line.blocks) {
      box_helper::Element element;
      element.min_size = block->min_size_x;
      element.flex_grow =
          block->flex_grow_x != 0 || global.config.justify_content ==
                                         FlexboxConfig::JustifyContent::Stretch
              ? 1
              : 0;
      element.flex_shrink = block->flex_shrink_x;
      elements.push_back(element);
    }

    box_helper::Compute(
        &elements,
        global.size_x - global.config.gap_x * (int(line.blocks.size()) - 1));

    int x = 0;
    for (size_t i = 0; i < line.blocks.size(); ++i) {
      line.blocks[i]->dim_x = elements[i].size;
      line.blocks[i]->x = x;
      x += elements[i].size;
      x += global.config.gap_x;
    }
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void SetY(Global& g, std::vector<Line> lines) {
  std::vector<box_helper::Element> elements;
  for (auto& line : lines) {
    box_helper::Element element;
    element.flex_shrink = line.blocks.front()->flex_shrink_y;
    element.flex_grow = line.blocks.front()->flex_grow_y;
    for (auto* block : line.blocks) {
      element.min_size = std::max(element.min_size, block->min_size_y);
      element.flex_shrink = std::min(element.flex_shrink, block->flex_shrink_y);
      element.flex_grow = std::min(element.flex_grow, block->flex_grow_y);
    }
    elements.push_back(element);
  }

  // box_helper::Compute(&elements, g.size_y);
  box_helper::Compute(&elements, 10000);  // NOLINT

  // [Align-content]
  std::vector<int> ys(elements.size());
  int y = 0;
  for (size_t i = 0; i < elements.size(); ++i) {
    ys[i] = y;
    y += elements[i].size;
    y += g.config.gap_y;
  }
  int remaining_space = std::max(0, g.size_y - y);
  switch (g.config.align_content) {
    case FlexboxConfig::AlignContent::FlexStart: {
      break;
    }

    case FlexboxConfig::AlignContent::FlexEnd: {
      for (size_t i = 0; i < ys.size(); ++i) {  // NOLINT
        ys[i] += remaining_space;
      }
      break;
    }

    case FlexboxConfig::AlignContent::Center: {
      for (size_t i = 0; i < ys.size(); ++i) {  // NOLINT
        ys[i] += remaining_space / 2;
      }
      break;
    }

    case FlexboxConfig::AlignContent::Stretch: {
      for (int i = static_cast<int>(ys.size()) - 1; i >= 0; --i) {  // NOLINT
        const int shifted = remaining_space * (i + 0) / (i + 1);
        ys[i] += shifted;
        const int consumed = remaining_space - shifted;
        elements[i].size += consumed;
        remaining_space -= consumed;
      }
      break;
    }

    case FlexboxConfig::AlignContent::SpaceBetween: {
      for (int i = static_cast<int>(ys.size()) - 1; i >= 1; --i) {  // NOLINT
        ys[i] += remaining_space;
        remaining_space = remaining_space * (i - 1) / i;
      }
      break;
    }

    case FlexboxConfig::AlignContent::SpaceAround: {
      for (int i = static_cast<int>(ys.size()) - 1; i >= 0; --i) {  // NOLINT
        ys[i] += remaining_space * (2 * i + 1) / (2 * i + 2);
        remaining_space = remaining_space * (2 * i) / (2 * i + 2);
      }
      break;
    }

    case FlexboxConfig::AlignContent::SpaceEvenly: {
      for (int i = static_cast<int>(ys.size()) - 1; i >= 0; --i) {  // NOLINT
        ys[i] += remaining_space * (i + 1) / (i + 2);
        remaining_space = remaining_space * (i + 1) / (i + 2);
      }
      break;
    }
  }

  // [Align items]
  for (size_t i = 0; i < lines.size(); ++i) {
    auto& element = elements[i];
    for (auto* block : lines[i].blocks) {
      const bool stretch =
          block->flex_grow_y != 0 ||
          g.config.align_content == FlexboxConfig::AlignContent::Stretch;
      const int size =
          stretch ? element.size : std::min(element.size, block->min_size_y);
      switch (g.config.align_items) {
        case FlexboxConfig::AlignItems::FlexStart: {
          block->y = ys[i];
          block->dim_y = size;
          break;
        }

        case FlexboxConfig::AlignItems::Center: {
          block->y = ys[i] + (element.size - size) / 2;
          block->dim_y = size;
          break;
        }

        case FlexboxConfig::AlignItems::FlexEnd: {
          block->y = ys[i] + element.size - size;
          block->dim_y = size;
          break;
        }

        case FlexboxConfig::AlignItems::Stretch: {
          block->y = ys[i];
          block->dim_y = element.size;
          break;
        }
      }
    }
  }
}

void JustifyContent(Global& g, std::vector<Line> lines) {
  for (auto& line : lines) {
    Block* last = line.blocks.back();
    int remaining_space = g.size_x - last->x - last->dim_x;
    switch (g.config.justify_content) {
      case FlexboxConfig::JustifyContent::FlexStart:
      case FlexboxConfig::JustifyContent::Stretch:
        break;

      case FlexboxConfig::JustifyContent::FlexEnd: {
        for (auto* block : line.blocks) {
          block->x += remaining_space;
        }
        break;
      }

      case FlexboxConfig::JustifyContent::Center: {
        for (auto* block : line.blocks) {
          block->x += remaining_space / 2;
        }
        break;
      }

      case FlexboxConfig::JustifyContent::SpaceBetween: {
        for (int i = (int)line.blocks.size() - 1; i >= 1; --i) {
          line.blocks[i]->x += remaining_space;
          remaining_space = remaining_space * (i - 1) / i;
        }
        break;
      }

      case FlexboxConfig::JustifyContent::SpaceAround: {
        for (int i = (int)line.blocks.size() - 1; i >= 0; --i) {
          line.blocks[i]->x += remaining_space * (2 * i + 1) / (2 * i + 2);
          remaining_space = remaining_space * (2 * i) / (2 * i + 2);
        }
        break;
      }

      case FlexboxConfig::JustifyContent::SpaceEvenly: {
        for (int i = (int)line.blocks.size() - 1; i >= 0; --i) {
          line.blocks[i]->x += remaining_space * (i + 1) / (i + 2);
          remaining_space = remaining_space * (i + 1) / (i + 2);
        }
        break;
      }
    }
  }
}
}  // namespace

namespace {

void Compute1(Global& global);
void Compute2(Global& global);
void Compute3(Global& global);

void Compute1(Global& global) {
  if (global.config.direction == FlexboxConfig::Direction::RowInversed) {
    SymmetryX(global);
    Compute2(global);
    SymmetryX(global);
    return;
  }
  Compute2(global);
}

void Compute2(Global& global) {
  if (global.config.wrap == FlexboxConfig::Wrap::WrapInversed) {
    SymmetryY(global);
    Compute3(global);
    SymmetryY(global);
    return;
  }
  Compute3(global);
}

void Compute3(Global& global) {
  // Step 1: Lay out every elements into rows:
  std::vector<Line> lines;
  {
    Line line;
    int x = 0;
    for (auto& block : global.blocks) {
      // Does it fit the end of the row?
      // No? Then we need to start a new one:
      if (x + block.min_size_x > global.size_x) {
        x = 0;
        if (!line.blocks.empty()) {
          lines.push_back(std::move(line));
        }
        line = Line();
      }

      block.line = (int)lines.size();
      block.line_position = (int)line.blocks.size();
      line.blocks.push_back(&block);
      x += block.min_size_x + global.config.gap_x;
    }
    if (!line.blocks.empty()) {
      lines.push_back(std::move(line));
    }
  }

  // Step 2: Set positions on the X axis.
  SetX(global, lines);
  JustifyContent(global, lines);  // Distribute remaining space.

  // Step 3: Set positions on the Y axis.
  SetY(global, lines);
}

}  // namespace

void Compute(Global& global) {
  if (global.config.direction == FlexboxConfig::Direction::Column ||
      global.config.direction == FlexboxConfig::Direction::ColumnInversed) {
    SymmetryXY(global);
    Compute1(global);
    SymmetryXY(global);
    return;
  }
  Compute1(global);
}

}  // namespace ftxui::flexbox_helper

// Copyright 2021 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <memory>   // for make_shared
#include <utility>  // for move

namespace ftxui {

/// @brief Used inside a `frame`, this force the view to be scrolled toward a
/// a given position. The position is expressed in proportion of the requested
/// size.
///
/// For instance:
/// - (0, 0) means that the view is scrolled toward the upper left.
/// - (1, 0) means that the view is scrolled toward the upper right.
/// - (0, 1) means that the view is scrolled toward the bottom left.
/// @ingroup dom
///
/// ### Example
///
/// ```cpp
/// Element document = huge_document()
///   | focusPositionRelative(0.f, 1.f)
///   | frame;
/// ```
Decorator focusPositionRelative(float x, float y) {
  class Impl : public NodeDecorator {
   public:
    Impl(Element child, float x, float y)
        : NodeDecorator(std::move(child)), x_(x), y_(y) {}

    void ComputeRequirement() override {
      NodeDecorator::ComputeRequirement();
      requirement_.selection = Requirement::Selection::NORMAL;

      Box& box = requirement_.selected_box;
      box.x_min = (int)((float)requirement_.min_x * x_);
      box.y_min = (int)((float)requirement_.min_y * y_);
      box.x_max = (int)((float)requirement_.min_x * x_);
      box.y_max = (int)((float)requirement_.min_y * y_);
    }

   private:
    const float x_;
    const float y_;
  };

  return [x, y](Element child) {
    return std::make_shared<Impl>(std::move(child), x, y);
  };
}

/// @brief Used inside a `frame`, this force the view to be scrolled toward a
/// a given position. The position is expressed in the numbers of cells.
///
/// @ingroup dom
///
/// ### Example
///
/// ```cpp
/// Element document = huge_document()
///   | focusPosition(10, 10)
///   | frame;
/// ```
Decorator focusPosition(int x, int y) {
  class Impl : public NodeDecorator {
   public:
    Impl(Element child, int x, int y)
        : NodeDecorator(std::move(child)), x_(x), y_(y) {}

    void ComputeRequirement() override {
      NodeDecorator::ComputeRequirement();
      requirement_.selection = Requirement::Selection::NORMAL;

      Box& box = requirement_.selected_box;
      box.x_min = x_;
      box.y_min = y_;
      box.x_max = x_;
      box.y_max = y_;
    }

   private:
    const int x_;
    const int y_;
  };

  return [x, y](Element child) {
    return std::make_shared<Impl>(std::move(child), x, y);
  };
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <algorithm>  // for max, min
#include <memory>     // for make_shared, __shared_ptr_access
#include <utility>    // for move
#include <vector>     // for __alloc_traits<>::value_type

#ifndef FTXUI_UTIL_AUTORESET_HPP
#define FTXUI_UTIL_AUTORESET_HPP

#include <utility>

namespace ftxui {

/// Assign a value to a variable, reset its old value when going out of scope.
template <typename T>
class AutoReset {
 public:
  AutoReset(T* variable, T new_value)
      : variable_(variable), previous_value_(std::move(*variable)) {
    *variable_ = std::move(new_value);
  }
  ~AutoReset() { *variable_ = std::move(previous_value_); }

 private:
  T* variable_;
  T previous_value_;
};

}  // namespace ftxui

#endif /* end of include guard: FTXUI_UTIL_AUTORESET_HPP */

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

namespace ftxui {

// -----------------------------------------------------------------------------

class Select : public Node {
 public:
  explicit Select(Elements children) : Node(std::move(children)) {}

  void ComputeRequirement() override {
    Node::ComputeRequirement();
    requirement_ = children_[0]->requirement();
    auto& selected_box = requirement_.selected_box;
    selected_box.x_min = 0;
    selected_box.y_min = 0;
    selected_box.x_max = requirement_.min_x - 1;
    selected_box.y_max = requirement_.min_y - 1;
    requirement_.selection = Requirement::SELECTED;
  };

  void SetBox(Box box) override {
    Node::SetBox(box);
    children_[0]->SetBox(box);
  }
};

Element select(Element child) {
  return std::make_shared<Select>(unpack(std::move(child)));
}

// -----------------------------------------------------------------------------

class Focus : public Select {
 public:
  using Select::Select;

  void ComputeRequirement() override {
    Select::ComputeRequirement();
    requirement_.selection = Requirement::FOCUSED;
  };

  void Render(Screen& screen) override {
    Select::Render(screen);

    // Setting the cursor to the right position allow folks using CJK (China,
    // Japanese, Korean, ...) characters to see their [input method editor]
    // displayed at the right location. See [issue].
    //
    // [input method editor]:
    // https://en.wikipedia.org/wiki/Input_method
    //
    // [issue]:
    // https://github.com/ArthurSonzogni/FTXUI/issues/2#issuecomment-505282355
    //
    // Unfortunately, Microsoft terminal do not handle properly hidding the
    // cursor. Instead the character under the cursor is hidden, which is a big
    // problem. As a result, we can't enable setting cursor to the right
    // location. It will be displayed at the bottom right corner.
    // See:
    // https://github.com/microsoft/terminal/issues/1203
    // https://github.com/microsoft/terminal/issues/3093
#if !defined(FTXUI_MICROSOFT_TERMINAL_FALLBACK)
    screen.SetCursor(Screen::Cursor{
        box_.x_min,
        box_.y_min,
        Screen::Cursor::Shape::Hidden,
    });
#endif
  }
};

Element focus(Element child) {
  return std::make_shared<Focus>(unpack(std::move(child)));
}

// -----------------------------------------------------------------------------

class Frame : public Node {
 public:
  Frame(Elements children, bool x_frame, bool y_frame)
      : Node(std::move(children)), x_frame_(x_frame), y_frame_(y_frame) {}

  void ComputeRequirement() override {
    Node::ComputeRequirement();
    requirement_ = children_[0]->requirement();
  }

  void SetBox(Box box) override {
    Node::SetBox(box);
    auto& selected_box = requirement_.selected_box;
    Box children_box = box;

    if (x_frame_) {
      const int external_dimx = box.x_max - box.x_min;
      const int internal_dimx = std::max(requirement_.min_x, external_dimx);
      const int focused_dimx = selected_box.x_max - selected_box.x_min;
      int dx = selected_box.x_min - external_dimx / 2 + focused_dimx / 2;
      dx = std::max(0, std::min(internal_dimx - external_dimx - 1, dx));
      children_box.x_min = box.x_min - dx;
      children_box.x_max = box.x_min + internal_dimx - dx;
    }

    if (y_frame_) {
      const int external_dimy = box.y_max - box.y_min;
      const int internal_dimy = std::max(requirement_.min_y, external_dimy);
      const int focused_dimy = selected_box.y_max - selected_box.y_min;
      int dy = selected_box.y_min - external_dimy / 2 + focused_dimy / 2;
      dy = std::max(0, std::min(internal_dimy - external_dimy - 1, dy));
      children_box.y_min = box.y_min - dy;
      children_box.y_max = box.y_min + internal_dimy - dy;
    }

    children_[0]->SetBox(children_box);
  }

  void Render(Screen& screen) override {
    const AutoReset<Box> stencil(&screen.stencil,
                                 Box::Intersection(box_, screen.stencil));
    children_[0]->Render(screen);
  }

 private:
  bool x_frame_;
  bool y_frame_;
};

/// @brief Allow an element to be displayed inside a 'virtual' area. It size can
/// be larger than its container. In this case only a smaller portion is
/// displayed. The view is scrollable to make the focused element visible.
/// @see focus
Element frame(Element child) {
  return std::make_shared<Frame>(unpack(std::move(child)), true, true);
}

Element xframe(Element child) {
  return std::make_shared<Frame>(unpack(std::move(child)), true, false);
}

Element yframe(Element child) {
  return std::make_shared<Frame>(unpack(std::move(child)), false, true);
}

class FocusCursor : public Focus {
 public:
  FocusCursor(Elements children, Screen::Cursor::Shape shape)
      : Focus(std::move(children)), shape_(shape) {}

 private:
  void Render(Screen& screen) override {
    Select::Render(screen);  // NOLINT
    screen.SetCursor(Screen::Cursor{
        box_.x_min,
        box_.y_min,
        shape_,
    });
  }
  Screen::Cursor::Shape shape_;
};

Element focusCursorBlock(Element child) {
  return std::make_shared<FocusCursor>(unpack(std::move(child)),
                                       Screen::Cursor::Block);
}
Element focusCursorBlockBlinking(Element child) {
  return std::make_shared<FocusCursor>(unpack(std::move(child)),
                                       Screen::Cursor::BlockBlinking);
}
Element focusCursorBar(Element child) {
  return std::make_shared<FocusCursor>(unpack(std::move(child)),
                                       Screen::Cursor::Bar);
}
Element focusCursorBarBlinking(Element child) {
  return std::make_shared<FocusCursor>(unpack(std::move(child)),
                                       Screen::Cursor::BarBlinking);
}
Element focusCursorUnderline(Element child) {
  return std::make_shared<FocusCursor>(unpack(std::move(child)),
                                       Screen::Cursor::Underline);
}
Element focusCursorUnderlineBlinking(Element child) {
  return std::make_shared<FocusCursor>(unpack(std::move(child)),
                                       Screen::Cursor::UnderlineBlinking);
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <memory>  // for allocator, make_shared
#include <string>  // for string

namespace ftxui {

// NOLINTNEXTLINE
static const std::string charset_horizontal[11] = {
#if defined(FTXUI_MICROSOFT_TERMINAL_FALLBACK)
    // Microsoft's terminals often use fonts not handling the 8 unicode
    // characters for representing the whole gauge. Fallback with less.
    " ", " ", " ", " ", "▌", "▌", "▌", "█", "█", "█",
#else
    " ", " ", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█",
#endif
    // An extra character in case when the fuzzer manage to have:
    // int(9 * (limit - limit_int) = 9
    "█"};

// NOLINTNEXTLINE
static const std::string charset_vertical[10] = {
    "█",
    "▇",
    "▆",
    "▅",
    "▄",
    "▃",
    "▂",
    "▁",
    " ",
    // An extra character in case when the fuzzer manage to have:
    // int(8 * (limit - limit_int) = 8
    " ",
};

class Gauge : public Node {
 public:
  Gauge(float progress, GaugeDirection direction)
      : progress_(progress), direction_(direction) {
    // This handle NAN correctly:
    if (!(progress_ > 0.F)) {
      progress_ = 0.F;
    }
    if (!(progress_ < 1.F)) {
      progress_ = 1.F;
    }
  }

  void ComputeRequirement() override {
    switch (direction_) {
      case GaugeDirection::Right:
      case GaugeDirection::Left:
        requirement_.flex_grow_x = 1;
        requirement_.flex_grow_y = 0;
        requirement_.flex_shrink_x = 1;
        requirement_.flex_shrink_y = 0;
        break;
      case GaugeDirection::Up:
      case GaugeDirection::Down:
        requirement_.flex_grow_x = 0;
        requirement_.flex_grow_y = 1;
        requirement_.flex_shrink_x = 0;
        requirement_.flex_shrink_y = 1;
        break;
    }
    requirement_.min_x = 1;
    requirement_.min_y = 1;
  }

  void Render(Screen& screen) override {
    switch (direction_) {
      case GaugeDirection::Right:
        RenderHorizontal(screen, /*invert=*/false);
        break;
      case GaugeDirection::Up:
        RenderVertical(screen, /*invert=*/false);
        break;
      case GaugeDirection::Left:
        RenderHorizontal(screen, /*invert=*/true);
        break;
      case GaugeDirection::Down:
        RenderVertical(screen, /*invert=*/true);
        break;
    }
  }

  void RenderHorizontal(Screen& screen, bool invert) {
    const int y = box_.y_min;
    if (y > box_.y_max) {
      return;
    }

    // Draw the progress bar horizontally.
    {
      const float progress = invert ? 1.F - progress_ : progress_;
      const float limit =
          (float)box_.x_min + progress * (float)(box_.x_max - box_.x_min + 1);
      const int limit_int = (int)limit;
      int x = box_.x_min;
      while (x < limit_int) {
        screen.at(x++, y) = charset_horizontal[9];  // NOLINT
      }
      // NOLINTNEXTLINE
      screen.at(x++, y) = charset_horizontal[int(9 * (limit - limit_int))];
      while (x <= box_.x_max) {
        screen.at(x++, y) = charset_horizontal[0];
      }
    }

    if (invert) {
      for (int x = box_.x_min; x <= box_.x_max; x++) {
        screen.PixelAt(x, y).inverted ^= true;
      }
    }
  }

  void RenderVertical(Screen& screen, bool invert) {
    const int x = box_.x_min;
    if (x > box_.x_max) {
      return;
    }

    // Draw the progress bar vertically:
    {
      const float progress = invert ? progress_ : 1.F - progress_;
      const float limit =
          (float)box_.y_min + progress * (float)(box_.y_max - box_.y_min + 1);
      const int limit_int = (int)limit;
      int y = box_.y_min;
      while (y < limit_int) {
        screen.at(x, y++) = charset_vertical[8];  // NOLINT
      }
      // NOLINTNEXTLINE
      screen.at(x, y++) = charset_vertical[int(8 * (limit - limit_int))];
      while (y <= box_.y_max) {
        screen.at(x, y++) = charset_vertical[0];
      }
    }

    if (invert) {
      for (int y = box_.y_min; y <= box_.y_max; y++) {
        screen.PixelAt(x, y).inverted ^= true;
      }
    }
  }

 private:
  float progress_;
  GaugeDirection direction_;
};

/// @brief Draw a high definition progress bar progressing in specified
/// direction.
/// @param progress The proportion of the area to be filled. Belong to [0,1].
//  @param direction Direction of progress bars progression.
/// @ingroup dom
Element gaugeDirection(float progress, GaugeDirection direction) {
  return std::make_shared<Gauge>(progress, direction);
}

/// @brief Draw a high definition progress bar progressing from left to right.
/// @param progress The proportion of the area to be filled. Belong to [0,1].
/// @ingroup dom
///
/// ### Example
///
/// A gauge. It can be used to represent a progress bar.
/// ~~~cpp
/// border(gaugeRight(0.5))
/// ~~~
///
/// #### Output
///
/// ~~~bash
/// ┌──────────────────────────────────────────────────────────────────────────┐
/// │█████████████████████████████████████                                     │
/// └──────────────────────────────────────────────────────────────────────────┘
/// ~~~
Element gaugeRight(float progress) {
  return gaugeDirection(progress, GaugeDirection::Right);
}

/// @brief Draw a high definition progress bar progressing from right to left.
/// @param progress The proportion of the area to be filled. Belong to [0,1].
/// @ingroup dom
///
/// ### Example
///
/// A gauge. It can be used to represent a progress bar.
/// ~~~cpp
/// border(gaugeLeft(0.5))
/// ~~~
///
/// #### Output
///
/// ~~~bash
/// ┌──────────────────────────────────────────────────────────────────────────┐
/// │                                     █████████████████████████████████████│
/// └──────────────────────────────────────────────────────────────────────────┘
/// ~~~
Element gaugeLeft(float progress) {
  return gaugeDirection(progress, GaugeDirection::Left);
}

/// @brief Draw a high definition progress bar progressing from bottom to top.
/// @param progress The proportion of the area to be filled. Belong to [0,1].
/// @ingroup dom
///
/// ### Example
///
/// A gauge. It can be used to represent a progress bar.
/// ~~~cpp
/// border(gaugeUp(0.5))
/// ~~~
///
/// #### Output
///
/// ~~~bash
///  ┌─┐
///  │ │
///  │ │
///  │ │
///  │ │
///  │█│
///  │█│
///  │█│
///  │█│
///  └─┘
/// ~~~
Element gaugeUp(float progress) {
  return gaugeDirection(progress, GaugeDirection::Up);
}

/// @brief Draw a high definition progress bar progressing from top to bottom.
/// @param progress The proportion of the area to be filled. Belong to [0,1].
/// @ingroup dom
///
/// ### Example
///
/// A gauge. It can be used to represent a progress bar.
/// ~~~cpp
/// border(gaugeDown(0.5))
/// ~~~
///
/// #### Output
///
/// ~~~bash
///  ┌─┐
///  │█│
///  │█│
///  │█│
///  │█│
///  │ │
///  │ │
///  │ │
///  │ │
///  └─┘
/// ~~~
Element gaugeDown(float progress) {
  return gaugeDirection(progress, GaugeDirection::Down);
}

/// @brief Draw a high definition progress bar.
/// @param progress The proportion of the area to be filled. Belong to [0,1].
/// @ingroup dom
///
/// ### Example
///
/// A gauge. It can be used to represent a progress bar.
/// ~~~cpp
/// border(gauge(0.5))
/// ~~~
///
/// #### Output
///
/// ~~~bash
/// ┌──────────────────────────────────────────────────────────────────────────┐
/// │█████████████████████████████████████                                     │
/// └──────────────────────────────────────────────────────────────────────────┘
/// ~~~
Element gauge(float progress) {
  return gaugeRight(progress);
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <functional>  // for function
#include <memory>      // for allocator, make_shared
#include <string>      // for string
#include <utility>     // for move
#include <vector>      // for vector

namespace ftxui {

// NOLINTNEXTLINE
static std::string charset_graph[] =
#if defined(FTXUI_MICROSOFT_TERMINAL_FALLBACK)
    // Microsoft's terminals often use fonts not handling the 8 unicode
    // characters for representing the whole graph. Fallback with less.
    {" ", " ", "█", " ", "█", "█", "█", "█", "█"};
#else
    {" ", "▗", "▐", "▖", "▄", "▟", "▌", "▙", "█"};
#endif

class Graph : public Node {
 public:
  explicit Graph(GraphFunction graph_function)
      : graph_function_(std::move(graph_function)) {}

  void ComputeRequirement() override {
    requirement_.flex_grow_x = 1;
    requirement_.flex_grow_y = 1;
    requirement_.flex_shrink_x = 1;
    requirement_.flex_shrink_y = 1;
    requirement_.min_x = 3;
    requirement_.min_y = 3;
  }

  void Render(Screen& screen) override {
    const int width = (box_.x_max - box_.x_min + 1) * 2;
    const int height = (box_.y_max - box_.y_min + 1) * 2;
    if (width <= 0 || height <= 0) {
      return;
    }
    auto data = graph_function_(width, height);
    int i = 0;
    for (int x = box_.x_min; x <= box_.x_max; ++x) {
      const int height_1 = 2 * box_.y_max - data[i++];
      const int height_2 = 2 * box_.y_max - data[i++];
      for (int y = box_.y_min; y <= box_.y_max; ++y) {
        const int yy = 2 * y;
        int i_1 = yy < height_1 ? 0 : yy == height_1 ? 3 : 6;  // NOLINT
        int i_2 = yy < height_2 ? 0 : yy == height_2 ? 1 : 2;  // NOLINT
        screen.at(x, y) = charset_graph[i_1 + i_2];                  // NOLINT
      }
    }
  }

 private:
  GraphFunction graph_function_;
};

/// @brief Draw a graph using a GraphFunction.
/// @param graph_function the function to be called to get the data.
Element graph(GraphFunction graph_function) {
  return std::make_shared<Graph>(std::move(graph_function));
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <algorithm>  // for max, min
#include <cstddef>    // for size_t
#include <memory>  // for __shared_ptr_access, shared_ptr, make_shared, allocator_traits<>::value_type
#include <utility>  // for move
#include <vector>   // for vector, __alloc_traits<>::value_type

namespace ftxui {
class Screen;

namespace {

// Accumulate the values of a list U[n] into v[n]. So that:
// V[0] = 0;
// V[n+1] = v[n] + U[n]
// return the sum of U[n].
int Integrate(std::vector<int>& elements) {
  int accu = 0;
  for (auto& i : elements) {
    const int old_accu = accu;
    accu += i;
    i = old_accu;
  }
  return accu;
}
}  // namespace

class GridBox : public Node {
 public:
  explicit GridBox(std::vector<Elements> lines) : lines_(std::move(lines)) {
    y_size = (int)lines_.size();
    for (const auto& line : lines_) {
      x_size = std::max(x_size, (int)line.size());
    }
    for (auto& line : lines_) {
      while (line.size() < (size_t)x_size) {
        line.push_back(filler());
      }
    }
  }

  void ComputeRequirement() override {
    requirement_.min_x = 0;
    requirement_.min_y = 0;
    requirement_.flex_grow_x = 0;
    requirement_.flex_grow_y = 0;
    requirement_.flex_shrink_x = 0;
    requirement_.flex_shrink_y = 0;

    for (auto& line : lines_) {
      for (auto& cell : line) {
        cell->ComputeRequirement();
      }
    }

    // Compute the size of each columns/row.
    std::vector<int> size_x(x_size, 0);
    std::vector<int> size_y(y_size, 0);
    for (int x = 0; x < x_size; ++x) {
      for (int y = 0; y < y_size; ++y) {
        size_x[x] = std::max(size_x[x], lines_[y][x]->requirement().min_x);
        size_y[y] = std::max(size_y[y], lines_[y][x]->requirement().min_y);
      }
    }

    requirement_.min_x = Integrate(size_x);
    requirement_.min_y = Integrate(size_y);

    // Forward the selected/focused child state:
    requirement_.selection = Requirement::NORMAL;
    for (int x = 0; x < x_size; ++x) {
      for (int y = 0; y < y_size; ++y) {
        if (requirement_.selection >= lines_[y][x]->requirement().selection) {
          continue;
        }
        requirement_.selection = lines_[y][x]->requirement().selection;
        requirement_.selected_box = lines_[y][x]->requirement().selected_box;
        requirement_.selected_box.x_min += size_x[x];
        requirement_.selected_box.x_max += size_x[x];
        requirement_.selected_box.y_min += size_y[y];
        requirement_.selected_box.y_max += size_y[y];
      }
    }
  }

  void SetBox(Box box) override {
    Node::SetBox(box);

    box_helper::Element init;
    init.min_size = 0;
    init.flex_grow = 1024;    // NOLINT
    init.flex_shrink = 1024;  // NOLINT
    std::vector<box_helper::Element> elements_x(x_size, init);
    std::vector<box_helper::Element> elements_y(y_size, init);

    for (int y = 0; y < y_size; ++y) {
      for (int x = 0; x < x_size; ++x) {
        const auto& cell = lines_[y][x];
        const auto& requirement = cell->requirement();
        auto& e_x = elements_x[x];
        auto& e_y = elements_y[y];
        e_x.min_size = std::max(e_x.min_size, requirement.min_x);
        e_y.min_size = std::max(e_y.min_size, requirement.min_y);
        e_x.flex_grow = std::min(e_x.flex_grow, requirement.flex_grow_x);
        e_y.flex_grow = std::min(e_y.flex_grow, requirement.flex_grow_y);
        e_x.flex_shrink = std::min(e_x.flex_shrink, requirement.flex_shrink_x);
        e_y.flex_shrink = std::min(e_y.flex_shrink, requirement.flex_shrink_y);
      }
    }

    const int target_size_x = box.x_max - box.x_min + 1;
    const int target_size_y = box.y_max - box.y_min + 1;
    box_helper::Compute(&elements_x, target_size_x);
    box_helper::Compute(&elements_y, target_size_y);

    Box box_y = box;
    int y = box_y.y_min;
    for (int iy = 0; iy < y_size; ++iy) {
      box_y.y_min = y;
      y += elements_y[iy].size;
      box_y.y_max = y - 1;

      Box box_x = box_y;
      int x = box_x.x_min;
      for (int ix = 0; ix < x_size; ++ix) {
        box_x.x_min = x;
        x += elements_x[ix].size;
        box_x.x_max = x - 1;
        lines_[iy][ix]->SetBox(box_x);
      }
    }
  }

  void Render(Screen& screen) override {
    for (auto& line : lines_) {
      for (auto& cell : line) {
        cell->Render(screen);
      }
    }
  }

  int x_size = 0;
  int y_size = 0;
  std::vector<Elements> lines_;
};

/// @brief A container displaying a grid of elements.
/// @param lines A list of lines, each line being a list of elements.
/// @return The container.
///
/// #### Example
///
/// ```cpp
/// auto cell = [](const char* t) { return text(t) | border; };
/// auto document = gridbox({
///   {cell("north-west") , cell("north")  , cell("north-east")} ,
///   {cell("west")       , cell("center") , cell("east")}       ,
///   {cell("south-west") , cell("south")  , cell("south-east")} ,
/// });
/// ```
/// Output:
/// ```
/// ╭──────────╮╭──────╮╭──────────╮
/// │north-west││north ││north-east│
/// ╰──────────╯╰──────╯╰──────────╯
/// ╭──────────╮╭──────╮╭──────────╮
/// │west      ││center││east      │
/// ╰──────────╯╰──────╯╰──────────╯
/// ╭──────────╮╭──────╮╭──────────╮
/// │south-west││south ││south-east│
/// ╰──────────╯╰──────╯╰──────────╯
/// ```
Element gridbox(std::vector<Elements> lines) {
  return std::make_shared<GridBox>(std::move(lines));
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <algorithm>  // for max
#include <cstddef>    // for size_t
#include <memory>  // for __shared_ptr_access, shared_ptr, make_shared, allocator_traits<>::value_type
#include <utility>  // for move
#include <vector>   // for vector, __alloc_traits<>::value_type

namespace ftxui {

class HBox : public Node {
 public:
  explicit HBox(Elements children) : Node(std::move(children)) {}

  void ComputeRequirement() override {
    requirement_.min_x = 0;
    requirement_.min_y = 0;
    requirement_.flex_grow_x = 0;
    requirement_.flex_grow_y = 0;
    requirement_.flex_shrink_x = 0;
    requirement_.flex_shrink_y = 0;
    requirement_.selection = Requirement::NORMAL;
    for (auto& child : children_) {
      child->ComputeRequirement();
      if (requirement_.selection < child->requirement().selection) {
        requirement_.selection = child->requirement().selection;
        requirement_.selected_box = child->requirement().selected_box;
        requirement_.selected_box.x_min += requirement_.min_x;
        requirement_.selected_box.x_max += requirement_.min_x;
      }
      requirement_.min_x += child->requirement().min_x;
      requirement_.min_y =
          std::max(requirement_.min_y, child->requirement().min_y);
    }
  }

  void SetBox(Box box) override {
    Node::SetBox(box);

    std::vector<box_helper::Element> elements(children_.size());
    for (size_t i = 0; i < children_.size(); ++i) {
      auto& element = elements[i];
      const auto& requirement = children_[i]->requirement();
      element.min_size = requirement.min_x;
      element.flex_grow = requirement.flex_grow_x;
      element.flex_shrink = requirement.flex_shrink_x;
    }
    const int target_size = box.x_max - box.x_min + 1;
    box_helper::Compute(&elements, target_size);

    int x = box.x_min;
    for (size_t i = 0; i < children_.size(); ++i) {
      box.x_min = x;
      box.x_max = x + elements[i].size - 1;
      children_[i]->SetBox(box);
      x = box.x_max + 1;
    }
  }
};

/// @brief A container displaying elements horizontally one by one.
/// @param children The elements in the container
/// @return The container.
///
/// #### Example
///
/// ```cpp
/// hbox({
///   text("Left"),
///   text("Right"),
/// });
/// ```
Element hbox(Elements children) {
  return std::make_shared<HBox>(std::move(children));
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <memory>   // for make_shared
#include <utility>  // for move

namespace ftxui {

class Inverted : public NodeDecorator {
 public:
  using NodeDecorator::NodeDecorator;

  void Render(Screen& screen) override {
    Node::Render(screen);
    for (int y = box_.y_min; y <= box_.y_max; ++y) {
      for (int x = box_.x_min; x <= box_.x_max; ++x) {
        screen.PixelAt(x, y).inverted ^= true;
      }
    }
  }
};

/// @brief Add a filter that will invert the foreground and the background
/// colors.
/// @ingroup dom
Element inverted(Element child) {
  return std::make_shared<Inverted>(std::move(child));
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <stddef.h>                       // for size_t
#include <algorithm>                      // for max, min, sort, copy
#include <cmath>                          // for fmod, cos, sin
#include <memory>    // for allocator_traits<>::value_type, make_shared
// #include <optional>  // provided by cxx11_compat.hpp
#include <utility>   // for move
#include <vector>    // for vector

namespace ftxui {
namespace {

struct LinearGradientNormalized {
  float angle = 0.f;
  std::vector<Color> colors;
  std::vector<float> positions;  // Sorted.

  LinearGradientNormalized() = default;
  LinearGradientNormalized(float a,
                           std::vector<Color> c,
                           std::vector<float> p)
      : angle(a), colors(std::move(c)), positions(std::move(p)) {}
};

// Convert a LinearGradient to a normalized version.
LinearGradientNormalized Normalize(LinearGradient gradient) {
  // Handle gradient of size 0.
  if (gradient.stops.size() == 0) {
    return LinearGradientNormalized{
        0.f, {Color::Default, Color::Default}, {0.f, 1.f}};
  }

  // Fill in the two extent, if not provided.
  if (!gradient.stops.front().position) {
    gradient.stops.front().position = 0.f;
  }
  if (!gradient.stops.back().position) {
    gradient.stops.back().position = 1.f;
  }

  // Fill in the blank, by interpolating positions.
  size_t last_checkpoint = 0;
  for (size_t i = 1; i < gradient.stops.size(); ++i) {
    if (!gradient.stops[i].position) {
      continue;
    }

    if (i - last_checkpoint >= 2) {
      const float min = gradient.stops[i].position.value();
      const float max = gradient.stops[last_checkpoint].position.value();
      for (size_t j = last_checkpoint + 1; j < i; ++j) {
        gradient.stops[j].position =
            min + (max - min) * (j - last_checkpoint) / (i - last_checkpoint);
      }
    }

    last_checkpoint = i;
  }

  // Sort the stops by position.
  std::sort(
      gradient.stops.begin(), gradient.stops.end(),
      [](const struct LinearGradient::Stop& a,
         const struct LinearGradient::Stop& b) { return a.position < b.position; });

  // If we don't being with zero, add a stop at zero.
  if (gradient.stops.front().position != 0) {
    gradient.stops.insert(gradient.stops.begin(),
                          {gradient.stops.front().color, 0.f});
  }
  // If we don't end with one, add a stop at one.
  if (gradient.stops.back().position != 1) {
    gradient.stops.push_back({gradient.stops.back().color, 1.f});
  }

  // Normalize the angle.
  LinearGradientNormalized normalized;
  normalized.angle = std::fmod(std::fmod(gradient.angle, 360.f) + 360.f, 360.f);
  for (auto& stop : gradient.stops) {
    normalized.colors.push_back(stop.color);
    normalized.positions.push_back(stop.position.value());
  }
  return normalized;
}

Color Interpolate(const LinearGradientNormalized& gradient, float t) {
  // Find the right color in the gradient's stops.
  size_t i = 1;
  while (true) {
    if (i > gradient.positions.size()) {
      return Color::Interpolate(0.5f, gradient.colors.back(),
                                gradient.colors.back());
    }
    if (t <= gradient.positions[i]) {
      break;
    }
    ++i;
  }

  const float t0 = gradient.positions[i - 1];
  const float t1 = gradient.positions[i - 0];
  const float tt = (t - t0) / (t1 - t0);

  const Color& c0 = gradient.colors[i - 1];
  const Color& c1 = gradient.colors[i - 0];
  const Color& cc = Color::Interpolate(tt, c0, c1);

  return cc;
}

class LinearGradientColor : public NodeDecorator {
 public:
  explicit LinearGradientColor(Element child,
                               const LinearGradient& gradient,
                               bool background_color)
      : NodeDecorator(std::move(child)),
        gradient_(Normalize(gradient)),
        background_color_{background_color} {}

 private:
  void Render(Screen& screen) override {
    const float degtorad = 0.01745329251f;
    const float dx = std::cos(gradient_.angle * degtorad);
    const float dy = std::sin(gradient_.angle * degtorad);

    // Project every corner to get the extent of the gradient.
    const float p1 = box_.x_min * dx + box_.y_min * dy;
    const float p2 = box_.x_min * dx + box_.y_max * dy;
    const float p3 = box_.x_max * dx + box_.y_min * dy;
    const float p4 = box_.x_max * dx + box_.y_max * dy;
    const float min = std::min({p1, p2, p3, p4});
    const float max = std::max({p1, p2, p3, p4});

    // Renormalize the projection to [0, 1] using the extent and projective
    // geometry.
    const float dX = dx / (max - min);
    const float dY = dy / (max - min);
    const float dZ = -min / (max - min);

    // Project every pixel to get the color.
    if (background_color_) {
      for (int y = box_.y_min; y <= box_.y_max; ++y) {
        for (int x = box_.x_min; x <= box_.x_max; ++x) {
          const float t = x * dX + y * dY + dZ;
          screen.PixelAt(x, y).background_color = Interpolate(gradient_, t);
        }
      }
    } else {
      for (int y = box_.y_min; y <= box_.y_max; ++y) {
        for (int x = box_.x_min; x <= box_.x_max; ++x) {
          const float t = x * dX + y * dY + dZ;
          screen.PixelAt(x, y).foreground_color = Interpolate(gradient_, t);
        }
      }
    }

    NodeDecorator::Render(screen);
  }

  LinearGradientNormalized gradient_;
  bool background_color_;
};

}  // namespace

/// @brief Build the "empty" gradient. This is often followed by calls to
/// LinearGradient::Angle() and LinearGradient::Stop().
/// Example:
/// ```cpp
///  auto gradient =
///   LinearGradient()
///    .Angle(45)
///    .Stop(Color::Red, 0.0)
///    .Stop(Color::Green, 0.5)
///    .Stop(Color::Blue, 1.0);;
/// ```
/// @ingroup dom
LinearGradient::LinearGradient() = default;

/// @brief Build a gradient with two colors.
/// @param begin The color at the beginning of the gradient.
/// @param end The color at the end of the gradient.
/// @ingroup dom
LinearGradient::LinearGradient(Color begin, Color end) {
  stops.push_back({begin, {}});
  stops.push_back({end, {}});
}

/// @brief Build a gradient with two colors and an angle.
/// @param a The angle of the gradient.
/// @param begin The color at the beginning of the gradient.
/// @param end The color at the end of the gradient.
/// @ingroup dom
LinearGradient::LinearGradient(float a, Color begin, Color end) {
  angle = a;
  stops.push_back({begin, {}});
  stops.push_back({end, {}});
}

/// @brief Set the angle of the gradient.
/// @param a The angle of the gradient.
/// @return The gradient.
/// @ingroup dom
LinearGradient& LinearGradient::Angle(float a) {
  angle = a;
  return *this;
}

/// @brief Add a color stop to the gradient.
/// @param c The color of the stop.
/// @param p The position of the stop.
/// @return The gradient.
LinearGradient& LinearGradient::Stop(Color c, float p) {
  stops.push_back({c, p});
  return *this;
}

/// @brief Add a color stop to the gradient.
/// @param c The color of the stop.
/// @return The gradient.
/// @ingroup dom
/// @note The position of the stop is interpolated from nearby stops.
LinearGradient& LinearGradient::Stop(Color c) {
  stops.push_back({c, {}});
  return *this;
}

/// @brief Set the foreground color of an element with linear-gradient effect.
/// @param gradient The gradient effect to be applied on the output element.
/// @param child The input element.
/// @return The output element colored.
/// @ingroup dom
///
/// ### Example
///
/// ```cpp
/// color(LinearGradient{0, {Color::Red, Color::Blue}}, text("Hello"))
/// ```
Element color(const LinearGradient& gradient, Element child) {
  return std::make_shared<LinearGradientColor>(std::move(child), gradient,
                                               /*background_color*/ false);
}

/// @brief Set the background color of an element with linear-gradient effect.
/// @param gradient The gradient effect to be applied on the output element.
/// @param child The input element.
/// @return The output element colored.
/// @ingroup dom
///
/// ### Example
///
/// ```cpp
/// bgcolor(LinearGradient{0, {Color::Red, Color::Blue}}, text("Hello"))
/// ```
Element bgcolor(const LinearGradient& gradient, Element child) {
  return std::make_shared<LinearGradientColor>(std::move(child), gradient,
                                               /*background_color*/ true);
}

/// @brief Decorate using a linear-gradient effect on the foreground color.
/// @param gradient The gradient effect to be applied on the output element.
/// @return The Decorator applying the color.
/// @ingroup dom
///
/// ### Example
///
/// ```cpp
/// text("Hello") | color(LinearGradient{0, {Color::Red, Color::Blue}})
/// ```
Decorator color(const LinearGradient& gradient) {
  return
      [gradient](Element child) { return color(gradient, std::move(child)); };
}

/// @brief Decorate using a linear-gradient effect on the background color.
/// @param gradient The gradient effect to be applied on the output element.
/// @return The Decorator applying the color.
/// @ingroup dom
///
/// ### Example
///
/// ```cpp
/// text("Hello") | color(LinearGradient{0, {Color::Red, Color::Blue}})
/// ```
Decorator bgcolor(const LinearGradient& gradient) {
  return
      [gradient](Element child) { return bgcolor(gradient, std::move(child)); };
}

}  // namespace ftxui

// Copyright 2023 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <utility>               // for move

namespace ftxui {

Node::Node() = default;
Node::Node(Elements children) : children_(std::move(children)) {}
Node::~Node() = default;

/// @brief Compute how much space an elements needs.
/// @ingroup dom
void Node::ComputeRequirement() {
  for (auto& child : children_) {
    child->ComputeRequirement();
  }
}

/// @brief Assign a position and a dimension to an element for drawing.
/// @ingroup dom
void Node::SetBox(Box box) {
  box_ = box;
}

/// @brief Display an element on a ftxui::Screen.
/// @ingroup dom
void Node::Render(Screen& screen) {
  for (auto& child : children_) {
    child->Render(screen);
  }
}

void Node::Check(Status* status) {
  for (auto& child : children_) {
    child->Check(status);
  }
  status->need_iteration |= (status->iteration == 0);
}

/// @brief Display an element on a ftxui::Screen.
/// @ingroup dom
void Render(Screen& screen, const Element& element) {
  Render(screen, element.get());
}

/// @brief Display an element on a ftxui::Screen.
/// @ingroup dom
void Render(Screen& screen, Node* node) {
  Box box;
  box.x_min = 0;
  box.y_min = 0;
  box.x_max = screen.dimx() - 1;
  box.y_max = screen.dimy() - 1;

  Node::Status status;
  node->Check(&status);
  const int max_iterations = 20;
  while (status.need_iteration && status.iteration < max_iterations) {
    // Step 1: Find what dimension this elements wants to be.
    node->ComputeRequirement();

    // Step 2: Assign a dimension to the element.
    node->SetBox(box);

    // Check if the element needs another iteration of the layout algorithm.
    status.need_iteration = false;
    status.iteration++;
    node->Check(&status);
  }

  // Step 3: Draw the element.
  screen.stencil = box;
  node->Render(screen);

  // Step 4: Apply shaders
  screen.ApplyShader();
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <memory>              // for __shared_ptr_access
#include <vector>              // for __alloc_traits<>::value_type

namespace ftxui {

void NodeDecorator::ComputeRequirement() {
  Node::ComputeRequirement();
  requirement_ = children_[0]->requirement();
}

void NodeDecorator::SetBox(Box box) {
  Node::SetBox(box);
  children_[0]->SetBox(box);
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <sstream>  // for basic_istream, stringstream
#include <string>   // for string, allocator, getline
#include <utility>  // for move

namespace ftxui {

namespace {
Elements Split(const std::string& the_text) {
  Elements output;
  std::stringstream ss(the_text);
  std::string word;
  while (std::getline(ss, word, ' ')) {
    output.push_back(text(word));
  }
  return output;
}
}  // namespace

/// @brief Return an element drawing the paragraph on multiple lines.
/// @ingroup dom
/// @see flexbox.
Element paragraph(const std::string& the_text) {
  return paragraphAlignLeft(the_text);
}

/// @brief Return an element drawing the paragraph on multiple lines, aligned on
/// the left.
/// @ingroup dom
/// @see flexbox.
Element paragraphAlignLeft(const std::string& the_text) {
  static const auto config = FlexboxConfig().SetGap(1, 0);
  return flexbox(Split(the_text), config);
}

/// @brief Return an element drawing the paragraph on multiple lines, aligned on
/// the right.
/// @ingroup dom
/// @see flexbox.
Element paragraphAlignRight(const std::string& the_text) {
  static const auto config =
      FlexboxConfig().SetGap(1, 0).Set(FlexboxConfig::JustifyContent::FlexEnd);
  return flexbox(Split(the_text), config);
}

/// @brief Return an element drawing the paragraph on multiple lines, aligned on
/// the center.
/// @ingroup dom
/// @see flexbox.
Element paragraphAlignCenter(const std::string& the_text) {
  static const auto config =
      FlexboxConfig().SetGap(1, 0).Set(FlexboxConfig::JustifyContent::Center);
  return flexbox(Split(the_text), config);
}

/// @brief Return an element drawing the paragraph on multiple lines, aligned
/// using a justified alignment.
/// the center.
/// @ingroup dom
/// @see flexbox.
Element paragraphAlignJustify(const std::string& the_text) {
  static const auto config = FlexboxConfig().SetGap(1, 0).Set(
      FlexboxConfig::JustifyContent::SpaceBetween);
  Elements words = Split(the_text);
  words.push_back(text("") | xflex);
  return flexbox(std::move(words), config);
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <memory>   // for make_shared, __shared_ptr_access
#include <utility>  // for move
#include <vector>   // for __alloc_traits<>::value_type

namespace ftxui {

// Helper class.
class Reflect : public Node {
 public:
  Reflect(Element child, Box& box)
      : Node(unpack(std::move(child))), reflected_box_(box) {}

  void ComputeRequirement() final {
    Node::ComputeRequirement();
    requirement_ = children_[0]->requirement();
  }

  void SetBox(Box box) final {
    reflected_box_ = box;
    Node::SetBox(box);
    children_[0]->SetBox(box);
  }

  void Render(Screen& screen) final {
    reflected_box_ = Box::Intersection(screen.stencil, reflected_box_);
    return Node::Render(screen);
  }

 private:
  Box& reflected_box_;
};

Decorator reflect(Box& box) {
  return [&](Element child) -> Element {
    return std::make_shared<Reflect>(std::move(child), box);
  };
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <algorithm>  // for max
#include <memory>     // for make_shared, __shared_ptr_access
#include <string>     // for string
#include <utility>    // for move
#include <vector>     // for __alloc_traits<>::value_type

namespace ftxui {

/// @brief Add a filter that will invert the foreground and the background
/// colors.
/// @ingroup dom
Element vscroll_indicator(Element child) {
  class Impl : public NodeDecorator {
    using NodeDecorator::NodeDecorator;

    void ComputeRequirement() override {
      NodeDecorator::ComputeRequirement();
      requirement_ = children_[0]->requirement();
      requirement_.min_x++;
    }

    void SetBox(Box box) override {
      box_ = box;
      box.x_max--;
      children_[0]->SetBox(box);
    }

    void Render(Screen& screen) final {
      NodeDecorator::Render(screen);

      const Box& stencil = screen.stencil;

      const int size_inner = box_.y_max - box_.y_min;
      if (size_inner <= 0) {
        return;
      }
      const int size_outter = stencil.y_max - stencil.y_min + 1;
      if (size_outter >= size_inner) {
        return;
      }

      int size = 2 * size_outter * size_outter / size_inner;
      size = std::max(size, 1);

      const int start_y =
          2 * stencil.y_min +  //
          2 * (stencil.y_min - box_.y_min) * size_outter / size_inner;

      const int x = stencil.x_max;
      for (int y = stencil.y_min; y <= stencil.y_max; ++y) {
        const int y_up = 2 * y + 0;
        const int y_down = 2 * y + 1;
        const bool up = (start_y <= y_up) && (y_up <= start_y + size);
        const bool down = (start_y <= y_down) && (y_down <= start_y + size);

        const char* c = up ? (down ? "┃" : "╹") : (down ? "╻" : " ");  // NOLINT
        screen.PixelAt(x, y) = Pixel();
        screen.PixelAt(x, y).character = c;
      }
    };
  };
  return std::make_shared<Impl>(std::move(child));
}

}  // namespace ftxui

// Copyright 2021 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <array>    // for array, array<>::value_type
#include <memory>   // for make_shared, allocator
#include <string>   // for basic_string, string
#include <utility>  // for move

namespace ftxui {

namespace {
using Charset = std::array<std::string, 2>;  // NOLINT
using Charsets = std::array<Charset, 6>;     // NOLINT
// NOLINTNEXTLINE
const Charsets charsets = {
    Charset{"│", "─"},  // LIGHT
    Charset{"╏", "╍"},  // DASHED
    Charset{"┃", "━"},  // HEAVY
    Charset{"║", "═"},  // DOUBLE
    Charset{"│", "─"},  // ROUNDED
    Charset{" ", " "},  // EMPTY
};

}  // namespace

class Separator : public Node {
 public:
  explicit Separator(std::string value) : value_(std::move(value)) {}

  void ComputeRequirement() override {
    requirement_.min_x = 1;
    requirement_.min_y = 1;
  }

  void Render(Screen& screen) override {
    for (int y = box_.y_min; y <= box_.y_max; ++y) {
      for (int x = box_.x_min; x <= box_.x_max; ++x) {
        Pixel& pixel = screen.PixelAt(x, y);
        pixel.character = value_;
        pixel.automerge = true;
      }
    }
  }

  std::string value_;
};

class SeparatorAuto : public Node {
 public:
  explicit SeparatorAuto(BorderStyle style) : style_(style) {}

  void ComputeRequirement() override {
    requirement_.min_x = 1;
    requirement_.min_y = 1;
  }

  void Render(Screen& screen) override {
    const bool is_column = (box_.x_max == box_.x_min);
    const bool is_line = (box_.y_min == box_.y_max);

    const std::string c = charsets[style_][int(is_line && !is_column)];

    for (int y = box_.y_min; y <= box_.y_max; ++y) {
      for (int x = box_.x_min; x <= box_.x_max; ++x) {
        Pixel& pixel = screen.PixelAt(x, y);
        pixel.character = c;
        pixel.automerge = true;
      }
    }
  }

  BorderStyle style_;
};

class SeparatorWithPixel : public SeparatorAuto {
 public:
  explicit SeparatorWithPixel(Pixel pixel)
      : SeparatorAuto(LIGHT), pixel_(std::move(pixel)) {
    pixel_.automerge = true;
  }
  void Render(Screen& screen) override {
    for (int y = box_.y_min; y <= box_.y_max; ++y) {
      for (int x = box_.x_min; x <= box_.x_max; ++x) {
        screen.PixelAt(x, y) = pixel_;
      }
    }
  }

 private:
  Pixel pixel_;
};

/// @brief Draw a vertical or horizontal separation in between two other
/// elements.
/// @ingroup dom
/// @see separator
/// @see separatorLight
/// @see separatorDashed
/// @see separatorDouble
/// @see separatorHeavy
/// @see separatorEmpty
/// @see separatorRounded
/// @see separatorStyled
/// @see separatorCharacter
///
/// Add a visual separation in between two elements.
///
/// ### Example
///
/// ```cpp
/// // Use 'border' as a function...
/// Element document = vbox({
///   text("up"),
///   separator(),
///   text("down"),
/// });
/// ```
///
/// ### Output
///
/// ```bash
/// up
/// ────
/// down
/// ```
Element separator() {
  return std::make_shared<SeparatorAuto>(LIGHT);
}

/// @brief Draw a vertical or horizontal separation in between two other
/// elements.
/// @param style the style of the separator.
/// @ingroup dom
/// @see separator
/// @see separatorLight
/// @see separatorDashed
/// @see separatorDouble
/// @see separatorHeavy
/// @see separatorEmpty
/// @see separatorRounded
/// @see separatorStyled
/// @see separatorCharacter
///
/// Add a visual separation in between two elements.
///
/// ### Example
///
/// ```cpp
/// // Use 'border' as a function...
/// Element document = vbox({
///   text("up"),
///   separatorStyled(DOUBLE),
///   text("down"),
/// });
/// ```
///
/// ### Output
///
/// ```bash
/// up
/// ════
/// down
/// ```
Element separatorStyled(BorderStyle style) {
  return std::make_shared<SeparatorAuto>(style);
}

/// @brief Draw a vertical or horizontal separation in between two other
/// elements, using the LIGHT style.
/// @ingroup dom
/// @see separator
/// @see separatorLight
/// @see separatorDashed
/// @see separatorDouble
/// @see separatorHeavy
/// @see separatorEmpty
/// @see separatorRounded
/// @see separatorStyled
/// @see separatorCharacter
///
/// Add a visual separation in between two elements.
///
/// ### Example
///
/// ```cpp
/// // Use 'border' as a function...
/// Element document = vbox({
///   text("up"),
///   separatorLight(),
///   text("down"),
/// });
/// ```
///
/// ### Output
///
/// ```bash
/// up
/// ────
/// down
/// ```
Element separatorLight() {
  return std::make_shared<SeparatorAuto>(LIGHT);
}

/// @brief Draw a vertical or horizontal separation in between two other
/// elements, using the DASHED style.
/// @ingroup dom
/// @see separator
/// @see separatorLight
/// @see separatorDashed
/// @see separatorDouble
/// @see separatorHeavy
/// @see separatorEmpty
/// @see separatorRounded
/// @see separatorStyled
/// @see separatorCharacter
///
/// Add a visual separation in between two elements.
///
/// ### Example
///
/// ```cpp
/// // Use 'border' as a function...
/// Element document = vbox({
///   text("up"),
///   separatorLight(),
///   text("down"),
/// });
/// ```
///
/// ### Output
///
/// ```bash
/// up
/// ╍╍╍╍
/// down
/// ```
Element separatorDashed() {
  return std::make_shared<SeparatorAuto>(DASHED);
}

/// @brief Draw a vertical or horizontal separation in between two other
/// elements, using the HEAVY style.
/// @ingroup dom
/// @see separator
/// @see separatorLight
/// @see separatorDashed
/// @see separatorDouble
/// @see separatorHeavy
/// @see separatorEmpty
/// @see separatorRounded
/// @see separatorStyled
/// @see separatorCharacter
///
/// Add a visual separation in between two elements.
///
/// ### Example
///
/// ```cpp
/// // Use 'border' as a function...
/// Element document = vbox({
///   text("up"),
///   separatorHeavy(),
///   text("down"),
/// });
/// ```
///
/// ### Output
///
/// ```bash
/// up
/// ━━━━
/// down
/// ```
Element separatorHeavy() {
  return std::make_shared<SeparatorAuto>(HEAVY);
}

/// @brief Draw a vertical or horizontal separation in between two other
/// elements, using the DOUBLE style.
/// @ingroup dom
/// @see separator
/// @see separatorLight
/// @see separatorDashed
/// @see separatorDouble
/// @see separatorHeavy
/// @see separatorEmpty
/// @see separatorRounded
/// @see separatorStyled
/// @see separatorCharacter
///
/// Add a visual separation in between two elements.
///
/// ### Example
///
/// ```cpp
/// // Use 'border' as a function...
/// Element document = vbox({
///   text("up"),
///   separatorDouble(),
///   text("down"),
/// });
/// ```
///
/// ### Output
///
/// ```bash
/// up
/// ════
/// down
/// ```
Element separatorDouble() {
  return std::make_shared<SeparatorAuto>(DOUBLE);
}

/// @brief Draw a vertical or horizontal separation in between two other
/// elements, using the EMPTY style.
/// @ingroup dom
/// @see separator
/// @see separatorLight
/// @see separatorDashed
/// @see separatorDouble
/// @see separatorHeavy
/// @see separatorEmpty
/// @see separatorRounded
/// @see separatorStyled
/// @see separatorCharacter
///
/// Add a visual separation in between two elements.
///
/// ### Example
///
/// ```cpp
/// // Use 'border' as a function...
/// Element document = vbox({
///   text("up"),
///   separator(),
///   text("down"),
/// });
/// ```
///
/// ### Output
///
/// ```bash
/// up
///
/// down
/// ```
Element separatorEmpty() {
  return std::make_shared<SeparatorAuto>(EMPTY);
}

/// @brief Draw a vertical or horizontal separation in between two other
/// elements.
/// @param value the character to fill the separator area.
/// @ingroup dom
/// @see separator
/// @see separatorLight
/// @see separatorDashed
/// @see separatorDouble
/// @see separatorHeavy
/// @see separatorEmpty
/// @see separatorRounded
/// @see separatorStyled
/// @see separatorCharacter
///
/// Add a visual separation in between two elements.
///
/// ### Example
///
/// ```cpp
/// // Use 'border' as a function...
/// Element document = vbox({
///   text("up"),
///   separator(),
///   text("down"),
/// });
/// ```
///
/// ### Output
///
/// ```bash
/// up
/// ────
/// down
/// ```
Element separatorCharacter(std::string value) {
  return std::make_shared<Separator>(std::move(value));
}

/// @brief Draw a separator in between two element filled with a given pixel.
/// @ingroup dom
/// @see separator
/// @see separatorLight
/// @see separatorDashed
/// @see separatorHeavy
/// @see separatorDouble
/// @see separatorStyled
///
/// ### Example
///
/// ```cpp
/// Pixel empty;
/// Element document = vbox({
///   text("Up"),
///   separator(empty),
///   text("Down"),
/// })
/// ```
///
/// ### Output
///
/// ```bash
/// Up
///
/// Down
/// ```
Element separator(Pixel pixel) {
  return std::make_shared<SeparatorWithPixel>(std::move(pixel));
}

/// @brief Draw an horizontal bar, with the area in between left/right colored
/// differently.
/// @param left the left limit of the active area.
/// @param right the right limit of the active area.
/// @param selected_color the color of the selected area.
/// @param unselected_color the color of the unselected area.
///
/// ### Example
///
/// ```cpp
/// Element document = separatorHSelector(2,5, Color::White, Color::Blue);
/// ```
Element separatorHSelector(float left,
                           float right,
                           Color unselected_color,
                           Color selected_color) {
  class Impl : public Node {
   public:
    Impl(float left, float right, Color selected_color, Color unselected_color)
        : left_(left),
          right_(right),
          unselected_color_(unselected_color),
          selected_color_(selected_color) {}
    void ComputeRequirement() override {
      requirement_.min_x = 1;
      requirement_.min_y = 1;
    }

    void Render(Screen& screen) override {
      if (box_.y_max < box_.y_min) {
        return;
      }

      // This are the two location with an empty demi-cell.
      int demi_cell_left = int(left_ * 2.F - 1.F);    // NOLINT
      int demi_cell_right = int(right_ * 2.F + 2.F);  // NOLINT

      const int y = box_.y_min;
      for (int x = box_.x_min; x <= box_.x_max; ++x) {
        Pixel& pixel = screen.PixelAt(x, y);

        const int a = (x - box_.x_min) * 2;
        const int b = a + 1;
        const bool a_empty = demi_cell_left == a || demi_cell_right == a;
        const bool b_empty = demi_cell_left == b || demi_cell_right == b;

        if (!a_empty && !b_empty) {
          pixel.character = "─";
          pixel.automerge = true;
        } else {
          pixel.character = a_empty ? "╶" : "╴";  // NOLINT
          pixel.automerge = false;
        }

        if (demi_cell_left <= a && b <= demi_cell_right) {
          pixel.foreground_color = selected_color_;
        } else {
          pixel.foreground_color = unselected_color_;
        }
      }
    }

    float left_;
    float right_;
    Color unselected_color_;
    Color selected_color_;
  };
  return std::make_shared<Impl>(left, right, unselected_color, selected_color);
}

/// @brief Draw an vertical bar, with the area in between up/downcolored
/// differently.
/// @param up the left limit of the active area.
/// @param down the right limit of the active area.
/// @param selected_color the color of the selected area.
/// @param unselected_color the color of the unselected area.
///
/// ### Example
///
/// ```cpp
/// Element document = separatorHSelector(2,5, Color::White, Color::Blue);
/// ```
Element separatorVSelector(float up,
                           float down,
                           Color unselected_color,
                           Color selected_color) {
  class Impl : public Node {
   public:
    Impl(float up, float down, Color unselected_color, Color selected_color)
        : up_(up),
          down_(down),
          unselected_color_(unselected_color),
          selected_color_(selected_color) {}
    void ComputeRequirement() override {
      requirement_.min_x = 1;
      requirement_.min_y = 1;
    }

    void Render(Screen& screen) override {
      if (box_.x_max < box_.x_min) {
        return;
      }

      // This are the two location with an empty demi-cell.
      const int demi_cell_up = int(up_ * 2 - 1);
      const int demi_cell_down = int(down_ * 2 + 2);

      const int x = box_.x_min;
      for (int y = box_.y_min; y <= box_.y_max; ++y) {
        Pixel& pixel = screen.PixelAt(x, y);

        const int a = (y - box_.y_min) * 2;
        const int b = a + 1;
        const bool a_empty = demi_cell_up == a || demi_cell_down == a;
        const bool b_empty = demi_cell_up == b || demi_cell_down == b;

        if (!a_empty && !b_empty) {
          pixel.character = "│";
          pixel.automerge = true;
        } else {
          pixel.character = a_empty ? "╷" : "╵";  // NOLINT
          pixel.automerge = false;
        }

        if (demi_cell_up <= a && b <= demi_cell_down) {
          pixel.foreground_color = selected_color_;
        } else {
          pixel.foreground_color = unselected_color_;
        }
      }
    }

    float up_;
    float down_;
    Color unselected_color_;
    Color selected_color_;
  };
  return std::make_shared<Impl>(up, down, unselected_color, selected_color);
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <algorithm>  // for min, max
#include <memory>     // for make_shared, __shared_ptr_access
#include <utility>    // for move
#include <vector>     // for __alloc_traits<>::value_type

namespace ftxui {

class Size : public Node {
 public:
  Size(Element child, Direction direction, Constraint constraint, int value)
      : Node(unpack(std::move(child))),
        direction_(direction),
        constraint_(constraint),
        value_(value) {}

  void ComputeRequirement() override {
    Node::ComputeRequirement();
    requirement_ = children_[0]->requirement();

    auto& value = direction_ == WIDTH ? requirement_.min_x : requirement_.min_y;

    switch (constraint_) {
      case LESS_THAN:
        value = std::min(value, value_);
        break;
      case EQUAL:
        value = value_;
        break;
      case GREATER_THAN:
        value = std::max(value, value_);
        break;
    }

    if (direction_ == WIDTH) {
      requirement_.flex_grow_x = 0;
      requirement_.flex_shrink_x = 0;
    } else {
      requirement_.flex_grow_y = 0;
      requirement_.flex_shrink_y = 0;
    }
  }

  void SetBox(Box box) override {
    Node::SetBox(box);

    if (direction_ == WIDTH) {
      switch (constraint_) {
        case LESS_THAN:
        case EQUAL:
          box.x_max = std::min(box.x_min + value_ + 1, box.x_max);
          break;
        case GREATER_THAN:
          break;
      }
    } else {
      switch (constraint_) {
        case LESS_THAN:
        case EQUAL:
          box.y_max = std::min(box.y_min + value_ + 1, box.y_max);
          break;
        case GREATER_THAN:
          break;
      }
    }
    children_[0]->SetBox(box);
  }

 private:
  Direction direction_;
  Constraint constraint_;
  int value_;
};

/// @brief Apply a constraint on the size of an element.
/// @param direction Whether the WIDTH of the HEIGHT of the element must be
///                  constrained.
/// @param constraint The type of constaint.
/// @param value The value.
/// @ingroup dom
Decorator size(Direction direction, Constraint constraint, int value) {
  return [=](Element e) {
    return std::make_shared<Size>(std::move(e), direction, constraint, value);
  };
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <cstddef>  // for size_t
#include <memory>   // for allocator, allocator_traits<>::value_type
#include <string>   // for basic_string, string
#include <utility>  // for move
#include <vector>   // for vector, __alloc_traits<>::value_type

namespace ftxui {

namespace {
// NOLINTNEXTLINE
const std::vector<std::vector<std::vector<std::string>>> elements = {
    {
        {"Replaced by the gauge"},
    },
    {
        {".  "},
        {".. "},
        {"..."},
    },
    {
        {"|"},
        {"/"},
        {"-"},
        {"\\"},
    },
    {
        {"+"},
        {"x"},
    },
    {
        {"|  "},
        {"|| "},
        {"|||"},
    },
    {
        {"←"},
        {"↖"},
        {"↑"},
        {"↗"},
        {"→"},
        {"↘"},
        {"↓"},
        {"↙"},
    },
    {
        {"▁"},
        {"▂"},
        {"▃"},
        {"▄"},
        {"▅"},
        {"▆"},
        {"▇"},
        {"█"},
        {"▇"},
        {"▆"},
        {"▅"},
        {"▄"},
        {"▃"},
        {"▁"},
    },
    {
        {"▉"},
        {"▊"},
        {"▋"},
        {"▌"},
        {"▍"},
        {"▎"},
        {"▏"},
        {"▎"},
        {"▍"},
        {"▌"},
        {"▋"},
        {"▊"},
    },
    {
        {"▖"},
        {"▘"},
        {"▝"},
        {"▗"},
    },
    {
        {"◢"},
        {"◣"},
        {"◤"},
        {"◥"},
    },
    {
        {"◰"},
        {"◳"},
        {"◲"},
        {"◱"},
    },
    {
        {"◴"},
        {"◷"},
        {"◶"},
        {"◵"},
    },
    {
        {"◐"},
        {"◓"},
        {"◑"},
        {"◒"},
    },
    {
        {"◡"},
        {"⊙"},
        {"◠"},
    },
    {
        {"⠁"},
        {"⠂"},
        {"⠄"},
        {"⡀"},
        {"⢀"},
        {"⠠"},
        {"⠐"},
        {"⠈"},
    },
    {
        {"⠋"},
        {"⠙"},
        {"⠹"},
        {"⠸"},
        {"⠼"},
        {"⠴"},
        {"⠦"},
        {"⠧"},
        {"⠇"},
        {"⠏"},
    },
    {
        {"(*----------)"}, {"(-*---------)"}, {"(--*--------)"},
        {"(---*-------)"}, {"(----*------)"}, {"(-----*-----)"},
        {"(------*----)"}, {"(-------*---)"}, {"(--------*--)"},
        {"(---------*-)"}, {"(----------*)"}, {"(---------*-)"},
        {"(--------*--)"}, {"(-------*---)"}, {"(------*----)"},
        {"(-----*-----)"}, {"(----*------)"}, {"(---*-------)"},
        {"(--*--------)"}, {"(-*---------)"},
    },
    {
        {"[      ]"},
        {"[=     ]"},
        {"[==    ]"},
        {"[===   ]"},
        {"[====  ]"},
        {"[===== ]"},
        {"[======]"},
        {"[===== ]"},
        {"[====  ]"},
        {"[===   ]"},
        {"[==    ]"},
        {"[=     ]"},
    },
    {
        {"[      ]"},
        {"[=     ]"},
        {"[==    ]"},
        {"[===   ]"},
        {"[====  ]"},
        {"[===== ]"},
        {"[======]"},
        {"[ =====]"},
        {"[  ====]"},
        {"[   ===]"},
        {"[    ==]"},
        {"[     =]"},
    },
    {
        {"[==    ]"},
        {"[==    ]"},
        {"[==    ]"},
        {"[==    ]"},
        {"[==    ]"},
        {" [==   ]"},
        {"[  ==  ]"},
        {"[   == ]"},
        {"[    ==]"},
        {"[    ==]"},
        {"[    ==]"},
        {"[    ==]"},
        {"[    ==]"},
        {"[   ==] "},
        {"[  ==  ]"},
        {"[ ==   ]"},
    },
    {
        {
            " ─╮",
            "  │",
            "   ",
        },
        {
            "  ╮",
            "  │",
            "  ╯",
        },
        {
            "   ",
            "  │",
            " ─╯",
        },
        {
            "   ",
            "   ",
            "╰─╯",
        },
        {
            "   ",
            "│  ",
            "╰─ ",
        },
        {
            "╭  ",
            "│  ",
            "╰  ",
        },
        {
            "╭─ ",
            "│  ",
            "   ",
        },
        {
            "╭─╮",
            "   ",
            "   ",
        },
    },
    {
        {
            "   /\\O ",
            "    /\\/",
            "   /\\  ",
            "  /  \\ ",
            "LOL  LOL",
        },
        {
            "    _O  ",
            "   //|_ ",
            "    |   ",
            "   /|   ",
            "   LLOL ",
        },
        {
            "     O  ",
            "    /_  ",
            "    |\\  ",
            "   / |  ",
            " LOLLOL ",
        },
    },
    {
        {"       ", "_______", "       "},
        {"       ", "______/", "       "},
        {"      _", "_____/ ", "       "},
        {"     _ ", "____/ \\", "       "},
        {"    _  ", "___/ \\ ", "      \\"},
        {"   _   ", "__/ \\  ", "     \\_"},
        {"  _    ", "_/ \\   ", "    \\_/"},
        {" _     ", "/ \\   _", "   \\_/ "},
        {"_      ", " \\   __", "  \\_/  "},
        {"       ", "\\   ___", " \\_/   "},
        {"       ", "    ___", "\\_/    "},
        {"       ", "  _____", "_/     "},
        {"       ", " ______", "/      "},
        {"       ", "_______", "       "},
    },
};

}  // namespace

/// @brief Useful to represent the effect of time and/or events. This display an
/// ASCII art "video".
/// @param charset_index The type of "video".
/// @param image_index The "frame" of the video. You need to increase this for
/// every "step".
/// @ingroup dom
Element spinner(int charset_index, size_t image_index) {
  if (charset_index == 0) {
    const int progress_size = 40;
    image_index %= progress_size;
    if (image_index > progress_size / 2) {
      image_index = progress_size - image_index;
    }
    return gauge(float(image_index) * 0.05F);  // NOLINT
  }
  charset_index %= (int)elements.size();
  image_index %= (int)elements[charset_index].size();
  std::vector<Element> lines;
  for (const auto& it : elements[charset_index][image_index]) {
    lines.push_back(text(it));
  }
  return vbox(std::move(lines));
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <memory>   // for make_shared
#include <utility>  // for move

namespace ftxui {

/// @brief Apply a strikethrough to text.
/// @ingroup dom
Element strikethrough(Element child) {
  class Impl : public NodeDecorator {
   public:
    using NodeDecorator::NodeDecorator;

    void Render(Screen& screen) override {
      for (int y = box_.y_min; y <= box_.y_max; ++y) {
        for (int x = box_.x_min; x <= box_.x_max; ++x) {
          screen.PixelAt(x, y).strikethrough = true;
        }
      }
      Node::Render(screen);
    }
  };

  return std::make_shared<Impl>(std::move(child));
}

}  // namespace ftxui

// Copyright 2023 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#ifndef FTXUI_DOM_TABLE
#define FTXUI_DOM_TABLE

#include <memory>
#include <string>  // for string
#include <vector>  // for vector

namespace ftxui {

// Usage:
//
// Initialization:
// ---------------
//
// auto table = Table({
//   {"X", "Y"},
//   {"-1", "1"},
//   {"+0", "0"},
//   {"+1", "1"},
// });
//
// table.SelectAll().Border(LIGHT);
//
// table.SelectRow(1).Border(DOUBLE);
// table.SelectRow(1).SeparatorInternal(Light);
//
// std::move(table).Element();

class Table;
class TableSelection;

class Table {
 public:
  Table();
  Table(std::vector<std::vector<std::string>>);
  Table(std::vector<std::vector<Element>>);
  TableSelection SelectAll();
  TableSelection SelectCell(int column, int row);
  TableSelection SelectRow(int row_index);
  TableSelection SelectRows(int row_min, int row_max);
  TableSelection SelectColumn(int column_index);
  TableSelection SelectColumns(int column_min, int column_max);
  TableSelection SelectRectangle(int column_min,
                                 int column_max,
                                 int row_min,
                                 int row_max);
  Element Render();

 private:
  void Initialize(std::vector<std::vector<Element>>);
  friend TableSelection;
  std::vector<std::vector<Element>> elements_;
  int input_dim_x_ = 0;
  int input_dim_y_ = 0;
  int dim_x_ = 0;
  int dim_y_ = 0;
};

class TableSelection {
 public:
  void Decorate(Decorator);
  void DecorateAlternateRow(Decorator, int modulo = 2, int shift = 0);
  void DecorateAlternateColumn(Decorator, int modulo = 2, int shift = 0);

  void DecorateCells(Decorator);
  void DecorateCellsAlternateColumn(Decorator, int modulo = 2, int shift = 0);
  void DecorateCellsAlternateRow(Decorator, int modulo = 2, int shift = 0);

  void Border(BorderStyle border = LIGHT);
  void BorderLeft(BorderStyle border = LIGHT);
  void BorderRight(BorderStyle border = LIGHT);
  void BorderTop(BorderStyle border = LIGHT);
  void BorderBottom(BorderStyle border = LIGHT);

  void Separator(BorderStyle border = LIGHT);
  void SeparatorVertical(BorderStyle border = LIGHT);
  void SeparatorHorizontal(BorderStyle border = LIGHT);

 private:
  friend Table;
  Table* table_;
  int x_min_;
  int x_max_;
  int y_min_;
  int y_max_;
};

}  // namespace ftxui

#endif /* end of include guard: FTXUI_DOM_TABLE */

// Copyright 2021 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

#include <algorithm>  // for max
#include <memory>   // for allocator, shared_ptr, allocator_traits<>::value_type
#include <utility>  // for move, swap

namespace ftxui {
namespace {

bool IsCell(int x, int y) {
  return x % 2 == 1 && y % 2 == 1;
}

// NOLINTNEXTLINE
static std::string charset[6][6] = {
    {"┌", "┐", "└", "┘", "─", "│"},  // LIGHT
    {"┏", "┓", "┗", "┛", "╍", "╏"},  // DASHED
    {"┏", "┓", "┗", "┛", "━", "┃"},  // HEAVY
    {"╔", "╗", "╚", "╝", "═", "║"},  // DOUBLE
    {"╭", "╮", "╰", "╯", "─", "│"},  // ROUNDED
    {" ", " ", " ", " ", " ", " "},  // EMPTY
};

int Wrap(int input, int modulo) {
  input %= modulo;
  input += modulo;
  input %= modulo;
  return input;
}

void Order(int& a, int& b) {
  if (a >= b) {
    std::swap(a, b);
  }
}

}  // namespace

Table::Table() {
  Initialize({});
}

Table::Table(std::vector<std::vector<std::string>> input) {
  std::vector<std::vector<Element>> output;
  for (auto& row : input) {
    output.emplace_back();
    auto& output_row = output.back();
    for (auto& cell : row) {
      output_row.push_back(text(std::move(cell)));
    }
  }
  Initialize(std::move(output));
}

Table::Table(std::vector<std::vector<Element>> input) {
  Initialize(std::move(input));
}

void Table::Initialize(std::vector<std::vector<Element>> input) {
  input_dim_y_ = (int)input.size();
  input_dim_x_ = 0;
  for (auto& row : input) {
    input_dim_x_ = std::max(input_dim_x_, (int)row.size());
  }

  dim_y_ = 2 * input_dim_y_ + 1;
  dim_x_ = 2 * input_dim_x_ + 1;

  // Reserve space.
  elements_.resize(dim_y_);
  for (int y = 0; y < dim_y_; ++y) {
    elements_[y].resize(dim_x_);
  }

  // Transfert elements_ from |input| toward |elements_|.
  {
    int y = 1;
    for (auto& row : input) {
      int x = 1;
      for (auto& cell : row) {
        elements_[y][x] = std::move(cell);
        x += 2;
      }
      y += 2;
    }
  }

  // Add empty element for the border.
  for (int y = 0; y < dim_y_; ++y) {
    for (int x = 0; x < dim_x_; ++x) {
      auto& element = elements_[y][x];

      if (IsCell(x, y)) {
        if (!element) {
          element = emptyElement();
        }
        continue;
      }

      element = emptyElement();
    }
  }
}

TableSelection Table::SelectRow(int index) {
  return SelectRectangle(0, -1, index, index);
}

TableSelection Table::SelectRows(int row_min, int row_max) {
  return SelectRectangle(0, -1, row_min, row_max);
}

TableSelection Table::SelectColumn(int index) {
  return SelectRectangle(index, index, 0, -1);
}

TableSelection Table::SelectColumns(int column_min, int column_max) {
  return SelectRectangle(column_min, column_max, 0, -1);
}

TableSelection Table::SelectCell(int column, int row) {
  return SelectRectangle(column, column, row, row);
}

TableSelection Table::SelectRectangle(int column_min,
                                      int column_max,
                                      int row_min,
                                      int row_max) {
  column_min = Wrap(column_min, input_dim_x_);
  column_max = Wrap(column_max, input_dim_x_);
  Order(column_min, column_max);
  row_min = Wrap(row_min, input_dim_y_);
  row_max = Wrap(row_max, input_dim_y_);
  Order(row_min, row_max);

  TableSelection output;  // NOLINT
  output.table_ = this;
  output.x_min_ = 2 * column_min;
  output.x_max_ = 2 * column_max + 2;
  output.y_min_ = 2 * row_min;
  output.y_max_ = 2 * row_max + 2;
  return output;
}

TableSelection Table::SelectAll() {
  TableSelection output;  // NOLINT
  output.table_ = this;
  output.x_min_ = 0;
  output.x_max_ = dim_x_ - 1;
  output.y_min_ = 0;
  output.y_max_ = dim_y_ - 1;
  return output;
}

Element Table::Render() {
  for (int y = 0; y < dim_y_; ++y) {
    for (int x = 0; x < dim_x_; ++x) {
      auto& it = elements_[y][x];

      // Line
      if ((x + y) % 2 == 1) {
        it = std::move(it) | flex;
        continue;
      }

      // Cells
      if ((x % 2) == 1 && (y % 2) == 1) {
        it = std::move(it) | flex_shrink;
        continue;
      }

      // Corners
      it = std::move(it) | size(WIDTH, EQUAL, 0) | size(HEIGHT, EQUAL, 0);
    }
  }
  dim_x_ = 0;
  dim_y_ = 0;
  return gridbox(std::move(elements_));
}

// NOLINTNEXTLINE
void TableSelection::Decorate(Decorator decorator) {
  for (int y = y_min_; y <= y_max_; ++y) {
    for (int x = x_min_; x <= x_max_; ++x) {
      Element& e = table_->elements_[y][x];
      e = std::move(e) | decorator;
    }
  }
}

// NOLINTNEXTLINE
void TableSelection::DecorateCells(Decorator decorator) {
  for (int y = y_min_; y <= y_max_; ++y) {
    for (int x = x_min_; x <= x_max_; ++x) {
      if (y % 2 == 1 && x % 2 == 1) {
        Element& e = table_->elements_[y][x];
        e = std::move(e) | decorator;
      }
    }
  }
}

// NOLINTNEXTLINE
void TableSelection::DecorateAlternateColumn(Decorator decorator,
                                             int modulo,
                                             int shift) {
  for (int y = y_min_; y <= y_max_; ++y) {
    for (int x = x_min_; x <= x_max_; ++x) {
      if (y % 2 == 1 && (x / 2) % modulo == shift) {
        Element& e = table_->elements_[y][x];
        e = std::move(e) | decorator;
      }
    }
  }
}

// NOLINTNEXTLINE
void TableSelection::DecorateAlternateRow(Decorator decorator,
                                          int modulo,
                                          int shift) {
  for (int y = y_min_ + 1; y <= y_max_ - 1; ++y) {
    for (int x = x_min_; x <= x_max_; ++x) {
      if (y % 2 == 1 && (y / 2) % modulo == shift) {
        Element& e = table_->elements_[y][x];
        e = std::move(e) | decorator;
      }
    }
  }
}

// NOLINTNEXTLINE
void TableSelection::DecorateCellsAlternateColumn(Decorator decorator,
                                                  int modulo,
                                                  int shift) {
  for (int y = y_min_; y <= y_max_; ++y) {
    for (int x = x_min_; x <= x_max_; ++x) {
      if (y % 2 == 1 && x % 2 == 1 && ((x / 2) % modulo == shift)) {
        Element& e = table_->elements_[y][x];
        e = std::move(e) | decorator;
      }
    }
  }
}

// NOLINTNEXTLINE
void TableSelection::DecorateCellsAlternateRow(Decorator decorator,
                                               int modulo,
                                               int shift) {
  for (int y = y_min_; y <= y_max_; ++y) {
    for (int x = x_min_; x <= x_max_; ++x) {
      if (y % 2 == 1 && x % 2 == 1 && ((y / 2) % modulo == shift)) {
        Element& e = table_->elements_[y][x];
        e = std::move(e) | decorator;
      }
    }
  }
}

void TableSelection::Border(BorderStyle border) {
  BorderLeft(border);
  BorderRight(border);
  BorderTop(border);
  BorderBottom(border);

  // NOLINTNEXTLINE
  table_->elements_[y_min_][x_min_] = text(charset[border][0]) | automerge;
  // NOLINTNEXTLINE
  table_->elements_[y_min_][x_max_] = text(charset[border][1]) | automerge;
  // NOLINTNEXTLINE
  table_->elements_[y_max_][x_min_] = text(charset[border][2]) | automerge;
  // NOLINTNEXTLINE
  table_->elements_[y_max_][x_max_] = text(charset[border][3]) | automerge;
}

void TableSelection::Separator(BorderStyle border) {
  for (int y = y_min_ + 1; y <= y_max_ - 1; ++y) {
    for (int x = x_min_ + 1; x <= x_max_ - 1; ++x) {
      if (y % 2 == 0 || x % 2 == 0) {
        Element& e = table_->elements_[y][x];
        e = (y % 2 == 1)
                ? separatorCharacter(charset[border][5]) | automerge   // NOLINT
                : separatorCharacter(charset[border][4]) | automerge;  // NOLINT
      }
    }
  }
}

void TableSelection::SeparatorVertical(BorderStyle border) {
  for (int y = y_min_ + 1; y <= y_max_ - 1; ++y) {
    for (int x = x_min_ + 1; x <= x_max_ - 1; ++x) {
      if (x % 2 == 0) {
        table_->elements_[y][x] =
            separatorCharacter(charset[border][5]) | automerge;  // NOLINT
      }
    }
  }
}

void TableSelection::SeparatorHorizontal(BorderStyle border) {
  for (int y = y_min_ + 1; y <= y_max_ - 1; ++y) {
    for (int x = x_min_ + 1; x <= x_max_ - 1; ++x) {
      if (y % 2 == 0) {
        table_->elements_[y][x] =
            separatorCharacter(charset[border][4]) | automerge;  // NOLINT
      }
    }
  }
}

void TableSelection::BorderLeft(BorderStyle border) {
  for (int y = y_min_; y <= y_max_; y++) {
    table_->elements_[y][x_min_] =
        separatorCharacter(charset[border][5]) | automerge;  // NOLINT
  }
}

void TableSelection::BorderRight(BorderStyle border) {
  for (int y = y_min_; y <= y_max_; y++) {
    table_->elements_[y][x_max_] =
        separatorCharacter(charset[border][5]) | automerge;  // NOLINT
  }
}

void TableSelection::BorderTop(BorderStyle border) {
  for (int x = x_min_; x <= x_max_; x++) {
    table_->elements_[y_min_][x] =
        separatorCharacter(charset[border][4]) | automerge;  // NOLINT
  }
}

void TableSelection::BorderBottom(BorderStyle border) {
  for (int x = x_min_; x <= x_max_; x++) {
    table_->elements_[y_max_][x] =
        separatorCharacter(charset[border][4]) | automerge;  // NOLINT
  }
}

}  // namespace ftxui

// Copyright 2021 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <algorithm>  // for min
#include <memory>     // for make_shared
#include <string>     // for string, wstring
#include <utility>    // for move
#include <vector>     // for vector

namespace ftxui {

using ftxui::Screen;

class Text : public Node {
 public:
  explicit Text(std::string text) : text_(std::move(text)) {}

  void ComputeRequirement() override {
    requirement_.min_x = string_width(text_);
    requirement_.min_y = 1;
  }

  void Render(Screen& screen) override {
    int x = box_.x_min;
    const int y = box_.y_min;
    if (y > box_.y_max) {
      return;
    }
    for (const auto& cell : Utf8ToGlyphs(text_)) {
      if (x > box_.x_max) {
        return;
      }
      screen.PixelAt(x, y).character = cell;
      ++x;
    }
  }

 private:
  std::string text_;
};

class VText : public Node {
 public:
  explicit VText(std::string text)
      : text_(std::move(text)), width_{std::min(string_width(text_), 1)} {}

  void ComputeRequirement() override {
    requirement_.min_x = width_;
    requirement_.min_y = string_width(text_);
  }

  void Render(Screen& screen) override {
    const int x = box_.x_min;
    int y = box_.y_min;
    if (x + width_ - 1 > box_.x_max) {
      return;
    }
    for (const auto& it : Utf8ToGlyphs(text_)) {
      if (y > box_.y_max) {
        return;
      }
      screen.PixelAt(x, y).character = it;
      y += 1;
    }
  }

 private:
  std::string text_;
  int width_ = 1;
};

/// @brief Display a piece of UTF8 encoded unicode text.
/// @ingroup dom
/// @see ftxui::to_wstring
///
/// ### Example
///
/// ```cpp
/// Element document = text("Hello world!");
/// ```
///
/// ### Output
///
/// ```bash
/// Hello world!
/// ```
Element text(std::string text) {
  return std::make_shared<Text>(std::move(text));
}

/// @brief Display a piece of unicode text.
/// @ingroup dom
/// @see ftxui::to_wstring
///
/// ### Example
///
/// ```cpp
/// Element document = text(L"Hello world!");
/// ```
///
/// ### Output
///
/// ```bash
/// Hello world!
/// ```
Element text(std::wstring text) {  // NOLINT
  return std::make_shared<Text>(to_string(text));
}

/// @brief Display a piece of unicode text vertically.
/// @ingroup dom
/// @see ftxui::to_wstring
///
/// ### Example
///
/// ```cpp
/// Element document = vtext("Hello world!");
/// ```
///
/// ### Output
///
/// ```bash
/// H
/// e
/// l
/// l
/// o
///
/// w
/// o
/// r
/// l
/// d
/// !
/// ```
Element vtext(std::string text) {
  return std::make_shared<VText>(std::move(text));
}

/// @brief Display a piece unicode text vertically.
/// @ingroup dom
/// @see ftxui::to_wstring
///
/// ### Example
///
/// ```cpp
/// Element document = vtext(L"Hello world!");
/// ```
///
/// ### Output
///
/// ```bash
/// H
/// e
/// l
/// l
/// o
///
/// w
/// o
/// r
/// l
/// d
/// !
/// ```
Element vtext(std::wstring text) {  // NOLINT
  return std::make_shared<VText>(to_string(text));
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <memory>   // for make_shared
#include <utility>  // for move

namespace ftxui {

class Underlined : public NodeDecorator {
 public:
  using NodeDecorator::NodeDecorator;

  void Render(Screen& screen) override {
    Node::Render(screen);
    for (int y = box_.y_min; y <= box_.y_max; ++y) {
      for (int x = box_.x_min; x <= box_.x_max; ++x) {
        screen.PixelAt(x, y).underlined = true;
      }
    }
  }
};

/// @brief Make the underlined element to be underlined.
/// @ingroup dom
Element underlined(Element child) {
  return std::make_shared<Underlined>(std::move(child));
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <memory>   // for make_shared
#include <utility>  // for move

namespace ftxui {

/// @brief Apply a underlinedDouble to text.
/// @ingroup dom
Element underlinedDouble(Element child) {
  class Impl : public NodeDecorator {
   public:
    using NodeDecorator::NodeDecorator;

    void Render(Screen& screen) override {
      for (int y = box_.y_min; y <= box_.y_max; ++y) {
        for (int x = box_.x_min; x <= box_.x_max; ++x) {
          screen.PixelAt(x, y).underlined_double = true;
        }
      }
      Node::Render(screen);
    }
  };

  return std::make_shared<Impl>(std::move(child));
}

}  // namespace ftxui

// Copyright 2023 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <algorithm>    // for min
#include <functional>   // for function
#include <memory>       // for __shared_ptr_access, make_unique
#include <type_traits>  // for remove_reference, remove_reference<>::type
#include <utility>      // for move
#include <vector>       // for vector

namespace ftxui {

namespace {
Decorator compose(Decorator a, Decorator b) {
  return [a = std::move(a), b = std::move(b)](Element element) {
    return b(a(std::move(element)));
  };
}
}  // namespace

/// @brief A decoration doing absolutely nothing.
/// @ingroup dom
Element nothing(Element element) {
  return element;
}

/// @brief Compose two decorator into one.
/// @ingroup dom
///
/// ### Example
///
/// ```cpp
/// auto decorator = bold | blink;
/// ```
Decorator operator|(Decorator a, Decorator b) {
  return compose(std::move(a),  //
                 std::move(b));
}

/// @brief From a set of element, apply a decorator to every elements.
/// @return the set of decorated element.
/// @ingroup dom
Elements operator|(Elements elements, Decorator decorator) {  // NOLINT
  Elements output;
  for (auto& it : elements) {
    output.push_back(std::move(it) | decorator);
  }
  return output;
}

/// @brief From an element, apply a decorator.
/// @return the decorated element.
/// @ingroup dom
///
/// ### Example
///
/// Both of these are equivalent:
/// ```cpp
/// bold(text("Hello"));
/// ```
/// ```cpp
/// text("Hello") | bold;
/// ```
Element operator|(Element element, Decorator decorator) {  // NOLINT
  return decorator(std::move(element));
}

/// @brief Apply a decorator to an element.
/// @return the decorated element.
/// @ingroup dom
///
/// ### Example
///
/// Both of these are equivalent:
/// ```cpp
/// auto element = text("Hello");
/// element |= bold;
/// ```
Element& operator|=(Element& e, Decorator d) {
  e = e | std::move(d);
  return e;
}

/// The minimal dimension that will fit the given element.
/// @see Fixed
/// @see Full
Dimensions Dimension::Fit(Element& e) {
  const Dimensions fullsize = Dimension::Full();
  Box box;
  box.x_min = 0;
  box.y_min = 0;
  box.x_max = fullsize.dimx;
  box.y_max = fullsize.dimy;

  Node::Status status;
  e->Check(&status);
  const int max_iteration = 20;
  while (status.need_iteration && status.iteration < max_iteration) {
    e->ComputeRequirement();

    // Don't give the element more space than it needs:
    box.x_max = std::min(box.x_max, e->requirement().min_x);
    box.y_max = std::min(box.y_max, e->requirement().min_y);

    e->SetBox(box);
    status.need_iteration = false;
    status.iteration++;
    e->Check(&status);

    if (!status.need_iteration) {
      break;
    }
    // Increase the size of the box until it fits, but not more than the with of
    // the terminal emulator:
    box.x_max = std::min(e->requirement().min_x, fullsize.dimx);
    box.y_max = std::min(e->requirement().min_y, fullsize.dimy);
  }

  return {
      box.x_max,
      box.y_max,
  };
}

/// An element of size 0x0 drawing nothing.
/// @ingroup dom
Element emptyElement() {
  class Impl : public Node {
    void ComputeRequirement() override {
      requirement_.min_x = 0;
      requirement_.min_y = 0;
    }
  };
  return ftxui::make_unique<Impl>();
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <algorithm>  // for max
#include <cstddef>    // for size_t
#include <memory>  // for __shared_ptr_access, shared_ptr, make_shared, allocator_traits<>::value_type
#include <utility>  // for move
#include <vector>   // for vector, __alloc_traits<>::value_type

namespace ftxui {

class VBox : public Node {
 public:
  explicit VBox(Elements children) : Node(std::move(children)) {}

  void ComputeRequirement() override {
    requirement_.min_x = 0;
    requirement_.min_y = 0;
    requirement_.flex_grow_x = 0;
    requirement_.flex_grow_y = 0;
    requirement_.flex_shrink_x = 0;
    requirement_.flex_shrink_y = 0;
    requirement_.selection = Requirement::NORMAL;
    for (auto& child : children_) {
      child->ComputeRequirement();
      if (requirement_.selection < child->requirement().selection) {
        requirement_.selection = child->requirement().selection;
        requirement_.selected_box = child->requirement().selected_box;
        requirement_.selected_box.y_min += requirement_.min_y;
        requirement_.selected_box.y_max += requirement_.min_y;
      }
      requirement_.min_y += child->requirement().min_y;
      requirement_.min_x =
          std::max(requirement_.min_x, child->requirement().min_x);
    }
  }

  void SetBox(Box box) override {
    Node::SetBox(box);

    std::vector<box_helper::Element> elements(children_.size());
    for (size_t i = 0; i < children_.size(); ++i) {
      auto& element = elements[i];
      const auto& requirement = children_[i]->requirement();
      element.min_size = requirement.min_y;
      element.flex_grow = requirement.flex_grow_y;
      element.flex_shrink = requirement.flex_shrink_y;
    }
    const int target_size = box.y_max - box.y_min + 1;
    box_helper::Compute(&elements, target_size);

    int y = box.y_min;
    for (size_t i = 0; i < children_.size(); ++i) {
      box.y_min = y;
      box.y_max = y + elements[i].size - 1;
      children_[i]->SetBox(box);
      y = box.y_max + 1;
    }
  }
};

/// @brief A container displaying elements vertically one by one.
/// @param children The elements in the container
/// @return The container.
/// @ingroup dom
///
/// #### Example
///
/// ```cpp
/// vbox({
///   text("Up"),
///   text("Down"),
/// });
/// ```
Element vbox(Elements children) {
  return std::make_shared<VBox>(std::move(children));
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <cmath>    // for sin, pow, sqrt, cos
#include <ratio>    // for ratio
#include <utility>  // for move

#ifndef FTXUI_ANIMATION_HPP
#define FTXUI_ANIMATION_HPP

#include <chrono>      // for milliseconds, duration, steady_clock, time_point
#include <functional>  // for function

#ifndef FTXUI_COMPONENT_EVENT_HPP
#define FTXUI_COMPONENT_EVENT_HPP

#ifndef FTXUI_COMPONENT_MOUSE_HPP
#define FTXUI_COMPONENT_MOUSE_HPP
namespace ftxui {

/// @brief A mouse event. It contains the coordinate of the mouse, the button
/// pressed and the modifier (shift, ctrl, meta).
/// @ingroup component
struct Mouse {
  enum Button {
    Left = 0,
    Middle = 1,
    Right = 2,
    None = 3,
    WheelUp = 4,
    WheelDown = 5,
  };

  enum Motion {
    Released = 0,
    Pressed = 1,
  };

  // Button
  Button button;

  // Motion
  Motion motion;

  // Modifiers:
  bool shift;
  bool meta;
  bool control;

  // Coordinates:
  int x;
  int y;
};

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#endif /* end of include guard: FTXUI_COMPONENT_MOUSE_HPP */
#include <functional>
#include <string>  // for string, operator==
#include <vector>

namespace ftxui {

class ScreenInteractive;
class ComponentBase;

/// @brief Represent an event. It can be key press event, a terminal resize, or
/// more ...
///
/// For example:
/// - Printable character can be created using Event::Character('a').
/// - Some special are predefined, like Event::ArrowLeft.
/// - One can find arbitrary code for special Events using:
///   ./example/util/print_key_press
///  For instance, CTLR+A maps to Event::Special({1});
///
/// Useful documentation about xterm specification:
/// https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
struct Event {
  // --- Constructor section ---------------------------------------------------
  static Event Character(std::string);
  static Event Character(char);
  static Event Character(wchar_t);
  static Event Special(std::string);
  static Event Mouse(std::string, Mouse mouse);
  static Event CursorReporting(std::string, int x, int y);

  // --- Arrow ---
  static const Event ArrowLeft;
  static const Event ArrowRight;
  static const Event ArrowUp;
  static const Event ArrowDown;

  static const Event ArrowLeftCtrl;
  static const Event ArrowRightCtrl;
  static const Event ArrowUpCtrl;
  static const Event ArrowDownCtrl;

  // --- Other ---
  static const Event Backspace;
  static const Event Delete;
  static const Event Return;
  static const Event Escape;
  static const Event Tab;
  static const Event TabReverse;
  static const Event F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12;

  static const Event Home;
  static const Event End;

  static const Event PageUp;
  static const Event PageDown;

  // --- Custom ---
  static const Event Custom;

  //--- Method section ---------------------------------------------------------
  bool is_character() const { return type_ == Type::Character; }
  std::string character() const { return input_; }

  bool is_mouse() const { return type_ == Type::Mouse; }
  struct Mouse& mouse() { return mouse_; }

  bool is_cursor_reporting() const { return type_ == Type::CursorReporting; }
  int cursor_x() const { return cursor_.x; }
  int cursor_y() const { return cursor_.y; }

  const std::string& input() const { return input_; }

  bool operator==(const Event& other) const { return input_ == other.input_; }
  bool operator!=(const Event& other) const { return !operator==(other); }

  //--- State section ----------------------------------------------------------
  ScreenInteractive* screen_ = nullptr;

 private:
  friend ComponentBase;
  friend ScreenInteractive;
  enum class Type {
    Unknown,
    Character,
    Mouse,
    CursorReporting,
  };
  Type type_ = Type::Unknown;

  struct Cursor {
    int x;
    int y;
  };

  union {
    struct Mouse mouse_;
    struct Cursor cursor_;
  };
  std::string input_;
};

}  // namespace ftxui

#endif /* end of include guard: FTXUI_COMPONENT_EVENT_HPP */

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

namespace ftxui {

namespace animation {
// Components who haven't completed their animation can call this function to
// request a new frame to be drawn later.
//
// When there is no new events and no animations to complete, no new frame is
// drawn.
void RequestAnimationFrame();

using Clock = std::chrono::steady_clock;
using TimePoint = std::chrono::time_point<Clock>;
using Duration = std::chrono::duration<double>;

// Parameter of Component::OnAnimation(param).
class Params {
 public:
  Params(Duration duration) : duration_(duration) {}

  /// The duration this animation step represents.
  Duration duration() const { return duration_; }

 private:
  Duration duration_;
};

namespace easing {
using Function = std::function<float(float)>;
// Linear interpolation (no easing)
float Linear(float p);

// Quadratic easing; p^2
float QuadraticIn(float p);
float QuadraticOut(float p);
float QuadraticInOut(float p);

// Cubic easing; p^3
float CubicIn(float p);
float CubicOut(float p);
float CubicInOut(float p);

// Quartic easing; p^4
float QuarticIn(float p);
float QuarticOut(float p);
float QuarticInOut(float p);

// Quintic easing; p^5
float QuinticIn(float p);
float QuinticOut(float p);
float QuinticInOut(float p);

// Sine wave easing; sin(p * PI/2)
float SineIn(float p);
float SineOut(float p);
float SineInOut(float p);

// Circular easing; sqrt(1 - p^2)
float CircularIn(float p);
float CircularOut(float p);
float CircularInOut(float p);

// Exponential easing, base 2
float ExponentialIn(float p);
float ExponentialOut(float p);
float ExponentialInOut(float p);

// Exponentially-damped sine wave easing
float ElasticIn(float p);
float ElasticOut(float p);
float ElasticInOut(float p);

// Overshooting cubic easing;
float BackIn(float p);
float BackOut(float p);
float BackInOut(float p);

// Exponentially-decaying bounce easing
float BounceIn(float p);
float BounceOut(float p);
float BounceInOut(float p);
}  // namespace easing

class Animator {
 public:
  Animator(float* from,
           float to = 0.f,
           Duration duration = std::chrono::milliseconds(250),
           easing::Function easing_function = easing::Linear,
           Duration delay = std::chrono::milliseconds(0));

  void OnAnimation(Params&);

  float to() const { return to_; }

 private:
  float* value_;
  float from_;
  float to_;
  Duration duration_;
  easing::Function easing_function_;
  Duration current_;
};

}  // namespace animation
}  // namespace ftxui

#endif /* end of include guard: FTXUI_ANIMATION_HPP */

// Copyright 2022 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

namespace ftxui::animation {

namespace easing {

namespace {
constexpr float kPi = 3.14159265358979323846F;
constexpr float kPi2 = kPi / 2.F;
}  // namespace

// Easing function have been taken out of:
// https://github.com/warrenm/AHEasing/blob/master/AHEasing/easing.c
//
// Corresponding license:
//  Copyright (c) 2011, Auerhaus Development, LLC
//
//  This program is free software. It comes without any warranty, to
//  the extent permitted by applicable law. You can redistribute it
//  and/or modify it under the terms of the Do What The Fuck You Want
//  To Public License, Version 2, as published by Sam Hocevar. See
//  http://sam.zoy.org/wtfpl/COPYING for more details.

// Modeled after the line y = x
float Linear(float p) {
  return p;
}

// Modeled after the parabola y = x^2
float QuadraticIn(float p) {
  return p * p;
}

// Modeled after the parabola y = -x^2 + 2x
float QuadraticOut(float p) {
  return -(p * (p - 2));
}

// Modeled after the piecewise quadratic
// y = (1/2)((2x)^2)             ; [0, 0.5)
// y = -(1/2)((2x-1)*(2x-3) - 1) ; [0.5, 1]
float QuadraticInOut(float p) {
  if (p < 0.5F) {  // NOLINT
    return 2 * p * p;
  } else {
    return (-2 * p * p) + (4 * p) - 1;
  }
}

// Modeled after the cubic y = x^3
float CubicIn(float p) {
  return p * p * p;
}

// Modeled after the cubic y = (x - 1)^3 + 1
float CubicOut(float p) {
  const float f = (p - 1);
  return f * f * f + 1;
}

// Modeled after the piecewise cubic
// y = (1/2)((2x)^3)       ; [0, 0.5)
// y = (1/2)((2x-2)^3 + 2) ; [0.5, 1]
float CubicInOut(float p) {
  if (p < 0.5F) {  // NOLINT
    return 4 * p * p * p;
  } else {
    const float f = ((2 * p) - 2);
    return 0.5F * f * f * f + 1;  // NOLINT
  }
}

// Modeled after the quartic x^4
float QuarticIn(float p) {
  return p * p * p * p;
}

// Modeled after the quartic y = 1 - (x - 1)^4
float QuarticOut(float p) {
  const float f = (p - 1);
  return f * f * f * (1 - p) + 1;
}

// Modeled after the piecewise quartic
// y = (1/2)((2x)^4)        ; [0, 0.5)
// y = -(1/2)((2x-2)^4 - 2) ; [0.5, 1]
float QuarticInOut(float p) {
  if (p < 0.5F) {              // NOLINT
    return 8 * p * p * p * p;  // NOLINT
  } else {
    const float f = (p - 1);
    return -8 * f * f * f * f + 1;  // NOLINT
  }
}

// Modeled after the quintic y = x^5
float QuinticIn(float p) {
  return p * p * p * p * p;
}

// Modeled after the quintic y = (x - 1)^5 + 1
float QuinticOut(float p) {
  const float f = (p - 1);
  return f * f * f * f * f + 1;
}

// Modeled after the piecewise quintic
// y = (1/2)((2x)^5)       ; [0, 0.5)
// y = (1/2)((2x-2)^5 + 2) ; [0.5, 1]
float QuinticInOut(float p) {
  if (p < 0.5F) {                        // NOLINT
    return 16 * p * p * p * p * p;       // NOLINT
  } else {                               // NOLINT
    float f = ((2 * p) - 2);             // NOLINT
    return 0.5 * f * f * f * f * f + 1;  // NOLINT
  }
}

// Modeled after quarter-cycle of sine wave
float SineIn(float p) {
  return std::sin((p - 1) * kPi2) + 1;
}

// Modeled after quarter-cycle of sine wave (different phase)
float SineOut(float p) {
  return std::sin(p * kPi2);
}

// Modeled after half sine wave
float SineInOut(float p) {
  return 0.5F * (1 - std::cos(p * kPi));  // NOLINT
}

// Modeled after shifted quadrant IV of unit circle
float CircularIn(float p) {
  return 1 - std::sqrt(1 - (p * p));
}

// Modeled after shifted quadrant II of unit circle
float CircularOut(float p) {
  return std::sqrt((2 - p) * p);
}

// Modeled after the piecewise circular function
// y = (1/2)(1 - sqrt(1 - 4x^2))           ; [0, 0.5)
// y = (1/2)(sqrt(-(2x - 3)*(2x - 1)) + 1) ; [0.5, 1]
float CircularInOut(float p) {
  if (p < 0.5F) {                                    // NOLINT
    return 0.5F * (1 - std::sqrt(1 - 4 * (p * p)));  // NOLINT
  } else {
    return 0.5F * (std::sqrt(-((2 * p) - 3) * ((2 * p) - 1)) + 1);  // NOLINT
  }
}

// Modeled after the exponential function y = 2^(10(x - 1))
float ExponentialIn(float p) {
  return (p == 0.0) ? p : std::pow(2, 10 * (p - 1));  // NOLINT
}

// Modeled after the exponential function y = -2^(-10x) + 1
float ExponentialOut(float p) {
  return (p == 1.0) ? p : 1 - std::pow(2, -10 * p);  // NOLINT
}

// Modeled after the piecewise exponential
// y = (1/2)2^(10(2x - 1))         ; [0,0.5)
// y = -(1/2)*2^(-10(2x - 1))) + 1 ; [0.5,1]
float ExponentialInOut(float p) {
  if (p == 0.0 || p == 1.F) {
    return p;
  }

  if (p < 0.5F) {                                   // NOLINT
    return 0.5 * std::pow(2, (20 * p) - 10);        // NOLINT
  } else {                                          // NOLINT
    return -0.5 * std::pow(2, (-20 * p) + 10) + 1;  // NOLINT
  }
}

// Modeled after the damped sine wave y = sin(13pi/2*x)*pow(2, 10 * (x - 1))
float ElasticIn(float p) {
  return std::sin(13.F * kPi2 * p) * std::pow(2.F, 10.F * (p - 1));  // NOLINT
}

// Modeled after the damped sine wave y = sin(-13pi/2*(x + 1))*pow(2, -10x) +
// 1
float ElasticOut(float p) {
  // NOLINTNEXTLINE
  return std::sin(-13.F * kPi2 * (p + 1)) * std::pow(2.F, -10.F * p) + 1;
}

// Modeled after the piecewise exponentially-damped sine wave:
// y = (1/2)*sin(13pi/2*(2*x))*pow(2, 10 * ((2*x) - 1))      ; [0,0.5)
// y = (1/2)*(sin(-13pi/2*((2x-1)+1))*pow(2,-10(2*x-1)) + 2) ; [0.5, 1]
float ElasticInOut(float p) {
  if (p < 0.5F) {                                               // NOLINT
    return 0.5 * std::sin(13.F * kPi2 * (2 * p)) *              // NOLINT
           std::pow(2, 10 * ((2 * p) - 1));                     // NOLINT
  } else {                                                      // NOLINT
    return 0.5 * (std::sin(-13.F * kPi2 * ((2 * p - 1) + 1)) *  // NOLINT
                      std::pow(2, -10 * (2 * p - 1)) +          // NOLINT
                  2);                                           // NOLINT
  }
}

// Modeled after the overshooting cubic y = x^3-x*sin(x*pi)
float BackIn(float p) {
  return p * p * p - p * std::sin(p * kPi);
}

// Modeled after overshooting cubic y = 1-((1-x)^3-(1-x)*sin((1-x)*pi))
float BackOut(float p) {
  const float f = (1 - p);
  return 1 - (f * f * f - f * std::sin(f * kPi));
}

// Modeled after the piecewise overshooting cubic function:
// y = (1/2)*((2x)^3-(2x)*sin(2*x*pi))           ; [0, 0.5)
// y = (1/2)*(1-((1-x)^3-(1-x)*sin((1-x)*pi))+1) ; [0.5, 1]
float BackInOut(float p) {
  if (p < 0.5F) {  // NOLINT
    const float f = 2 * p;
    return 0.5F * (f * f * f - f * std::sin(f * kPi));  // NOLINT
  } else {
    float f = (1 - (2 * p - 1));                                    // NOLINT
    return 0.5F * (1 - (f * f * f - f * std::sin(f * kPi))) + 0.5;  // NOLINT
  }
}

float BounceIn(float p) {
  return 1 - BounceOut(1 - p);
}

float BounceOut(float p) {
  if (p < 4 / 11.0) {                                           // NOLINT
    return (121 * p * p) / 16.0;                                // NOLINT
  } else if (p < 8 / 11.0) {                                    // NOLINT
    return (363 / 40.0 * p * p) - (99 / 10.0 * p) + 17 / 5.0;   // NOLINT
  } else if (p < 9 / 10.0) {                                    // NOLINT
    return (4356 / 361.0 * p * p) - (35442 / 1805.0 * p) +      // NOLINT
           16061 / 1805.0;                                      // NOLINT
  } else {                                                      // NOLINT
    return (54 / 5.0 * p * p) - (513 / 25.0 * p) + 268 / 25.0;  // NOLINT
  }
}

float BounceInOut(float p) {                    // NOLINT
  if (p < 0.5F) {                               // NOLINT
    return 0.5F * BounceIn(p * 2);              // NOLINT
  } else {                                      // NOLINT
    return 0.5F * BounceOut(p * 2 - 1) + 0.5F;  // NOLINT
  }
}

}  // namespace easing

Animator::Animator(float* from,
                   float to,
                   Duration duration,
                   easing::Function easing_function,
                   Duration delay)
    : value_(from),
      from_(*from),
      to_(to),
      duration_(duration),
      easing_function_(std::move(easing_function)),
      current_(-delay) {
  RequestAnimationFrame();
}

void Animator::OnAnimation(Params& params) {
  current_ += params.duration();

  if (current_ >= duration_) {
    *value_ = to_;
    return;
  }

  if (current_ <= Duration()) {
    *value_ = from_;
  } else {
    *value_ = from_ +
              (to_ - from_) * easing_function_(current_ / duration_);  // NOLINT
  }

  RequestAnimationFrame();
}

}  // namespace ftxui::animation
#include <functional>  // for function
#include <memory>      // for shared_ptr
#include <utility>     // for move

#ifndef FTXUI_CAPTURED_MOUSE_HPP
#define FTXUI_CAPTURED_MOUSE_HPP

#include <memory>

namespace ftxui {
class CapturedMouseInterface {
 public:
  virtual ~CapturedMouseInterface() = default;
};
using CapturedMouse = std::unique_ptr<CapturedMouseInterface>;
}  // namespace ftxui

#endif /* end of include guard: FTXUI_CAPTURED_MOUSE_HPP */

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#ifndef FTXUI_COMPONENT_HPP
#define FTXUI_COMPONENT_HPP

#include <functional>  // for function
#include <memory>      // for make_shared, shared_ptr
#include <string>      // for wstring
#include <utility>     // for forward
#include <vector>      // for vector

#ifndef FTXUI_COMPONENT_BASE_HPP
#define FTXUI_COMPONENT_BASE_HPP

#include <memory>  // for unique_ptr
#include <vector>  // for vector

namespace ftxui {

class Delegate;
class Focus;
struct Event;

namespace animation {
class Params;
}  // namespace animation

class ComponentBase;
using Component = std::shared_ptr<ComponentBase>;
using Components = std::vector<Component>;

/// @brief It implement rendering itself as ftxui::Element. It implement
/// keyboard navigation by responding to ftxui::Event.
/// @ingroup component
class ComponentBase {
 public:
  // virtual Destructor.
  virtual ~ComponentBase();

  // Component hierarchy:
  ComponentBase* Parent() const;
  Component& ChildAt(size_t i);
  size_t ChildCount() const;
  void Add(Component children);
  void Detach();
  void DetachAllChildren();

  // Renders the component.
  virtual Element Render();

  // Handles an event.
  // By default, reduce on children with a lazy OR.
  //
  // Returns whether the event was handled or not.
  virtual bool OnEvent(Event);

  // Handle an animation step.
  virtual void OnAnimation(animation::Params& params);

  // Focus management ----------------------------------------------------------
  //
  // If this component contains children, this indicates which one is active,
  // nullptr if none is active.
  //
  // We say an element has the focus if the chain of ActiveChild() from the
  // root component contains this object.
  virtual Component ActiveChild();

  // Return true when the component contains focusable elements.
  // The non focusable Component will be skipped when navigating using the
  // keyboard.
  virtual bool Focusable() const;

  // Whether this is the active child of its parent.
  bool Active() const;
  // Whether all the ancestors are active.
  bool Focused() const;

  // Make the |child| to be the "active" one.
  virtual void SetActiveChild(ComponentBase* child);
  void SetActiveChild(Component child);

  // Configure all the ancestors to give focus to this component.
  void TakeFocus();

 protected:
  CapturedMouse CaptureMouse(const Event& event);

  Components children_;

 private:
  ComponentBase* parent_ = nullptr;
};

}  // namespace ftxui

#endif /* end of include guard: FTXUI_COMPONENT_BASE_HPP */

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#ifndef FTXUI_COMPONENT_COMPONENT_OPTIONS_HPP
#define FTXUI_COMPONENT_COMPONENT_OPTIONS_HPP

#include <chrono>                         // for milliseconds
#include <functional>          // for function
// #include <optional>  // provided by cxx11_compat.hpp
#include <string>              // for string

namespace ftxui {

/// @brief arguments for |ButtonOption::transform|, |CheckboxOption::transform|,
/// |Radiobox::transform|, |MenuEntryOption::transform|,
/// |MenuOption::transform|.
struct EntryState {
  std::string label;  /// < The label to display.
  bool state;         /// < The state of the button/checkbox/radiobox
  bool active;        /// < Whether the entry is the active one.
  bool focused;       /// < Whether the entry is one focused by the user.
};

struct UnderlineOption {
  bool enabled = false;

  Color color_active = Color::White;
  Color color_inactive = Color::GrayDark;

  animation::easing::Function leader_function =
      animation::easing::QuadraticInOut;
  animation::easing::Function follower_function =
      animation::easing::QuadraticInOut;

  animation::Duration leader_duration = std::chrono::milliseconds(250);
  animation::Duration leader_delay = std::chrono::milliseconds(0);
  animation::Duration follower_duration = std::chrono::milliseconds(250);
  animation::Duration follower_delay = std::chrono::milliseconds(0);

  void SetAnimation(animation::Duration d, animation::easing::Function f);
  void SetAnimationDuration(animation::Duration d);
  void SetAnimationFunction(animation::easing::Function f);
  void SetAnimationFunction(animation::easing::Function f_leader,
                            animation::easing::Function f_follower);
};

/// @brief Option about a potentially animated color.
/// @ingroup component
struct AnimatedColorOption {
  void Set(
      Color inactive,
      Color active,
      animation::Duration duration = std::chrono::milliseconds(250),
      animation::easing::Function function = animation::easing::QuadraticInOut);

  bool enabled = false;
  Color inactive;
  Color active;
  animation::Duration duration = std::chrono::milliseconds(250);
  animation::easing::Function function = animation::easing::QuadraticInOut;
};

struct AnimatedColorsOption {
  AnimatedColorOption background;
  AnimatedColorOption foreground;
};

/// @brief Option for the MenuEntry component.
/// @ingroup component
struct MenuEntryOption {
  std::function<Element(const EntryState& state)> transform;
  AnimatedColorsOption animated_colors;
};

/// @brief Option for the Menu component.
/// @ingroup component
struct MenuOption {
  // Standard constructors:
  static MenuOption Horizontal();
  static MenuOption HorizontalAnimated();
  static MenuOption Vertical();
  static MenuOption VerticalAnimated();
  static MenuOption Toggle();

  // Style:
  UnderlineOption underline;
  MenuEntryOption entries;
  enum Direction { Up, Down, Left, Right };
  Direction direction = Down;
  std::function<Element()> elements_prefix;
  std::function<Element()> elements_infix;
  std::function<Element()> elements_postfix;

  // Observers:
  std::function<void()> on_change;  ///> Called when the selected entry changes.
  std::function<void()> on_enter;   ///> Called when the user presses enter.
  Ref<int> focused_entry = 0;
};

/// @brief Option for the AnimatedButton component.
/// @ingroup component
struct ButtonOption {
  // Standard constructors:
  static ButtonOption Ascii();
  static ButtonOption Simple();
  static ButtonOption Border();
  static ButtonOption Animated();
  static ButtonOption Animated(Color color);
  static ButtonOption Animated(Color background, Color foreground);
  static ButtonOption Animated(Color background,
                               Color foreground,
                               Color background_active,
                               Color foreground_active);

  // Style:
  std::function<Element(const EntryState&)> transform;
  AnimatedColorsOption animated_colors;
};

/// @brief Option for the Checkbox component.
/// @ingroup component
struct CheckboxOption {
  // Standard constructors:
  static CheckboxOption Simple();

  // Style:
  std::function<Element(const EntryState&)> transform;

  // Observer:
  /// Called when the user change the state.
  std::function<void()> on_change = [] {};
};

/// @brief Option for the Input component.
/// @ingroup component
struct InputOption {
  /// Called when the content changes.
  std::function<void()> on_change = [] {};
  /// Called when the user presses enter.
  std::function<void()> on_enter = [] {};

  /// Obscure the input content using '*'.
  Ref<bool> password = false;

  /// When set different from -1, this attributes is used to store the cursor
  /// position.
  Ref<int> cursor_position = -1;
};

/// @brief Option for the Radiobox component.
/// @ingroup component
struct RadioboxOption {
  // Standard constructors:
  static RadioboxOption Simple();

  // Style:
  std::function<Element(const EntryState&)> transform;

  // Observers:
  /// Called when the selected entry changes.
  std::function<void()> on_change = [] {};
  Ref<int> focused_entry = 0;
};

// @brief Option for the `Slider` component.
// @ingroup component
template <typename T>
struct SliderOption {
  Ref<T> value;
  ConstRef<T> min = T(0);
  ConstRef<T> max = T(100);
  ConstRef<T> increment = (max() - min()) / 20;
  GaugeDirection direction = GaugeDirection::Right;
  Color color_active = Color::White;
  Color color_inactive = Color::GrayDark;
};

}  // namespace ftxui

#endif /* end of include guard: FTXUI_COMPONENT_COMPONENT_OPTIONS_HPP */

// Copyright 2021 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

namespace ftxui {
struct ButtonOption;
struct CheckboxOption;
struct Event;
struct InputOption;
struct MenuOption;
struct RadioboxOption;
struct MenuEntryOption;

template <class T, class... Args>
std::shared_ptr<T> Make(Args&&... args) {
  return std::make_shared<T>(std::forward<Args>(args)...);
}

// Pipe operator to decorate components.
using ComponentDecorator = std::function<Component(Component)>;
using ElementDecorator = std::function<Element(Element)>;
Component operator|(Component component, ComponentDecorator decorator);
Component operator|(Component component, ElementDecorator decorator);
Component& operator|=(Component& component, ComponentDecorator decorator);
Component& operator|=(Component& component, ElementDecorator decorator);

namespace Container {
Component Vertical(Components children);
Component Vertical(Components children, int* selector);
Component Horizontal(Components children);
Component Horizontal(Components children, int* selector);
Component Tab(Components children, int* selector);

}  // namespace Container

Component Button(ConstStringRef label,
                 std::function<void()> on_click,
                 Ref<ButtonOption> = ButtonOption::Simple());

Component Checkbox(ConstStringRef label,
                   bool* checked,
                   Ref<CheckboxOption> option = CheckboxOption::Simple());

Component Input(StringRef content,
                ConstStringRef placeholder,
                Ref<InputOption> option = {});

Component Menu(ConstStringListRef entries,
               int* selected_,
               Ref<MenuOption> = MenuOption::Vertical());
Component MenuEntry(ConstStringRef label, Ref<MenuEntryOption> = {});

Component Dropdown(ConstStringListRef entries, int* selected);

Component Radiobox(ConstStringListRef entries,
                   int* selected_,
                   Ref<RadioboxOption> option = {});
Component Toggle(ConstStringListRef entries, int* selected);

// General slider constructor:
template <typename T>
Component Slider(SliderOption<T> options = {});

// Shorthand without the `SliderOption` constructor:
Component Slider(ConstStringRef label,
                 Ref<int> value,
                 ConstRef<int> min = 0,
                 ConstRef<int> max = 100,
                 ConstRef<int> increment = 5);
Component Slider(ConstStringRef label,
                 Ref<float> value,
                 ConstRef<float> min = 0.f,
                 ConstRef<float> max = 100.f,
                 ConstRef<float> increment = 5.f);
Component Slider(ConstStringRef label,
                 Ref<long> value,
                 ConstRef<long> min = 0l,
                 ConstRef<long> max = 100l,
                 ConstRef<long> increment = 5l);

Component ResizableSplitLeft(Component main, Component back, int* main_size);
Component ResizableSplitRight(Component main, Component back, int* main_size);
Component ResizableSplitTop(Component main, Component back, int* main_size);
Component ResizableSplitBottom(Component main, Component back, int* main_size);

Component Renderer(Component child, std::function<Element()>);
Component Renderer(std::function<Element()>);
Component Renderer(std::function<Element(bool /* focused */)>);
ComponentDecorator Renderer(ElementDecorator);

Component CatchEvent(Component child, std::function<bool(Event)>);
ComponentDecorator CatchEvent(std::function<bool(Event)> on_event);

Component Maybe(Component, const bool* show);
Component Maybe(Component, std::function<bool()>);
ComponentDecorator Maybe(const bool* show);
ComponentDecorator Maybe(std::function<bool()>);

Component Modal(Component main, Component modal, const bool* show_modal);
ComponentDecorator Modal(Component modal, const bool* show_modal);

Component Collapsible(ConstStringRef label,
                      Component child,
                      Ref<bool> show = false);

Component Hoverable(Component component, bool* hover);
Component Hoverable(Component component,
                    std::function<void()> on_enter,
                    std::function<void()> on_leave);
Component Hoverable(Component component,  //
                    std::function<void(bool)> on_change);
ComponentDecorator Hoverable(bool* hover);
ComponentDecorator Hoverable(std::function<void()> on_enter,
                             std::function<void()> on_leave);
ComponentDecorator Hoverable(std::function<void(bool)> on_change);

}  // namespace ftxui

#endif /* end of include guard: FTXUI_COMPONENT_HPP */

// Copyright 2021 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#ifndef FTXUI_COMPONENT_SCREEN_INTERACTIVE_HPP
#define FTXUI_COMPONENT_SCREEN_INTERACTIVE_HPP

#include <atomic>                        // for atomic
#ifndef FTXUI_COMPONENT_RECEIVER_HPP_
#define FTXUI_COMPONENT_RECEIVER_HPP_

#include <algorithm>           // for copy
#include <atomic>              // for atomic, __atomic_base
#include <condition_variable>  // for condition_variable
#include <functional>
#include <iostream>
#include <memory>   // for unique_ptr, make_unique
#include <mutex>    // for mutex, unique_lock
#include <queue>    // for queue
#include <utility>  // for move

namespace ftxui {

// Usage:
//
// Initialization:
// ---------------
//
// auto receiver = MakeReceiver<std:string>();
// auto sender_1= receiver->MakeSender();
// auto sender_2 = receiver->MakeSender();
//
// Then move the senders elsewhere, potentially in a different thread.
//
// On the producer side:
// ----------------------
// [thread 1] sender_1->Send("hello");
// [thread 2] sender_2->Send("world");
//
// On the consumer side:
// ---------------------
// char c;
// while(receiver->Receive(&c)) // Return true as long as there is a producer.
//   print(c)
//
// Receiver::Receive() returns true when there are no more senders.

// clang-format off
template<class T> class SenderImpl;
template<class T> class ReceiverImpl;

template<class T> using Sender = std::unique_ptr<SenderImpl<T>>;
template<class T> using Receiver = std::unique_ptr<ReceiverImpl<T>>;
template<class T> Receiver<T> MakeReceiver();
// clang-format on

// ---- Implementation part ----

template <class T>
class SenderImpl {
 public:
  void Send(T t) { receiver_->Receive(std::move(t)); }
  ~SenderImpl() { receiver_->ReleaseSender(); }

  Sender<T> Clone() { return receiver_->MakeSender(); }

 private:
  friend class ReceiverImpl<T>;
  SenderImpl(ReceiverImpl<T>* consumer) : receiver_(consumer) {}
  ReceiverImpl<T>* receiver_;
};

template <class T>
class ReceiverImpl {
 public:
  Sender<T> MakeSender() {
    std::unique_lock<std::mutex> lock(mutex_);
    senders_++;
    return std::unique_ptr<SenderImpl<T>>(new SenderImpl<T>(this));
  }
  ReceiverImpl() { senders_ = 0; }

  bool Receive(T* t) {
    while (senders_ || !queue_.empty()) {
      std::unique_lock<std::mutex> lock(mutex_);
      if (queue_.empty())
        notifier_.wait(lock);
      if (queue_.empty())
        continue;
      *t = std::move(queue_.front());
      queue_.pop();
      return true;
    }
    return false;
  }

  bool ReceiveNonBlocking(T* t) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (queue_.empty())
      return false;
    *t = queue_.front();
    queue_.pop();
    return true;
  }

  bool HasPending() {
    std::unique_lock<std::mutex> lock(mutex_);
    return !queue_.empty();
  }

  bool HasQuitted() {
    std::unique_lock<std::mutex> lock(mutex_);
    return queue_.empty() && !senders_;
  }

 private:
  friend class SenderImpl<T>;

  void Receive(T t) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      queue_.push(std::move(t));
    }
    notifier_.notify_one();
  }

  void ReleaseSender() {
    senders_--;
    notifier_.notify_one();
  }

  std::mutex mutex_;
  std::queue<T> queue_;
  std::condition_variable notifier_;
  std::atomic<int> senders_;
};

template <class T>
Receiver<T> MakeReceiver() {
  return ftxui::make_unique<ReceiverImpl<T>>();
}

}  // namespace ftxui

#endif  // FTXUI_COMPONENT_RECEIVER_HPP_

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <functional>                    // for function
#include <memory>                        // for shared_ptr
#include <string>                        // for string
#include <thread>                        // for thread
// #include <variant>  // removed for C++11 (Task is a hand-written tagged struct)                       // for variant

#ifndef FTXUI_COMPONENT_ANIMATION_HPP
#define FTXUI_COMPONENT_ANIMATION_HPP

#include <functional>
// #include <variant>  // removed for C++11 (Task is a hand-written tagged struct)

namespace ftxui {
class AnimationTask {};
using Closure = std::function<void()>;
// C++11 replacement for std::variant<Event, Closure, AnimationTask>.
// A tagged struct that keeps the same implicit-conversion behaviour used by
// ScreenInteractive::Post(Task) (Event / Closure / lambda / AnimationTask).
struct Task {
  enum Tag { kEvent, kClosure, kAnimation } tag;
  Event event;
  Closure closure;
  Task() : tag(kEvent) {}
  Task(const Event& e) : tag(kEvent), event(e) {}
  Task(const AnimationTask&) : tag(kAnimation) {}
  // Accept any callable (std::function, lambda, ...) as a Closure in a single
  // user-defined conversion, mirroring std::variant's converting constructor.
  template <typename F,
            typename std::enable_if<
                !std::is_same<typename std::decay<F>::type, Task>::value &&
                    !std::is_same<typename std::decay<F>::type, Event>::value &&
                    !std::is_same<typename std::decay<F>::type,
                                  AnimationTask>::value,
                int>::type = 0>
  Task(F&& f) : tag(kClosure), closure(std::forward<F>(f)) {}
};
}  // namespace ftxui

#endif  // FTXUI_COMPONENT_ANIMATION_HPP

// Copyright 2022 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

namespace ftxui {
class ComponentBase;
class Loop;
struct Event;

using Component = std::shared_ptr<ComponentBase>;
class ScreenInteractivePrivate;

class ScreenInteractive : public Screen {
 public:
  // Constructors:
  static ScreenInteractive FixedSize(int dimx, int dimy);
  static ScreenInteractive Fullscreen();
  static ScreenInteractive FitComponent();
  static ScreenInteractive TerminalOutput();

  // C++11 needs an accessible move-constructor for the factory functions above
  // (C++17 guaranteed copy elision is not available). The implicit one is
  // deleted because of the std::atomic member, so we provide it explicitly.
  ScreenInteractive(ScreenInteractive&&) noexcept;

  // Return the currently active screen, nullptr if none.
  static ScreenInteractive* Active();

  // Start/Stop the main loop.
  void Loop(Component);
  void Exit();
  Closure ExitLoopClosure();

  // Post tasks to be executed by the loop.
  void Post(Task task);
  void PostEvent(Event event);
  void RequestAnimationFrame();

  CapturedMouse CaptureMouse();

  // Decorate a function. The outputted one will execute similarly to the
  // inputted one, but with the currently active screen terminal hooks
  // temporarily uninstalled.
  Closure WithRestoredIO(Closure);

 private:
  void ExitNow();

  void Install();
  void Uninstall();

  void PreMain();
  void PostMain();

  bool HasQuitted();
  void RunOnce(Component component);
  void RunOnceBlocking(Component component);

  void HandleTask(Component component, Task& task);
  void Draw(Component component);
  void ResetCursorPosition();

  void Signal(int signal);

  ScreenInteractive* suspended_screen_ = nullptr;
  enum class Dimension {
    FitComponent,
    Fixed,
    Fullscreen,
    TerminalOutput,
  };
  Dimension dimension_ = Dimension::Fixed;
  bool use_alternative_screen_ = false;
  ScreenInteractive(int dimx,
                    int dimy,
                    Dimension dimension,
                    bool use_alternative_screen);

  Sender<Task> task_sender_;
  Receiver<Task> task_receiver_;

  std::string set_cursor_position;
  std::string reset_cursor_position;

  std::atomic<bool> quit_{false};
  std::thread event_listener_;
  std::thread animation_listener_;
  bool animation_requested_ = false;
  animation::TimePoint previous_animation_time_;

  int cursor_x_ = 1;
  int cursor_y_ = 1;

  bool mouse_captured = false;
  bool previous_frame_resized_ = false;

  bool frame_valid_ = false;

  friend class Loop;

 public:
  class Private {
   public:
    static void Signal(ScreenInteractive& s, int signal) { s.Signal(signal); }
  };
  friend Private;
};

}  // namespace ftxui

#endif /* end of include guard: FTXUI_COMPONENT_SCREEN_INTERACTIVE_HPP */

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

namespace ftxui {

namespace {

Element DefaultTransform(EntryState params) {  // NOLINT
  auto element = text(params.label) | border;
  if (params.active) {
    element |= bold;
  }
  if (params.focused) {
    element |= inverted;
  }
  return element;
}

}  // namespace

/// @brief Draw a button. Execute a function when clicked.
/// @param label The label of the button.
/// @param on_click The action to execute when clicked.
/// @param option Additional optional parameters.
/// @ingroup component
/// @see ButtonBase
///
/// ### Example
///
/// ```cpp
/// auto screen = ScreenInteractive::FitComponent();
/// std::string label = "Click to quit";
/// Component button = Button(&label, screen.ExitLoopClosure());
/// screen.Loop(button)
/// ```
///
/// ### Output
///
/// ```bash
/// ┌─────────────┐
/// │Click to quit│
/// └─────────────┘
/// ```
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
Component Button(ConstStringRef label,
                 std::function<void()> on_click,
                 Ref<ButtonOption> option) {
  class Impl : public ComponentBase {
   public:
    Impl(ConstStringRef label,
         std::function<void()> on_click,
         Ref<ButtonOption> option)
        : label_(std::move(label)),
          on_click_(std::move(on_click)),
          option_(std::move(option)) {}

    // Component implementation:
    Element Render() override {
      const bool active = Active();
      const bool focused = Focused();
      const bool focused_or_hover = focused || mouse_hover_;

      float target = focused_or_hover ? 1.F : 0.F;  // NOLINT
      if (target != animator_background_.to()) {
        SetAnimationTarget(target);
      }

      auto focus_management = focused ? focus : active ? select : nothing;
      const EntryState state = {
          *label_,
          false,
          active,
          focused_or_hover,
      };

      auto element =
          (option_->transform ? option_->transform : DefaultTransform)  //
          (state);
      return element | AnimatedColorStyle() | focus_management | reflect(box_);
    }

    Decorator AnimatedColorStyle() {
      Decorator style = nothing;
      if (option_->animated_colors.background.enabled) {
        style = style | bgcolor(Color::Interpolate(
                            animation_foreground_,  //
                            option_->animated_colors.background.inactive,
                            option_->animated_colors.background.active));
      }
      if (option_->animated_colors.foreground.enabled) {
        style = style | color(Color::Interpolate(
                            animation_foreground_,  //
                            option_->animated_colors.foreground.inactive,
                            option_->animated_colors.foreground.active));
      }
      return style;
    }

    void SetAnimationTarget(float target) {
      if (option_->animated_colors.foreground.enabled) {
        animator_foreground_ =
            animation::Animator(&animation_foreground_, target,
                                option_->animated_colors.foreground.duration,
                                option_->animated_colors.foreground.function);
      }
      if (option_->animated_colors.background.enabled) {
        animator_background_ =
            animation::Animator(&animation_background_, target,
                                option_->animated_colors.background.duration,
                                option_->animated_colors.background.function);
      }
    }

    void OnAnimation(animation::Params& p) override {
      animator_background_.OnAnimation(p);
      animator_foreground_.OnAnimation(p);
    }

    void OnClick() {
      on_click_();
      animation_background_ = 0.5F;  // NOLINT
      animation_foreground_ = 0.5F;  // NOLINT
      SetAnimationTarget(1.F);       // NOLINT
    }

    bool OnEvent(Event event) override {
      if (event.is_mouse()) {
        return OnMouseEvent(event);
      }

      if (event == Event::Return) {
        OnClick();
        return true;
      }
      return false;
    }

    bool OnMouseEvent(Event event) {
      mouse_hover_ =
          box_.Contain(event.mouse().x, event.mouse().y) && CaptureMouse(event);

      if (!mouse_hover_) {
        return false;
      }

      if (event.mouse().button == Mouse::Left &&
          event.mouse().motion == Mouse::Pressed) {
        TakeFocus();
        OnClick();
        return true;
      }

      return false;
    }

    bool Focusable() const final { return true; }

   private:
    ConstStringRef label_;
    std::function<void()> on_click_;
    bool mouse_hover_ = false;
    Box box_;
    Ref<ButtonOption> option_;
    float animation_background_ = 0;
    float animation_foreground_ = 0;
    animation::Animator animator_background_ =
        animation::Animator(&animation_background_);
    animation::Animator animator_foreground_ =
        animation::Animator(&animation_foreground_);
  };

  return Make<Impl>(std::move(label), std::move(on_click), std::move(option));
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <functional>  // for function
#include <memory>  // for __shared_ptr_access, __shared_ptr_access<>::element_type, shared_ptr
#include <type_traits>  // for remove_reference, remove_reference<>::type
#include <utility>      // for move

namespace ftxui {

class CatchEventBase : public ComponentBase {
 public:
  // Constructor.
  explicit CatchEventBase(std::function<bool(Event)> on_event)
      : on_event_(std::move(on_event)) {}

  // Component implementation.
  bool OnEvent(Event event) override {
    if (on_event_(event)) {
      return true;
    } else {
      return ComponentBase::OnEvent(event);
    }
  }

 protected:
  std::function<bool(Event)> on_event_;
};

/// @brief Return a component, using |on_event| to catch events. This function
/// must returns true when the event has been handled, false otherwise.
/// @param child The wrapped component.
/// @param on_event The function drawing the interface.
/// @ingroup component
///
/// ### Example
///
/// ```cpp
/// auto screen = ScreenInteractive::TerminalOutput();
/// auto renderer = Renderer([] {
///   return text("My interface");
/// });
/// auto component = CatchEvent(renderer, [&](Event event) {
///   if (event == Event::Character('q')) {
///     screen.ExitLoopClosure()();
///     return true;
///   }
///   return false;
/// });
/// screen.Loop(component);
/// ```
Component CatchEvent(Component child,
                     std::function<bool(Event event)> on_event) {
  auto out = Make<CatchEventBase>(std::move(on_event));
  out->Add(std::move(child));
  return out;
}

/// @brief Decorate a component, using |on_event| to catch events. This function
/// must returns true when the event has been handled, false otherwise.
/// @param on_event The function drawing the interface.
/// @ingroup component
///
/// ### Example
///
/// ```cpp
/// auto screen = ScreenInteractive::TerminalOutput();
/// auto renderer = Renderer([] { return text("Hello world"); });
/// renderer |= CatchEvent([&](Event event) {
///   if (event == Event::Character('q')) {
///     screen.ExitLoopClosure()();
///     return true;
///   }
///   return false;
/// });
/// screen.Loop(renderer);
/// ```
ComponentDecorator CatchEvent(std::function<bool(Event)> on_event) {
  return [on_event = std::move(on_event)](Component child) {
    return CatchEvent(std::move(child), [on_event = on_event](Event event) {
      return on_event(std::move(event));
    });
  };
}

}  // namespace ftxui

// Copyright 2021 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <functional>  // for function
#include <utility>     // for move

namespace ftxui {

namespace {
class CheckboxBase : public ComponentBase {
 public:
  CheckboxBase(ConstStringRef label, bool* state, Ref<CheckboxOption> option)
      : label_(std::move(label)), state_(state), option_(std::move(option)) {}

 private:
  // Component implementation.
  Element Render() override {
    const bool is_focused = Focused();
    const bool is_active = Active();
    auto focus_management = is_focused ? focus : is_active ? select : nothing;
    auto state = EntryState{
        *label_,
        *state_,
        is_active,
        is_focused || hovered_,
    };
    auto element =
        (option_->transform ? option_->transform
                            : CheckboxOption::Simple().transform)(state);
    return element | focus_management | reflect(box_);
  }

  bool OnEvent(Event event) override {
    if (!CaptureMouse(event)) {
      return false;
    }

    if (event.is_mouse()) {
      return OnMouseEvent(event);
    }

    hovered_ = false;
    if (event == Event::Character(' ') || event == Event::Return) {
      *state_ = !*state_;
      option_->on_change();
      TakeFocus();
      return true;
    }
    return false;
  }

  bool OnMouseEvent(Event event) {
    hovered_ = box_.Contain(event.mouse().x, event.mouse().y);

    if (!CaptureMouse(event)) {
      return false;
    }

    if (!hovered_) {
      return false;
    }

    if (event.mouse().button == Mouse::Left &&
        event.mouse().motion == Mouse::Pressed) {
      *state_ = !*state_;
      option_->on_change();
      return true;
    }

    return false;
  }

  bool Focusable() const final { return true; }

  ConstStringRef label_;
  bool* const state_;
  bool hovered_ = false;
  Ref<CheckboxOption> option_;
  Box box_;
};
}  // namespace

/// @brief Draw checkable element.
/// @param label The label of the checkbox.
/// @param checked Whether the checkbox is checked or not.
/// @param option Additional optional parameters.
/// @ingroup component
/// @see CheckboxBase
///
/// ### Example
///
/// ```cpp
/// auto screen = ScreenInteractive::FitComponent();
/// std::string label = "Make a sandwidth";
/// bool checked = false;
/// Component checkbox = Checkbox(&label, &checked);
/// screen.Loop(checkbox)
/// ```
///
/// ### Output
///
/// ```bash
/// ☐ Make a sandwitch
/// ```
Component Checkbox(ConstStringRef label,
                   bool* checked,
                   Ref<CheckboxOption> option) {
  return Make<CheckboxBase>(std::move(label), checked, std::move(option));
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <functional>  // for function
#include <memory>      // for shared_ptr, allocator
#include <utility>     // for move

namespace ftxui {

/// @brief A collapsible component. It display a checkbox with an arrow. Once
/// activated, the children is displayed.
/// @params label The label of the checkbox.
/// @params child The children to display.
/// @params show Hold the state about whether the children is displayed or not.
///
/// ### Example
/// ```cpp
/// auto component = Collapsible("Show details", details);
/// ```
///
/// ### Output
/// ```
///
/// ▼ Show details
/// <details component>
///  ```
Component Collapsible(ConstStringRef label, Component child, Ref<bool> show) {
  class Impl : public ComponentBase {
   public:
    Impl(ConstStringRef label, Component child, Ref<bool> show) : show_(show) {
      CheckboxOption opt;
      opt.transform = [](EntryState s) {            // NOLINT
        auto prefix = text(s.state ? "▼ " : "▶ ");  // NOLINT
        auto t = text(s.label);
        if (s.active) {
          t |= bold;
        }
        if (s.focused) {
          t |= inverted;
        }
        return hbox({prefix, t});
      };
      Add(Container::Vertical({
          Checkbox(std::move(label), show_.operator->(), opt),
          Maybe(std::move(child), show_.operator->()),
      }));
    }
    Ref<bool> show_;
  };

  return Make<Impl>(std::move(label), std::move(child), show);
}

}  // namespace ftxui

// Copyright 2021 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <algorithm>  // for find_if
#include <cassert>    // for assert
#include <cstddef>    // for size_t
#include <iterator>   // for begin, end
#include <utility>    // for move
#include <vector>     // for vector, __alloc_traits<>::value_type

namespace ftxui::animation {
class Params;
}  // namespace ftxui::animation

namespace ftxui {

namespace {
class CaptureMouseImpl : public CapturedMouseInterface {};
}  // namespace

ComponentBase::~ComponentBase() {
  DetachAllChildren();
}

/// @brief Return the parent ComponentBase, or nul if any.
/// @see Detach
/// @see Parent
/// @ingroup component
ComponentBase* ComponentBase::Parent() const {
  return parent_;
}

/// @brief Access the child at index `i`.
/// @ingroup component
Component& ComponentBase::ChildAt(size_t i) {
  assert(i < ChildCount());  // NOLINT
  return children_[i];
}

/// @brief Returns the number of children.
/// @ingroup component
size_t ComponentBase::ChildCount() const {
  return children_.size();
}

/// @brief Add a child.
/// @@param child The child to be attached.
/// @ingroup component
void ComponentBase::Add(Component child) {
  child->Detach();
  child->parent_ = this;
  children_.push_back(std::move(child));
}

/// @brief Detach this child from its parent.
/// @see Detach
/// @see Parent
/// @ingroup component
void ComponentBase::Detach() {
  if (parent_ == nullptr) {
    return;
  }
  auto it = std::find_if(std::begin(parent_->children_),  //
                         std::end(parent_->children_),    //
                         [this](const Component& that) {  //
                           return this == that.get();
                         });
  ComponentBase* parent = parent_;
  parent_ = nullptr;
  parent->children_.erase(it);  // Might delete |this|.
}

/// @brief Remove all children.
/// @ingroup component
void ComponentBase::DetachAllChildren() {
  while (!children_.empty()) {
    children_[0]->Detach();
  }
}

/// @brief Draw the component.
/// Build a ftxui::Element to be drawn on the ftxi::Screen representing this
/// ftxui::ComponentBase.
/// @ingroup component
Element ComponentBase::Render() {
  if (children_.size() == 1) {
    return children_.front()->Render();
  }

  return text("Not implemented component");
}

/// @brief Called in response to an event.
/// @param event The event.
/// @return True when the event has been handled.
/// The default implementation called OnEvent on every child until one return
/// true. If none returns true, return false.
/// @ingroup component
bool ComponentBase::OnEvent(Event event) {  // NOLINT
  for (Component& child : children_) {      // NOLINT
    if (child->OnEvent(event)) {
      return true;
    }
  }
  return false;
}

/// @brief Called in response to an animation event.
/// @param animation_params the parameters of the animation
/// The default implementation dispatch the event to every child.
/// @ingroup component
void ComponentBase::OnAnimation(animation::Params& params) {
  for (const Component& child : children_) {
    child->OnAnimation(params);
  }
}

/// @brief Return the currently Active child.
/// @return the currently Active child.
/// @ingroup component
Component ComponentBase::ActiveChild() {
  for (auto& child : children_) {
    if (child->Focusable()) {
      return child;
    }
  }
  return nullptr;
}

/// @brief Return true when the component contains focusable elements.
/// The non focusable Components will be skipped when navigating using the
/// keyboard.
/// @ingroup component
bool ComponentBase::Focusable() const {
  for (const Component& child : children_) {  // NOLINT
    if (child->Focusable()) {
      return true;
    }
  }
  return false;
}

/// @brief Returns if the element if the currently active child of its parent.
/// @ingroup component
bool ComponentBase::Active() const {
  return parent_ == nullptr || parent_->ActiveChild().get() == this;
}

/// @brief Returns if the elements if focused by the user.
/// True when the ComponentBase is focused by the user. An element is Focused
/// when it is with all its ancestors the ActiveChild() of their parents, and it
/// Focusable().
/// @ingroup component
bool ComponentBase::Focused() const {
  const auto* current = this;
  while (current && current->Active()) {
    current = current->parent_;
  }
  return !current && Focusable();
}

/// @brief Make the |child| to be the "active" one.
/// @param child the child to become active.
/// @ingroup component
void ComponentBase::SetActiveChild(ComponentBase* /*child*/) {}

/// @brief Make the |child| to be the "active" one.
/// @param child the child to become active.
/// @ingroup component
void ComponentBase::SetActiveChild(Component child) {  // NOLINT
  SetActiveChild(child.get());
}

/// @brief Configure all the ancestors to give focus to this component.
/// @ingroup component
void ComponentBase::TakeFocus() {
  ComponentBase* child = this;
  while (ComponentBase* parent = child->parent_) {
    parent->SetActiveChild(child);
    child = parent;
  }
}

/// @brief Take the CapturedMouse if available. There is only one component of
/// them. It represents a component taking priority over others.
/// @param event
/// @ingroup component
CapturedMouse ComponentBase::CaptureMouse(const Event& event) {  // NOLINT
  if (event.screen_) {
    return event.screen_->CaptureMouse();
  }
  return ftxui::make_unique<CaptureMouseImpl>();
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

#include <memory>   // for shared_ptr
#include <utility>  // for move

namespace ftxui {

void AnimatedColorOption::Set(Color a_inactive,
                              Color a_active,
                              animation::Duration a_duration,
                              animation::easing::Function a_function) {
  enabled = true;
  inactive = a_inactive;
  active = a_active;
  duration = a_duration;
  function = std::move(a_function);
}

void UnderlineOption::SetAnimation(animation::Duration d,
                                   animation::easing::Function f) {
  SetAnimationDuration(d);
  SetAnimationFunction(std::move(f));
}

void UnderlineOption::SetAnimationDuration(animation::Duration d) {
  leader_duration = d;
  follower_duration = d;
}

void UnderlineOption::SetAnimationFunction(animation::easing::Function f) {
  leader_function = f;
  follower_function = std::move(f);
}

void UnderlineOption::SetAnimationFunction(
    animation::easing::Function f_leader,
    animation::easing::Function f_follower) {
  leader_function = std::move(f_leader);
  follower_function = std::move(f_follower);
}

// static
MenuOption MenuOption::Horizontal() {
  MenuOption option;
  option.direction = Direction::Right;
  option.entries.transform = [](const EntryState& state) {
    Element e = text(state.label);
    if (state.focused) {
      e |= inverted;
    }
    if (state.active) {
      e |= bold;
    }
    if (!state.focused && !state.active) {
      e |= dim;
    }
    return e;
  };
  option.elements_infix = [] { return text(" "); };

  return option;
}

// static
MenuOption MenuOption::HorizontalAnimated() {
  auto option = Horizontal();
  option.underline.enabled = true;
  return option;
}

// static
MenuOption MenuOption::Vertical() {
  MenuOption option;
  option.entries.transform = [](const EntryState& state) {
    Element e = text((state.active ? "> " : "  ") + state.label);  // NOLINT
    if (state.focused) {
      e |= inverted;
    }
    if (state.active) {
      e |= bold;
    }
    if (!state.focused && !state.active) {
      e |= dim;
    }
    return e;
  };
  return option;
}

// static
MenuOption MenuOption::VerticalAnimated() {
  auto option = MenuOption::Vertical();
  option.entries.transform = [](const EntryState& state) {
    Element e = text(state.label);
    if (state.focused) {
      e |= inverted;
    }
    if (state.active) {
      e |= bold;
    }
    if (!state.focused && !state.active) {
      e |= dim;
    }
    return e;
  };
  option.underline.enabled = true;
  return option;
}

// static
MenuOption MenuOption::Toggle() {
  auto option = MenuOption::Horizontal();
  option.elements_infix = [] { return text("│") | automerge; };
  return option;
}

/// @brief Create a ButtonOption, highlighted using [] characters.
// static
ButtonOption ButtonOption::Ascii() {
  ButtonOption option;
  option.transform = [](const EntryState& s) {
    const std::string label = s.focused ? "[" + s.label + "]"  //
                                        : " " + s.label + " ";
    return text(label);
  };
  return option;
}

/// @brief Create a ButtonOption, inverted when focused.
// static
ButtonOption ButtonOption::Simple() {
  ButtonOption option;
  option.transform = [](const EntryState& s) {
    auto element = text(s.label) | borderLight;
    if (s.focused) {
      element |= inverted;
    }
    return element;
  };
  return option;
}

/// @brief Create a ButtonOption. The button is shown using a border, inverted
/// when focused. This is the current default.
ButtonOption ButtonOption::Border() {
  ButtonOption option;
  option.transform = [](const EntryState& s) {
    auto element = text(s.label) | border;
    if (s.active) {
      element |= bold;
    }
    if (s.focused) {
      element |= inverted;
    }
    return element;
  };
  return option;
}

/// @brief Create a ButtonOption, using animated colors.
// static
ButtonOption ButtonOption::Animated() {
  return Animated(Color::Black, Color::GrayLight,  //
                  Color::GrayDark, Color::White);
}

/// @brief Create a ButtonOption, using animated colors.
// static
ButtonOption ButtonOption::Animated(Color color) {
  return ButtonOption::Animated(
      Color::Interpolate(0.85F, color, Color::Black),   // NOLINT
      Color::Interpolate(0.10F, color, Color::White),   // NOLINT
      Color::Interpolate(0.10F, color, Color::Black),   // NOLINT
      Color::Interpolate(0.85F, color, Color::White));  // NOLINT
}

/// @brief Create a ButtonOption, using animated colors.
// static
ButtonOption ButtonOption::Animated(Color background, Color foreground) {
  // NOLINTBEGIN
  return ButtonOption::Animated(
      /*bakground=*/background,
      /*foreground=*/foreground,
      /*background_active=*/foreground,
      /*foreground_active=*/background);
  // NOLINTEND
}

/// @brief Create a ButtonOption, using animated colors.
// static
ButtonOption ButtonOption::Animated(Color background,
                                    Color foreground,
                                    Color background_active,
                                    Color foreground_active) {
  ButtonOption option;
  option.transform = [](const EntryState& s) {
    auto element = text(s.label) | borderEmpty;
    if (s.focused) {
      element |= bold;
    }
    return element;
  };
  option.animated_colors.foreground.Set(foreground, foreground_active);
  option.animated_colors.background.Set(background, background_active);
  return option;
}

/// @brief Option for standard Checkbox.
// static
CheckboxOption CheckboxOption::Simple() {
  auto option = CheckboxOption();
  option.transform = [](const EntryState& s) {
#if defined(FTXUI_MICROSOFT_TERMINAL_FALLBACK)
    // Microsoft terminal do not use fonts able to render properly the default
    // radiobox glyph.
    auto prefix = text(s.state ? "[X] " : "[ ] ");  // NOLINT
#else
    auto prefix = text(s.state ? "▣ " : "☐ ");  // NOLINT
#endif
    auto t = text(s.label);
    if (s.active) {
      t |= bold;
    }
    if (s.focused) {
      t |= inverted;
    }
    return hbox({prefix, t});
  };
  return option;
}

/// @brief Option for standard Radiobox
// static
RadioboxOption RadioboxOption::Simple() {
  auto option = RadioboxOption();
  option.transform = [](const EntryState& s) {
#if defined(FTXUI_MICROSOFT_TERMINAL_FALLBACK)
    // Microsoft terminal do not use fonts able to render properly the default
    // radiobox glyph.
    auto prefix = text(s.state ? "(*) " : "( ) ");  // NOLINT
#else
    auto prefix = text(s.state ? "◉ " : "○ ");  // NOLINT
#endif
    auto t = text(s.label);
    if (s.active) {
      t |= bold;
    }
    if (s.focused) {
      t |= inverted;
    }
    return hbox({prefix, t});
  };
  return option;
}

}  // namespace ftxui

// Copyright 2022 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <algorithm>  // for max, min
#include <cstddef>    // for size_t
#include <memory>  // for make_shared, __shared_ptr_access, allocator, shared_ptr, allocator_traits<>::value_type
#include <utility>  // for move
#include <vector>   // for vector, __alloc_traits<>::value_type

namespace ftxui {

class ContainerBase : public ComponentBase {
 public:
  ContainerBase(Components children, int* selector)
      : selector_(selector ? selector : &selected_) {
    for (Component& child : children) {
      Add(std::move(child));
    }
  }

  // Component override.
  bool OnEvent(Event event) override {
    if (event.is_mouse()) {
      return OnMouseEvent(event);
    }

    if (!Focused()) {
      return false;
    }

    if (ActiveChild() && ActiveChild()->OnEvent(event)) {
      return true;
    }

    return EventHandler(event);
  }

  Component ActiveChild() override {
    if (children_.empty()) {
      return nullptr;
    }

    return children_[*selector_ % children_.size()];
  }

  void SetActiveChild(ComponentBase* child) override {
    for (size_t i = 0; i < children_.size(); ++i) {
      if (children_[i].get() == child) {
        *selector_ = (int)i;
        return;
      }
    }
  }

 protected:
  // Handlers
  virtual bool EventHandler(Event /*unused*/) { return false; }  // NOLINT

  virtual bool OnMouseEvent(Event event) {
    return ComponentBase::OnEvent(std::move(event));
  }

  int selected_ = 0;
  int* selector_ = nullptr;

  void MoveSelector(int dir) {
    for (int i = *selector_ + dir; i >= 0 && i < (int)children_.size();
         i += dir) {
      if (children_[i]->Focusable()) {
        *selector_ = i;
        return;
      }
    }
  }

  void MoveSelectorWrap(int dir) {
    if (children_.empty()) {
      return;
    }
    for (size_t offset = 1; offset < children_.size(); ++offset) {
      const size_t i = ((size_t(*selector_ + offset * dir + children_.size())) %
                        children_.size());
      if (children_[i]->Focusable()) {
        *selector_ = (int)i;
        return;
      }
    }
  }
};

class VerticalContainer : public ContainerBase {
 public:
  using ContainerBase::ContainerBase;

  Element Render() override {
    Elements elements;
    for (auto& it : children_) {
      elements.push_back(it->Render());
    }
    if (elements.empty()) {
      return text("Empty container") | reflect(box_);
    }
    return vbox(std::move(elements)) | reflect(box_);
  }

  bool EventHandler(Event event) override {
    const int old_selected = *selector_;
    if (event == Event::ArrowUp || event == Event::Character('k')) {
      MoveSelector(-1);
    }
    if (event == Event::ArrowDown || event == Event::Character('j')) {
      MoveSelector(+1);
    }
    if (event == Event::PageUp) {
      for (int i = 0; i < box_.y_max - box_.y_min; ++i) {
        MoveSelector(-1);
      }
    }
    if (event == Event::PageDown) {
      for (int i = 0; i < box_.y_max - box_.y_min; ++i) {
        MoveSelector(1);
      }
    }
    if (event == Event::Home) {
      for (size_t i = 0; i < children_.size(); ++i) {
        MoveSelector(-1);
      }
    }
    if (event == Event::End) {
      for (size_t i = 0; i < children_.size(); ++i) {
        MoveSelector(1);
      }
    }
    if (event == Event::Tab) {
      MoveSelectorWrap(+1);
    }
    if (event == Event::TabReverse) {
      MoveSelectorWrap(-1);
    }

    *selector_ = std::max(0, std::min(int(children_.size()) - 1, *selector_));
    return old_selected != *selector_;
  }

  bool OnMouseEvent(Event event) override {
    if (ContainerBase::OnMouseEvent(event)) {
      return true;
    }

    if (event.mouse().button != Mouse::WheelUp &&
        event.mouse().button != Mouse::WheelDown) {
      return false;
    }

    if (!box_.Contain(event.mouse().x, event.mouse().y)) {
      return false;
    }

    if (event.mouse().button == Mouse::WheelUp) {
      MoveSelector(-1);
    }
    if (event.mouse().button == Mouse::WheelDown) {
      MoveSelector(+1);
    }
    *selector_ = std::max(0, std::min(int(children_.size()) - 1, *selector_));

    return true;
  }

  Box box_;
};

class HorizontalContainer : public ContainerBase {
 public:
  using ContainerBase::ContainerBase;

  Element Render() override {
    Elements elements;
    for (auto& it : children_) {
      elements.push_back(it->Render());
    }
    if (elements.empty()) {
      return text("Empty container");
    }
    return hbox(std::move(elements));
  }

  bool EventHandler(Event event) override {
    const int old_selected = *selector_;
    if (event == Event::ArrowLeft || event == Event::Character('h')) {
      MoveSelector(-1);
    }
    if (event == Event::ArrowRight || event == Event::Character('l')) {
      MoveSelector(+1);
    }
    if (event == Event::Tab) {
      MoveSelectorWrap(+1);
    }
    if (event == Event::TabReverse) {
      MoveSelectorWrap(-1);
    }

    *selector_ = std::max(0, std::min(int(children_.size()) - 1, *selector_));
    return old_selected != *selector_;
  }
};

class TabContainer : public ContainerBase {
 public:
  using ContainerBase::ContainerBase;

  Element Render() override {
    const Component active_child = ActiveChild();
    if (active_child) {
      return active_child->Render();
    }
    return text("Empty container");
  }

  bool Focusable() const override {
    if (children_.empty()) {
      return false;
    }
    return children_[*selector_ % children_.size()]->Focusable();
  }

  bool OnMouseEvent(Event event) override {
    return ActiveChild() && ActiveChild()->OnEvent(event);
  }
};

namespace Container {

/// @brief A list of components, drawn one by one vertically and navigated
/// vertically using up/down arrow key or 'j'/'k' keys.
/// @param children the list of components.
/// @ingroup component
/// @see ContainerBase
///
/// ### Example
///
/// ```cpp
/// auto container = Container::Vertical({
///   children_1,
///   children_2,
///   children_3,
///   children_4,
/// });
/// ```
Component Vertical(Components children) {
  return Vertical(std::move(children), nullptr);
}

/// @brief A list of components, drawn one by one vertically and navigated
/// vertically using up/down arrow key or 'j'/'k' keys.
/// This is useful for implementing a Menu for instance.
/// @param children the list of components.
/// @param selector A reference to the index of the selected children.
/// @ingroup component
/// @see ContainerBase
///
/// ### Example
///
/// ```cpp
/// auto container = Container::Vertical({
///   children_1,
///   children_2,
///   children_3,
///   children_4,
/// });
/// ```
Component Vertical(Components children, int* selector) {
  return std::make_shared<VerticalContainer>(std::move(children), selector);
}

/// @brief A list of components, drawn one by one horizontally and navigated
/// horizontally using left/right arrow key or 'h'/'l' keys.
/// @param children the list of components.
/// @ingroup component
/// @see ContainerBase
///
/// ### Example
///
/// ```cpp
/// int selected_children = 2;
/// auto container = Container::Horizontal({
///   children_1,
///   children_2,
///   children_3,
///   children_4,
/// }, &selected_children);
/// ```
Component Horizontal(Components children) {
  return Horizontal(std::move(children), nullptr);
}

/// @brief A list of components, drawn one by one horizontally and navigated
/// horizontally using left/right arrow key or 'h'/'l' keys.
/// @param children the list of components.
/// @param selector A reference to the index of the selected children.
/// @ingroup component
/// @see ContainerBase
///
/// ### Example
///
/// ```cpp
/// int selected_children = 2;
/// auto container = Container::Horizontal({
///   children_1,
///   children_2,
///   children_3,
///   children_4,
/// }, selected_children);
/// ```
Component Horizontal(Components children, int* selector) {
  return std::make_shared<HorizontalContainer>(std::move(children), selector);
}

/// @brief A list of components, where only one is drawn and interacted with at
/// a time. The |selector| gives the index of the selected component. This is
/// useful to implement tabs.
/// @param children The list of components.
/// @param selector The index of the drawn children.
/// @ingroup component
/// @see ContainerBase
///
/// ### Example
///
/// ```cpp
/// int tab_drawn = 0;
/// auto container = Container::Tab({
///   children_1,
///   children_2,
///   children_3,
///   children_4,
/// }, &tab_drawn);
/// ```
Component Tab(Components children, int* selector) {
  return std::make_shared<TabContainer>(std::move(children), selector);
}

}  // namespace Container

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <algorithm>   // for max, min
#include <functional>  // for function
#include <memory>      // for __shared_ptr_access, shared_ptr, allocator
#include <string>      // for string

namespace ftxui {

Component Dropdown(ConstStringListRef entries, int* selected) {
  class Impl : public ComponentBase {
   public:
    Impl(ConstStringListRef entries, int* selected)
        : entries_(entries), selected_(selected) {
      CheckboxOption option;
      option.transform = [](const EntryState& s) {
        auto prefix = text(s.state ? "↓ " : "→ ");  // NOLINT
        auto t = text(s.label);
        if (s.active) {
          t |= bold;
        }
        if (s.focused) {
          t |= inverted;
        }
        return hbox({prefix, t});
      };
      checkbox_ = Checkbox(&title_, &show_, option),
      radiobox_ = Radiobox(entries_, selected_);

      Add(Container::Vertical({
          checkbox_,
          Maybe(radiobox_, &show_),
      }));
    }

    Element Render() override {
      *selected_ = std::min((int)entries_.size() - 1, std::max(0, *selected_));
      title_ = entries_[*selected_];
      if (show_) {
        const int max_height = 12;
        return vbox({
                   checkbox_->Render(),
                   separator(),
                   radiobox_->Render() | vscroll_indicator | frame |
                       size(HEIGHT, LESS_THAN, max_height),
               }) |
               border;
      }

      return vbox({
          checkbox_->Render() | border,
          filler(),
      });
    }

   private:
    ConstStringListRef entries_;
    bool show_ = false;
    int* selected_;
    std::string title_;
    Component checkbox_;
    Component radiobox_;
  };

  return Make<Impl>(entries, selected);
}

}  // namespace ftxui

// Copyright 2021 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <utility>  // for move

namespace ftxui {

// static
Event Event::Character(std::string input) {
  Event event;
  event.input_ = std::move(input);
  event.type_ = Type::Character;
  return event;
}

// static
Event Event::Character(char c) {
  return Event::Character(std::string{c});
}

// static
Event Event::Character(wchar_t c) {
  return Event::Character(to_string(std::wstring{c}));
}

// static
Event Event::Mouse(std::string input, struct Mouse mouse) {
  Event event;
  event.input_ = std::move(input);
  event.type_ = Type::Mouse;
  event.mouse_ = mouse;  // NOLINT
  return event;
}

// static
Event Event::Special(std::string input) {
  Event event;
  event.input_ = std::move(input);
  return event;
}

// static
Event Event::CursorReporting(std::string input, int x, int y) {
  Event event;
  event.input_ = std::move(input);
  event.type_ = Type::CursorReporting;
  event.cursor_.x = x;  // NOLINT
  event.cursor_.y = y;  // NOLINT
  return event;
}

// --- Arrow ---
const Event Event::ArrowLeft = Event::Special("\x1B[D");          // NOLINT
const Event Event::ArrowRight = Event::Special("\x1B[C");         // NOLINT
const Event Event::ArrowUp = Event::Special("\x1B[A");            // NOLINT
const Event Event::ArrowDown = Event::Special("\x1B[B");          // NOLINT
const Event Event::ArrowLeftCtrl = Event::Special("\x1B[1;5D");   // NOLINT
const Event Event::ArrowRightCtrl = Event::Special("\x1B[1;5C");  // NOLINT
const Event Event::ArrowUpCtrl = Event::Special("\x1B[1;5A");     // NOLINT
const Event Event::ArrowDownCtrl = Event::Special("\x1B[1;5B");   // NOLINT
const Event Event::Backspace = Event::Special({127});             // NOLINT
const Event Event::Delete = Event::Special("\x1B[3~");            // NOLINT
const Event Event::Escape = Event::Special("\x1B");               // NOLINT
const Event Event::Return = Event::Special({10});                 // NOLINT
const Event Event::Tab = Event::Special({9});                     // NOLINT
const Event Event::TabReverse = Event::Special({27, 91, 90});     // NOLINT

// See https://invisible-island.net/xterm/xterm-function-keys.html
// We follow xterm-new / vterm-xf86-v4 / mgt / screen
const Event Event::F1 = Event::Special("\x1BOP");     // NOLINT
const Event Event::F2 = Event::Special("\x1BOQ");     // NOLINT
const Event Event::F3 = Event::Special("\x1BOR");     // NOLINT
const Event Event::F4 = Event::Special("\x1BOS");     // NOLINT
const Event Event::F5 = Event::Special("\x1B[15~");   // NOLINT
const Event Event::F6 = Event::Special("\x1B[17~");   // NOLINT
const Event Event::F7 = Event::Special("\x1B[18~");   // NOLINT
const Event Event::F8 = Event::Special("\x1B[19~");   // NOLINT
const Event Event::F9 = Event::Special("\x1B[20~");   // NOLINT
const Event Event::F10 = Event::Special("\x1B[21~");  // NOLINT
const Event Event::F11 = Event::Special("\x1B[23~");  // NOLINT
const Event Event::F12 = Event::Special("\x1B[24~");  // NOLINT

const Event Event::Home = Event::Special({27, 91, 72});           // NOLINT
const Event Event::End = Event::Special({27, 91, 70});            // NOLINT
const Event Event::PageUp = Event::Special({27, 91, 53, 126});    // NOLINT
const Event Event::PageDown = Event::Special({27, 91, 54, 126});  // NOLINT
const Event Event::Custom = Event::Special({0});                  // NOLINT

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <functional>                          // for function
#include <utility>                             // for move

namespace ftxui {

namespace {

void Post(std::function<void()> f) {
  if (auto* screen = ScreenInteractive::Active()) {
    screen->Post(std::move(f));
    return;
  }
  f();
}

}  // namespace

/// @brief Wrap a component. Gives the ability to know if it is hovered by the
/// mouse.
/// @param component: The wrapped component.
/// @param hover: The value to reflect whether the component is hovered or not.
/// @ingroup component
///
/// ### Example
///
/// ```cpp
/// auto button = Button("exit", screen.ExitLoopClosure());
/// bool hover = false;
/// auto button_hover = Hoverable(button, &hover);
/// ```
// NOLINTNEXTLINE
Component Hoverable(Component component, bool* hover) {
  class Impl : public ComponentBase {
   public:
    Impl(Component component, bool* hover)
        : component_(std::move(component)), hover_(hover) {
      Add(component_);
    }

   private:
    Element Render() override {
      return ComponentBase::Render() | reflect(box_);
    }

    bool OnEvent(Event event) override {
      if (event.is_mouse()) {
        *hover_ = box_.Contain(event.mouse().x, event.mouse().y) &&
                  CaptureMouse(event);
      }

      return ComponentBase::OnEvent(event);
    }

    Component component_;
    bool* hover_;
    Box box_;
  };

  return Make<Impl>(component, hover);
}

/// @brief Wrap a component. Gives the ability to know if it is hovered by the
/// mouse.
/// @param component: The wrapped component.
/// @param hover: The value to reflect whether the component is hovered or not.
/// @ingroup component
///
/// ### Example
///
/// ```cpp
/// auto button = Button("exit", screen.ExitLoopClosure());
/// bool hover = false;
/// auto button_hover = Hoverable(button, &hover);
/// ```
Component Hoverable(Component component,
                    std::function<void()> on_enter,
                    std::function<void()> on_leave) {
  class Impl : public ComponentBase {
   public:
    Impl(Component component,
         std::function<void()> on_enter,
         std::function<void()> on_leave)
        : component_(std::move(component)),
          on_enter_(std::move(on_enter)),
          on_leave_(std::move(on_leave)) {
      Add(component_);
    }

   private:
    Element Render() override {
      return ComponentBase::Render() | reflect(box_);
    }

    bool OnEvent(Event event) override {
      if (event.is_mouse()) {
        const bool hover = box_.Contain(event.mouse().x, event.mouse().y) &&
                           CaptureMouse(event);
        if (hover != hover_) {
          Post(hover ? on_enter_ : on_leave_);
        }
        hover_ = hover;
      }

      return ComponentBase::OnEvent(event);
    }

    Component component_;
    Box box_;
    bool hover_ = false;
    std::function<void()> on_enter_;
    std::function<void()> on_leave_;
  };

  return Make<Impl>(std::move(component), std::move(on_enter),
                    std::move(on_leave));
}

/// @brief Wrap a component. Gives the ability to know if it is hovered by the
/// mouse.
/// @param hover: The value to reflect whether the component is hovered or not.
/// @ingroup component
///
/// ### Example
///
/// ```cpp
/// bool hover = false;
/// auto button = Button("exit", screen.ExitLoopClosure());
/// button |= Hoverable(&hover);
/// ```
ComponentDecorator Hoverable(bool* hover) {
  return [hover](Component component) {
    return Hoverable(std::move(component), hover);
  };
}

/// @brief Wrap a component. Gives the ability to know if it is hovered by the
/// mouse.
/// @param on_enter is called when the mouse hover the component.
/// @param on_leave is called when the mouse leave the component.
/// @ingroup component
///
/// ### Example
///
/// ```cpp
/// auto button = Button("exit", screen.ExitLoopClosure());
/// int on_enter_cnt = 0;
/// int on_leave_cnt = 0;
/// button |= Hoverable(
///   [&]{ on_enter_cnt++; },
///   [&]{ on_leave_cnt++; }
//  );
/// ```
// NOLINTNEXTLINE
ComponentDecorator Hoverable(std::function<void()> on_enter,
                             // NOLINTNEXTLINE
                             std::function<void()> on_leave) {
  return [on_enter, on_leave](Component component) {
    return Hoverable(std::move(component), on_enter, on_leave);
  };
}

/// @brief Wrap a component. Gives the ability to know if it is hovered by the
/// mouse.
/// @param component the wrapped component.
/// @param on_change is called when the mouse enter or leave the component.
/// @ingroup component
///
/// ### Example
///
/// ```cpp
/// auto button = Button("exit", screen.ExitLoopClosure());
/// bool hovered = false;
/// auto button_hoverable = Hoverable(button,
//                                    [&](bool hover) { hovered = hover;});
/// ```
// NOLINTNEXTLINE
Component Hoverable(Component component, std::function<void(bool)> on_change) {
  return Hoverable(
      std::move(component),              //
      [on_change] { on_change(true); },  //
      [on_change] { on_change(false); }  //
  );
}

/// @brief Wrap a component. Gives the ability to know if it is hovered by the
/// mouse.
/// @param on_change is called when the mouse enter or leave the component.
/// @ingroup component
///
/// ### Example
///
/// ```cpp
/// auto button = Button("exit", screen.ExitLoopClosure());
/// bool hovered = false;
/// button |= Hoverable([&](bool hover) { hovered = hover;});
/// ```
// NOLINTNEXTLINE
ComponentDecorator Hoverable(std::function<void(bool)> on_change) {
  return [on_change](Component component) {
    return Hoverable(std::move(component), on_change);
  };
}

}  // namespace ftxui

// Copyright 2022 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <algorithm>   // for max, min
#include <cstddef>     // for size_t
#include <functional>  // for function
#include <memory>      // for shared_ptr
#include <string>      // for string, allocator
#include <utility>     // for move
#include <vector>      // for vector

namespace ftxui {
namespace util {

// Similar to std::clamp, but allow hi to be lower than lo.
template <class T>
constexpr const T& clamp(const T& v, const T& lo, const T& hi) {
  return v < lo ? lo : hi < v ? hi : v;
}

}  // namespace util
}  // namespace ftxui

// Copyright 2022 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

namespace ftxui {

namespace {

// Group together several propertiej so they appear to form a similar group.
// For instance, letters are grouped with number and form a single word.
bool IsWordCharacter(WordBreakProperty property) {
  switch (property) {
    case WordBreakProperty::ALetter:
    case WordBreakProperty::Hebrew_Letter:
    case WordBreakProperty::Katakana:
    case WordBreakProperty::Numeric:
      return true;

    case WordBreakProperty::CR:
    case WordBreakProperty::Double_Quote:
    case WordBreakProperty::LF:
    case WordBreakProperty::MidLetter:
    case WordBreakProperty::MidNum:
    case WordBreakProperty::MidNumLet:
    case WordBreakProperty::Newline:
    case WordBreakProperty::Single_Quote:
    case WordBreakProperty::WSegSpace:
    // Unsure:
    case WordBreakProperty::Extend:
    case WordBreakProperty::ExtendNumLet:
    case WordBreakProperty::Format:
    case WordBreakProperty::Regional_Indicator:
    case WordBreakProperty::ZWJ:
      return false;
  }
  return true;  // NOT_REACHED();
}

std::string PasswordField(size_t size) {
  std::string out;
  out.reserve(2 * size);
  while (size--) {
    out += "•";
  }
  return out;
}

// An input box. The user can type text into it.
class InputBase : public ComponentBase {
 public:
  InputBase(StringRef content,
            ConstStringRef placeholder,
            Ref<InputOption> option)
      : content_(std::move(content)),
        placeholder_(std::move(placeholder)),
        option_(std::move(option)) {}

  int cursor_position_internal_ = 0;
  int& cursor_position() {
    int& opt = option_->cursor_position();
    if (opt != -1) {
      return opt;
    }
    return cursor_position_internal_;
  }

  // Component implementation:
  Element Render() override {
    std::string password_content;
    if (option_->password()) {
      password_content = PasswordField(content_->size());
    }
    const std::string& content =
        option_->password() ? password_content : *content_;

    const int size = GlyphCount(content);

    cursor_position() = std::max(0, std::min<int>(size, cursor_position()));
    auto main_decorator = flex | ftxui::size(HEIGHT, EQUAL, 1);
    const bool is_focused = Focused();

    // placeholder.
    if (size == 0) {
      auto element = text(*placeholder_) | dim | main_decorator | reflect(box_);
      if (is_focused) {
        element |= focus;
      }
      if (hovered_ || is_focused) {
        element |= inverted;
      }
      return element;
    }

    // Not focused.
    if (!is_focused) {
      auto element = text(content) | main_decorator | reflect(box_);
      if (hovered_) {
        element |= inverted;
      }
      return element;
    }

    const int index_before_cursor = GlyphPosition(content, cursor_position());
    const int index_after_cursor =
        GlyphPosition(content, 1, index_before_cursor);
    const std::string part_before_cursor =
        content.substr(0, index_before_cursor);
    std::string part_at_cursor = " ";
    if (cursor_position() < size) {
      part_at_cursor = content.substr(index_before_cursor,
                                      index_after_cursor - index_before_cursor);
    }
    const std::string part_after_cursor = content.substr(index_after_cursor);
    auto focused = (is_focused || hovered_) ? focusCursorBarBlinking : select;
    return hbox({
               text(part_before_cursor),
               text(part_at_cursor) | focused | reflect(cursor_box_),
               text(part_after_cursor),
           }) |
           flex | frame | bold | main_decorator | reflect(box_);
  }

  bool OnEvent(Event event) override {
    cursor_position() =
        std::max(0, std::min<int>((int)content_->size(), cursor_position()));

    if (event.is_mouse()) {
      return OnMouseEvent(event);
    }

    // Backspace.
    if (event == Event::Backspace) {
      if (cursor_position() == 0) {
        return false;
      }
      const size_t start = GlyphPosition(*content_, cursor_position() - 1);
      const size_t end = GlyphPosition(*content_, cursor_position());
      content_->erase(start, end - start);
      cursor_position()--;
      option_->on_change();
      return true;
    }

    // Delete
    if (event == Event::Delete) {
      if (cursor_position() == int(content_->size())) {
        return false;
      }
      const size_t start = GlyphPosition(*content_, cursor_position());
      const size_t end = GlyphPosition(*content_, cursor_position() + 1);
      content_->erase(start, end - start);
      option_->on_change();
      return true;
    }

    // Enter.
    if (event == Event::Return) {
      option_->on_enter();
      return true;
    }

    if (event == Event::Custom) {
      return false;
    }

    // Arrow
    if (event == Event::ArrowLeft && cursor_position() > 0) {
      cursor_position()--;
      return true;
    }

    if (event == Event::ArrowRight &&
        cursor_position() < (int)content_->size()) {
      cursor_position()++;
      return true;
    }

    // CTRL + Arrow:
    if (event == Event::ArrowLeftCtrl) {
      HandleLeftCtrl();
      return true;
    }
    if (event == Event::ArrowRightCtrl) {
      HandleRightCtrl();
      return true;
    }

    if (event == Event::Home) {
      cursor_position() = 0;
      return true;
    }

    if (event == Event::End) {
      cursor_position() = GlyphCount(*content_);
      return true;
    }

    // Content
    if (event.is_character()) {
      const size_t start = GlyphPosition(*content_, cursor_position());
      content_->insert(start, event.character());
      cursor_position()++;
      option_->on_change();
      return true;
    }
    return false;
  }

 private:
  void HandleLeftCtrl() {
    auto properties = Utf8ToWordBreakProperty(*content_);

    // Move left, as long as left is not a word character.
    while (cursor_position() > 0 &&
           !IsWordCharacter(properties[cursor_position() - 1])) {
      cursor_position()--;
    }

    // Move left, as long as left is a word character:
    while (cursor_position() > 0 &&
           IsWordCharacter(properties[cursor_position() - 1])) {
      cursor_position()--;
    }
  }

  void HandleRightCtrl() {
    auto properties = Utf8ToWordBreakProperty(*content_);
    const int max = (int)properties.size();

    // Move right, as long as right is not a word character.
    while (cursor_position() < max &&
           !IsWordCharacter(properties[cursor_position()])) {
      cursor_position()++;
    }

    // Move right, as long as right is a word character:
    while (cursor_position() < max &&
           IsWordCharacter(properties[cursor_position()])) {
      cursor_position()++;
    }
  }

  bool OnMouseEvent(Event event) {
    hovered_ =
        box_.Contain(event.mouse().x, event.mouse().y) && CaptureMouse(event);
    if (!hovered_) {
      return false;
    }

    if (event.mouse().button != Mouse::Left ||
        event.mouse().motion != Mouse::Pressed) {
      return false;
    }

    TakeFocus();
    if (content_->empty()) {
      return true;
    }

    auto mapping = CellToGlyphIndex(*content_);
    int original_glyph = cursor_position();
    original_glyph = util::clamp(original_glyph, 0, int(mapping.size()));
    size_t original_cell = 0;
    for (size_t i = 0; i < mapping.size(); i++) {
      if (mapping[i] == original_glyph) {
        original_cell = (int)i;
        break;
      }
    }
    if (mapping[original_cell] != original_glyph) {
      original_cell = mapping.size();
    }
    const int target_cell =
        int(original_cell) + event.mouse().x - cursor_box_.x_min;
    int target_glyph = target_cell < (int)mapping.size() ? mapping[target_cell]
                                                         : (int)mapping.size();
    target_glyph = util::clamp(target_glyph, 0, GlyphCount(*content_));
    if (cursor_position() != target_glyph) {
      cursor_position() = target_glyph;
      option_->on_change();
    }
    return true;
  }

  bool Focusable() const final { return true; }

  bool hovered_ = false;
  StringRef content_;
  ConstStringRef placeholder_;

  Box box_;
  Box cursor_box_;
  Ref<InputOption> option_;
};

}  // namespace

/// @brief An input box for editing text.
/// @param content The editable content.
/// @param placeholder The text displayed when content is still empty.
/// @param option Additional optional parameters.
/// @ingroup component
/// @see InputBase
///
/// ### Example
///
/// ```cpp
/// auto screen = ScreenInteractive::FitComponent();
/// std::string content= "";
/// std::string placeholder = "placeholder";
/// Component input = Input(&content, &placeholder);
/// screen.Loop(input);
/// ```
///
/// ### Output
///
/// ```bash
/// placeholder
/// ```
Component Input(StringRef content,
                ConstStringRef placeholder,
                Ref<InputOption> option) {
  return Make<InputBase>(std::move(content), std::move(placeholder),
                         std::move(option));
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#ifndef FTXUI_COMPONENT_LOOP_HPP
#define FTXUI_COMPONENT_LOOP_HPP

#include <memory>  // for shared_ptr

namespace ftxui {
class ComponentBase;

using Component = std::shared_ptr<ComponentBase>;
class ScreenInteractive;

class Loop {
 public:
  Loop(ScreenInteractive* screen, Component component);
  ~Loop();

  bool HasQuitted();
  void RunOnce();
  void RunOnceBlocking();
  void Run();

 private:
  // This class is non copyable.
  Loop(const ScreenInteractive&) = delete;
  Loop& operator=(const Loop&) = delete;

  ScreenInteractive* screen_;
  Component component_;
};

}  // namespace ftxui

#endif  // FTXUI_COMPONENT_LOOP_HPP

// Copyright 2022 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

#include <utility>  // for move

namespace ftxui {

// NOLINTNEXTLINE
Loop::Loop(ScreenInteractive* screen, Component component)
    : screen_(screen), component_(std::move(component)) {
  screen_->PreMain();
}

Loop::~Loop() {
  screen_->PostMain();
}

bool Loop::HasQuitted() {
  return screen_->HasQuitted();
}

/// @brief Execute the loop. Make the `component` to process every pending
/// tasks/events. A new frame might be drawn if the previous was invalidated.
/// Return true until the loop hasn't completed.
void Loop::RunOnce() {
  screen_->RunOnce(component_);
}

/// @brief Wait for at least one event to be handled and execute
/// `Loop::RunOnce()`.
void Loop::RunOnceBlocking() {
  screen_->RunOnceBlocking(component_);
}

/// Execute the loop, blocking the current thread, up until the loop has
/// quitted.
void Loop::Run() {
  while (!HasQuitted()) {
    RunOnceBlocking();
  }
}

}  // namespace ftxui

// Copyright 2022 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <functional>  // for function
#include <memory>  // for make_unique, __shared_ptr_access, __shared_ptr_access<>::element_type, shared_ptr
#include <type_traits>  // for remove_reference, remove_reference<>::type
#include <utility>      // for move

namespace ftxui {

Component Maybe(Component child, std::function<bool()> show) {
  class Impl : public ComponentBase {
   public:
    explicit Impl(std::function<bool()> show) : show_(std::move(show)) {}

   private:
    Element Render() override {
      return show_() ? ComponentBase::Render() : ftxui::make_unique<Node>();
    }
    bool Focusable() const override {
      return show_() && ComponentBase::Focusable();
    }
    bool OnEvent(Event event) override {
      return show_() && ComponentBase::OnEvent(event);
    }

    std::function<bool()> show_;
  };

  auto maybe = Make<Impl>(std::move(show));
  maybe->Add(std::move(child));
  return maybe;
}

/// @brief Decorate a component. It is shown only when the |show| function
/// returns true.
/// @params show a function returning whether the decoratorated component should
/// be shown.
/// @ingroup component
///
/// ### Example
///
/// ```cpp
/// auto component = Renderer([]{ return text("Hello World!"); });
/// auto maybe_component = component | Maybe([&]{ return counter == 42; });
/// ```
ComponentDecorator Maybe(std::function<bool()> show) {
  return [show = std::move(show)](Component child) mutable {
    return Maybe(std::move(child), std::move(show));
  };
}

/// @brief Decorate a component |child|. It is shown only when |show| is true.
/// @params child the compoennt to decorate.
/// @params show a boolean. |child| is shown when |show| is true.
/// @ingroup component
///
/// ### Example
///
/// ```cpp
/// auto component = Renderer([]{ return text("Hello World!"); });
/// auto maybe_component = Maybe(component, &show);
/// ```
Component Maybe(Component child, const bool* show) {
  return Maybe(std::move(child), [show] { return *show; });
}

/// @brief Decorate a component. It is shown only when |show| is true.
/// @params show a boolean. |child| is shown when |show| is true.
/// @ingroup component
///
/// ### Example
///
/// ```cpp
/// auto component = Renderer([]{ return text("Hello World!"); });
/// auto maybe_component = component | Maybe(&show);
/// ```
ComponentDecorator Maybe(const bool* show) {
  return [show](Component child) { return Maybe(std::move(child), show); };
}

}  // namespace ftxui

// Copyright 2021 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <algorithm>   // for max, fill_n, reverse
#include <chrono>      // for milliseconds
#include <functional>  // for function
#include <memory>      // for allocator_traits<>::value_type, swap
#include <string>      // for operator+, string
#include <utility>     // for move
#include <vector>      // for vector, __alloc_traits<>::value_type

namespace ftxui {

namespace {

Element DefaultOptionTransform(const EntryState& state) {
  std::string label = (state.active ? "> " : "  ") + state.label;  // NOLINT
  Element e = text(label);
  if (state.focused) {
    e = e | inverted;
  }
  if (state.active) {
    e = e | bold;
  }
  return e;
}

bool IsInverted(MenuOption::Direction direction) {
  switch (direction) {
    case MenuOption::Direction::Up:
    case MenuOption::Direction::Left:
      return true;
    case MenuOption::Direction::Down:
    case MenuOption::Direction::Right:
      return false;
  }
  return false;  // NOT_REACHED()
}

bool IsHorizontal(MenuOption::Direction direction) {
  switch (direction) {
    case MenuOption::Direction::Left:
    case MenuOption::Direction::Right:
      return true;
    case MenuOption::Direction::Down:
    case MenuOption::Direction::Up:
      return false;
  }
  return false;  // NOT_REACHED()
}

}  // namespace

/// @brief A list of items. The user can navigate through them.
/// @ingroup component
class MenuBase : public ComponentBase {
 public:
  MenuBase(ConstStringListRef entries, int* selected, Ref<MenuOption> option)
      : entries_(entries), selected_(selected), option_(std::move(option)) {}

  bool IsHorizontal() { return ftxui::IsHorizontal(option_->direction); }
  void OnChange() {
    if (option_->on_change) {
      option_->on_change();
    }
  }

  void OnEnter() {
    if (option_->on_enter) {
      option_->on_enter();
    }
  }

  void Clamp() {
    if (*selected_ != selected_previous_) {
      SelectedTakeFocus();
    }
    boxes_.resize(size());
    *selected_ = util::clamp(*selected_, 0, size() - 1);
    selected_previous_ = util::clamp(selected_previous_, 0, size() - 1);
    selected_focus_ = util::clamp(selected_focus_, 0, size() - 1);
    focused_entry() = util::clamp(focused_entry(), 0, size() - 1);
  }

  void OnAnimation(animation::Params& params) override {
    animator_first_.OnAnimation(params);
    animator_second_.OnAnimation(params);
    for (auto& animator : animator_background_) {
      animator.OnAnimation(params);
    }
    for (auto& animator : animator_foreground_) {
      animator.OnAnimation(params);
    }
  }

  Element Render() override {
    Clamp();
    UpdateAnimationTarget();

    Elements elements;
    const bool is_menu_focused = Focused();
    if (option_->elements_prefix) {
      elements.push_back(option_->elements_prefix());
    }
    for (int i = 0; i < size(); ++i) {
      if (i != 0 && option_->elements_infix) {
        elements.push_back(option_->elements_infix());
      }
      const bool is_focused = (focused_entry() == i) && is_menu_focused;
      const bool is_selected = (*selected_ == i);

      const EntryState state = {
          entries_[i],
          false,
          is_selected,
          is_focused,
      };

      auto focus_management =
          is_menu_focused && (selected_focus_ == i) ? focus : nothing;

      const Element element =
          (option_->entries.transform ? option_->entries.transform
                                      : DefaultOptionTransform)  //
          (state);
      elements.push_back(element | AnimatedColorStyle(i) | reflect(boxes_[i]) |
                         focus_management);
    }
    if (option_->elements_postfix) {
      elements.push_back(option_->elements_postfix());
    }

    if (IsInverted(option_->direction)) {
      std::reverse(elements.begin(), elements.end());
    }

    const Element bar =
        IsHorizontal() ? hbox(std::move(elements)) : vbox(std::move(elements));

    if (!option_->underline.enabled) {
      return bar | reflect(box_);
    }

    if (IsHorizontal()) {
      return vbox({
                 bar | xflex,
                 separatorHSelector(first_, second_,  //
                                    option_->underline.color_active,
                                    option_->underline.color_inactive),
             }) |
             reflect(box_);
    } else {
      return hbox({
                 separatorVSelector(first_, second_,  //
                                    option_->underline.color_active,
                                    option_->underline.color_inactive),
                 bar | yflex,
             }) |
             reflect(box_);
    }
  }

  void SelectedTakeFocus() {
    selected_previous_ = *selected_;
    selected_focus_ = *selected_;
  }

  void OnUp() {
    switch (option_->direction) {
      case MenuOption::Direction::Up:
        (*selected_)++;
        break;
      case MenuOption::Direction::Down:
        (*selected_)--;
        break;
      case MenuOption::Direction::Left:
      case MenuOption::Direction::Right:
        break;
    }
  }

  void OnDown() {
    switch (option_->direction) {
      case MenuOption::Direction::Up:
        (*selected_)--;
        break;
      case MenuOption::Direction::Down:
        (*selected_)++;
        break;
      case MenuOption::Direction::Left:
      case MenuOption::Direction::Right:
        break;
    }
  }

  void OnLeft() {
    switch (option_->direction) {
      case MenuOption::Direction::Left:
        (*selected_)++;
        break;
      case MenuOption::Direction::Right:
        (*selected_)--;
        break;
      case MenuOption::Direction::Down:
      case MenuOption::Direction::Up:
        break;
    }
  }

  void OnRight() {
    switch (option_->direction) {
      case MenuOption::Direction::Left:
        (*selected_)--;
        break;
      case MenuOption::Direction::Right:
        (*selected_)++;
        break;
      case MenuOption::Direction::Down:
      case MenuOption::Direction::Up:
        break;
    }
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  bool OnEvent(Event event) override {
    Clamp();
    if (!CaptureMouse(event)) {
      return false;
    }

    if (event.is_mouse()) {
      return OnMouseEvent(event);
    }

    if (Focused()) {
      const int old_selected = *selected_;
      if (event == Event::ArrowUp || event == Event::Character('k')) {
        OnUp();
      }
      if (event == Event::ArrowDown || event == Event::Character('j')) {
        OnDown();
      }
      if (event == Event::ArrowLeft || event == Event::Character('h')) {
        OnLeft();
      }
      if (event == Event::ArrowRight || event == Event::Character('l')) {
        OnRight();
      }
      if (event == Event::PageUp) {
        (*selected_) -= box_.y_max - box_.y_min;
      }
      if (event == Event::PageDown) {
        (*selected_) += box_.y_max - box_.y_min;
      }
      if (event == Event::Home) {
        (*selected_) = 0;
      }
      if (event == Event::End) {
        (*selected_) = size() - 1;
      }
      if (event == Event::Tab && size()) {
        *selected_ = (*selected_ + 1) % size();
      }
      if (event == Event::TabReverse && size()) {
        *selected_ = (*selected_ + size() - 1) % size();
      }

      *selected_ = util::clamp(*selected_, 0, size() - 1);

      if (*selected_ != old_selected) {
        focused_entry() = *selected_;
        SelectedTakeFocus();
        OnChange();
        return true;
      }
    }

    if (event == Event::Return) {
      OnEnter();
      return true;
    }

    return false;
  }

  bool OnMouseEvent(Event event) {
    if (event.mouse().button == Mouse::WheelDown ||
        event.mouse().button == Mouse::WheelUp) {
      return OnMouseWheel(event);
    }

    if (event.mouse().button != Mouse::None &&
        event.mouse().button != Mouse::Left) {
      return false;
    }
    if (!CaptureMouse(event)) {
      return false;
    }
    for (int i = 0; i < size(); ++i) {
      if (!boxes_[i].Contain(event.mouse().x, event.mouse().y)) {
        continue;
      }

      TakeFocus();
      focused_entry() = i;
      if (event.mouse().button == Mouse::Left &&
          event.mouse().motion == Mouse::Released) {
        if (*selected_ != i) {
          *selected_ = i;
          selected_previous_ = *selected_;
          OnChange();
        }
        return true;
      }
    }
    return false;
  }

  bool OnMouseWheel(Event event) {
    if (!box_.Contain(event.mouse().x, event.mouse().y)) {
      return false;
    }
    const int old_selected = *selected_;

    if (event.mouse().button == Mouse::WheelUp) {
      (*selected_)--;
    }
    if (event.mouse().button == Mouse::WheelDown) {
      (*selected_)++;
    }

    *selected_ = util::clamp(*selected_, 0, size() - 1);

    if (*selected_ != old_selected) {
      SelectedTakeFocus();
      OnChange();
    }
    return true;
  }

  void UpdateAnimationTarget() {
    UpdateColorTarget();
    UpdateUnderlineTarget();
  }

  void UpdateColorTarget() {
    if (size() != (int)animation_background_.size()) {
      animation_background_.resize(size());
      animation_foreground_.resize(size());
      animator_background_.clear();
      animator_foreground_.clear();

      for (int i = 0; i < size(); ++i) {
        animation_background_[i] = 0.F;
        animation_foreground_[i] = 0.F;
        animator_background_.emplace_back(&animation_background_[i], 0.F,
                                          std::chrono::milliseconds(0),
                                          animation::easing::Linear);
        animator_foreground_.emplace_back(&animation_foreground_[i], 0.F,
                                          std::chrono::milliseconds(0),
                                          animation::easing::Linear);
      }
    }

    const bool is_menu_focused = Focused();
    for (int i = 0; i < size(); ++i) {
      const bool is_focused = (focused_entry() == i) && is_menu_focused;
      const bool is_selected = (*selected_ == i);
      float target = is_selected ? 1.F : is_focused ? 0.5F : 0.F;  // NOLINT
      if (animator_background_[i].to() != target) {
        animator_background_[i] = animation::Animator(
            &animation_background_[i], target,
            option_->entries.animated_colors.background.duration,
            option_->entries.animated_colors.background.function);
        animator_foreground_[i] = animation::Animator(
            &animation_foreground_[i], target,
            option_->entries.animated_colors.foreground.duration,
            option_->entries.animated_colors.foreground.function);
      }
    }
  }

  Decorator AnimatedColorStyle(int i) {
    Decorator style = nothing;
    if (option_->entries.animated_colors.foreground.enabled) {
      style = style | color(Color::Interpolate(
                          animation_foreground_[i],
                          option_->entries.animated_colors.foreground.inactive,
                          option_->entries.animated_colors.foreground.active));
    }

    if (option_->entries.animated_colors.background.enabled) {
      style = style | bgcolor(Color::Interpolate(
                          animation_background_[i],
                          option_->entries.animated_colors.background.inactive,
                          option_->entries.animated_colors.background.active));
    }
    return style;
  }

  void UpdateUnderlineTarget() {
    if (!option_->underline.enabled) {
      return;
    }

    if (FirstTarget() == animator_first_.to() &&
        SecondTarget() == animator_second_.to()) {
      return;
    }

    if (FirstTarget() >= animator_first_.to()) {
      animator_first_ = animation::Animator(
          &first_, FirstTarget(), option_->underline.follower_duration,
          option_->underline.follower_function,
          option_->underline.follower_delay);

      animator_second_ = animation::Animator(
          &second_, SecondTarget(), option_->underline.leader_duration,
          option_->underline.leader_function, option_->underline.leader_delay);
    } else {
      animator_first_ = animation::Animator(
          &first_, FirstTarget(), option_->underline.leader_duration,
          option_->underline.leader_function, option_->underline.leader_delay);

      animator_second_ = animation::Animator(
          &second_, SecondTarget(), option_->underline.follower_duration,
          option_->underline.follower_function,
          option_->underline.follower_delay);
    }
  }

  bool Focusable() const final { return entries_.size(); }
  int& focused_entry() { return option_->focused_entry(); }
  int size() const { return int(entries_.size()); }
  float FirstTarget() {
    if (boxes_.empty()) {
      return 0.F;
    }
    const int value = IsHorizontal() ? boxes_[*selected_].x_min - box_.x_min
                                     : boxes_[*selected_].y_min - box_.y_min;
    return float(value);
  }
  float SecondTarget() {
    if (boxes_.empty()) {
      return 0.F;
    }
    const int value = IsHorizontal() ? boxes_[*selected_].x_max - box_.x_min
                                     : boxes_[*selected_].y_max - box_.y_min;
    return float(value);
  }

 protected:
  ConstStringListRef entries_;
  int* selected_;
  int selected_previous_ = *selected_;
  int selected_focus_ = *selected_;
  Ref<MenuOption> option_;

  std::vector<Box> boxes_;
  Box box_;

  float first_ = 0.F;
  float second_ = 0.F;
  animation::Animator animator_first_ = animation::Animator(&first_, 0.F);
  animation::Animator animator_second_ = animation::Animator(&second_, 0.F);

  std::vector<animation::Animator> animator_background_;
  std::vector<animation::Animator> animator_foreground_;
  std::vector<float> animation_background_;
  std::vector<float> animation_foreground_;
};

/// @brief A list of text. The focused element is selected.
/// @param entries The list of entries in the menu.
/// @param selected The index of the currently selected element.
/// @param option Additional optional parameters.
/// @ingroup component
///
/// ### Example
///
/// ```cpp
/// auto screen = ScreenInteractive::TerminalOutput();
/// std::vector<std::string> entries = {
///     "entry 1",
///     "entry 2",
///     "entry 3",
/// };
/// int selected = 0;
/// auto menu = Menu(&entries, &selected);
/// screen.Loop(menu);
/// ```
///
/// ### Output
///
/// ```bash
/// > entry 1
///   entry 2
///   entry 3
/// ```
Component Menu(ConstStringListRef entries,
               int* selected,
               Ref<MenuOption> option) {
  return Make<MenuBase>(entries, selected, std::move(option));
}

/// @brief An horizontal list of elements. The user can navigate through them.
/// @param entries The list of selectable entries to display.
/// @param selected Reference the selected entry.
/// @param See also |Menu|.
/// @ingroup component
Component Toggle(ConstStringListRef entries, int* selected) {
  return Menu(entries, selected, MenuOption::Toggle());
}

/// @brief A specific menu entry. They can be put into a Container::Vertical to
/// form a menu.
/// @param label The text drawn representing this element.
/// @param option Additional optional parameters.
/// @ingroup component
///
/// ### Example
///
/// ```cpp
/// auto screen = ScreenInteractive::TerminalOutput();
/// int selected = 0;
/// auto menu = Container::Vertical({
///    MenuEntry("entry 1"),
///    MenuEntry("entry 2"),
///    MenuEntry("entry 3"),
/// }, &selected);
/// screen.Loop(menu);
/// ```
///
/// ### Output
///
/// ```bash
/// > entry 1
///   entry 2
///   entry 3
/// ```
Component MenuEntry(ConstStringRef label, Ref<MenuEntryOption> option) {
  class Impl : public ComponentBase {
   public:
    Impl(ConstStringRef label, Ref<MenuEntryOption> option)
        : label_(std::move(label)), option_(std::move(option)) {}

   private:
    Element Render() override {
      const bool focused = Focused();
      UpdateAnimationTarget();

      const EntryState state = {
          *label_,
          false,
          hovered_,
          focused,
      };

      const Element element =
          (option_->transform ? option_->transform : DefaultOptionTransform)  //
          (state);

      auto focus_management = focused ? select : nothing;
      return element | AnimatedColorStyle() | focus_management | reflect(box_);
    }

    void UpdateAnimationTarget() {
      const bool focused = Focused();
      float target = focused ? 1.F : hovered_ ? 0.5F : 0.F;  // NOLINT
      if (target == animator_background_.to()) {
        return;
      }
      animator_background_ =
          animation::Animator(&animation_background_, target,
                              option_->animated_colors.background.duration,
                              option_->animated_colors.background.function);
      animator_foreground_ =
          animation::Animator(&animation_foreground_, target,
                              option_->animated_colors.foreground.duration,
                              option_->animated_colors.foreground.function);
    }

    Decorator AnimatedColorStyle() {
      Decorator style = nothing;
      if (option_->animated_colors.foreground.enabled) {
        style = style | color(Color::Interpolate(
                            animation_foreground_,
                            option_->animated_colors.foreground.inactive,
                            option_->animated_colors.foreground.active));
      }

      if (option_->animated_colors.background.enabled) {
        style = style | bgcolor(Color::Interpolate(
                            animation_background_,
                            option_->animated_colors.background.inactive,
                            option_->animated_colors.background.active));
      }
      return style;
    }

    bool Focusable() const override { return true; }
    bool OnEvent(Event event) override {
      if (!event.is_mouse()) {
        return false;
      }

      hovered_ = box_.Contain(event.mouse().x, event.mouse().y);

      if (!hovered_) {
        return false;
      }

      if (event.mouse().button == Mouse::Left &&
          event.mouse().motion == Mouse::Released) {
        TakeFocus();
        return true;
      }

      return false;
    }

    void OnAnimation(animation::Params& params) override {
      animator_background_.OnAnimation(params);
      animator_foreground_.OnAnimation(params);
    }

    ConstStringRef label_;
    Ref<MenuEntryOption> option_;
    Box box_;
    bool hovered_ = false;

    float animation_background_ = 0.F;
    float animation_foreground_ = 0.F;
    animation::Animator animator_background_ =
        animation::Animator(&animation_background_, 0.F);
    animation::Animator animator_foreground_ =
        animation::Animator(&animation_foreground_, 0.F);
  };

  return Make<Impl>(std::move(label), std::move(option));
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <memory>                  // for __shared_ptr_access, shared_ptr
#include <utility>                 // for move

namespace ftxui {

// Add a |modal| window on top of the |main| component. It is shown one on the
// top of the other when |show_modal| is true.
/// @ingroup component
// NOLINTNEXTLINE
Component Modal(Component main, Component modal, const bool* show_modal) {
  class Impl : public ComponentBase {
   public:
    explicit Impl(Component main, Component modal, const bool* show_modal)
        : main_(std::move(main)),
          modal_(std::move(modal)),
          show_modal_(show_modal) {
      Add(Container::Tab({main_, modal_}, &selector_));
    }

   private:
    Element Render() override {
      selector_ = *show_modal_;
      auto document = main_->Render();
      if (*show_modal_) {
        document = dbox({
            document,
            modal_->Render() | clear_under | center,
        });
      }
      return document;
    }

    bool OnEvent(Event event) override {
      selector_ = *show_modal_;
      return ComponentBase::OnEvent(event);
    }

    Component main_;
    Component modal_;
    const bool* show_modal_;
    int selector_ = *show_modal_;
  };
  return Make<Impl>(main, modal, show_modal);
}

// Decorate a component. Add a |modal| window on top of it. It is shown one on
// the top of the other when |show_modal| is true.
/// @ingroup component
// NOLINTNEXTLINE
ComponentDecorator Modal(Component modal, const bool* show_modal) {
  return [modal, show_modal](Component main) {
    return Modal(std::move(main), modal, show_modal);
  };
}

}  // namespace ftxui

// Copyright 2022 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <functional>  // for function
#include <memory>      // for allocator_traits<>::value_type
#include <utility>     // for move
#include <vector>      // for vector

namespace ftxui {

namespace {
/// @brief A list of selectable element. One and only one can be selected at
/// the same time.
/// @ingroup component
class RadioboxBase : public ComponentBase {
 public:
  RadioboxBase(ConstStringListRef entries,
               int* selected,
               Ref<RadioboxOption> option)
      : entries_(entries), selected_(selected), option_(std::move(option)) {}

 private:
  Element Render() override {
    Clamp();
    Elements elements;
    const bool is_menu_focused = Focused();
    for (int i = 0; i < size(); ++i) {
      const bool is_focused = (focused_entry() == i) && is_menu_focused;
      const bool is_selected = (hovered_ == i);
      auto focus_management = !is_selected      ? nothing
                              : is_menu_focused ? focus
                                                : select;
      auto state = EntryState{
          entries_[i],
          *selected_ == i,
          is_selected,
          is_focused,
      };
      auto element =
          (option_->transform ? option_->transform
                              : RadioboxOption::Simple().transform)(state);

      elements.push_back(element | focus_management | reflect(boxes_[i]));
    }
    return vbox(std::move(elements)) | reflect(box_);
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  bool OnEvent(Event event) override {
    Clamp();
    if (!CaptureMouse(event)) {
      return false;
    }

    if (event.is_mouse()) {
      return OnMouseEvent(event);
    }

    if (Focused()) {
      const int old_hovered = hovered_;
      if (event == Event::ArrowUp || event == Event::Character('k')) {
        (hovered_)--;
      }
      if (event == Event::ArrowDown || event == Event::Character('j')) {
        (hovered_)++;
      }
      if (event == Event::PageUp) {
        (hovered_) -= box_.y_max - box_.y_min;
      }
      if (event == Event::PageDown) {
        (hovered_) += box_.y_max - box_.y_min;
      }
      if (event == Event::Home) {
        (hovered_) = 0;
      }
      if (event == Event::End) {
        (hovered_) = size() - 1;
      }
      if (event == Event::Tab && size()) {
        hovered_ = (hovered_ + 1) % size();
      }
      if (event == Event::TabReverse && size()) {
        hovered_ = (hovered_ + size() - 1) % size();
      }

      hovered_ = util::clamp(hovered_, 0, size() - 1);

      if (hovered_ != old_hovered) {
        focused_entry() = hovered_;
        option_->on_change();
        return true;
      }
    }

    if (event == Event::Character(' ') || event == Event::Return) {
      *selected_ = hovered_;
      option_->on_change();
      return true;
    }

    return false;
  }

  bool OnMouseEvent(Event event) {
    if (event.mouse().button == Mouse::WheelDown ||
        event.mouse().button == Mouse::WheelUp) {
      return OnMouseWheel(event);
    }

    for (int i = 0; i < size(); ++i) {
      if (!boxes_[i].Contain(event.mouse().x, event.mouse().y)) {
        continue;
      }

      TakeFocus();
      focused_entry() = i;
      if (event.mouse().button == Mouse::Left &&
          event.mouse().motion == Mouse::Released) {
        if (*selected_ != i) {
          *selected_ = i;
          option_->on_change();
        }

        return true;
      }
    }
    return false;
  }

  bool OnMouseWheel(Event event) {
    if (!box_.Contain(event.mouse().x, event.mouse().y)) {
      return false;
    }

    const int old_hovered = hovered_;

    if (event.mouse().button == Mouse::WheelUp) {
      (hovered_)--;
    }
    if (event.mouse().button == Mouse::WheelDown) {
      (hovered_)++;
    }

    hovered_ = util::clamp(hovered_, 0, size() - 1);

    if (hovered_ != old_hovered) {
      option_->on_change();
    }

    return true;
  }

  void Clamp() {
    boxes_.resize(size());
    *selected_ = util::clamp(*selected_, 0, size() - 1);
    focused_entry() = util::clamp(focused_entry(), 0, size() - 1);
    hovered_ = util::clamp(hovered_, 0, size() - 1);
  }

  bool Focusable() const final { return entries_.size(); }
  int& focused_entry() { return option_->focused_entry(); }
  int size() const { return int(entries_.size()); }

  ConstStringListRef entries_;
  int* selected_;
  int hovered_ = *selected_;
  std::vector<Box> boxes_;
  Box box_;
  Ref<RadioboxOption> option_;
};

}  // namespace

/// @brief A list of element, where only one can be selected.
/// @param entries The list of entries in the list.
/// @param selected The index of the currently selected element.
/// @param option Additional optional parameters.
/// @ingroup component
/// @see RadioboxBase
///
/// ### Example
///
/// ```cpp
/// auto screen = ScreenInteractive::TerminalOutput();
/// std::vector<std::string> entries = {
///     "entry 1",
///     "entry 2",
///     "entry 3",
/// };
/// int selected = 0;
/// auto menu = Radiobox(&entries, &selected);
/// screen.Loop(menu);
/// ```
///
/// ### Output
///
/// ```bash
/// ◉ entry 1
/// ○ entry 2
/// ○ entry 3
/// ```
Component Radiobox(ConstStringListRef entries,
                   int* selected,
                   Ref<RadioboxOption> option) {
  return Make<RadioboxBase>(entries, selected, std::move(option));
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <functional>  // for function
#include <memory>      // for __shared_ptr_access, shared_ptr
#include <utility>     // for move

namespace ftxui {

/// @brief Return a component, using |render| to render its interface.
/// @param render The function drawing the interface.
/// @ingroup component
///
/// ### Example
///
/// ```cpp
/// auto screen = ScreenInteractive::TerminalOutput();
/// auto renderer = Renderer([] {
///   return text("My interface");
/// });
/// screen.Loop(renderer);
/// ```
Component Renderer(std::function<Element()> render) {
  class Impl : public ComponentBase {
   public:
    explicit Impl(std::function<Element()> render)
        : render_(std::move(render)) {}
    Element Render() override { return render_(); }
    std::function<Element()> render_;
  };

  return Make<Impl>(std::move(render));
}

/// @brief Return a new Component, similar to |child|, but using |render| as the
/// Component::Render() event.
/// @param child The component to forward events to.
/// @param render The function drawing the interface.
/// @ingroup component
///
/// ### Example
///
/// ```cpp
/// auto screen = ScreenInteractive::TerminalOutput();
/// std::string label = "Click to quit";
/// auto button = Button(&label, screen.ExitLoopClosure());
/// auto renderer = Renderer(button, [&] {
///   return hbox({
///     text("A button:"),
///     button->Render(),
///   });
/// });
/// screen.Loop(renderer);
/// ```
Component Renderer(Component child, std::function<Element()> render) {
  Component renderer = Renderer(std::move(render));
  renderer->Add(std::move(child));
  return renderer;
}

/// @brief Return a focusable component, using |render| to render its interface.
/// @param render The function drawing the interface, taking a boolean telling
/// whether the component is focused or not.
/// @ingroup component
///
/// ### Example
///
/// ```cpp
/// auto screen = ScreenInteractive::TerminalOutput();
/// auto renderer = Renderer([] (bool focused) {
///   if (focused)
///     return text("My interface") | inverted;
///   else
///     return text("My interface");
/// });
/// screen.Loop(renderer);
/// ```
Component Renderer(std::function<Element(bool)> render) {
  class Impl : public ComponentBase {
   public:
    explicit Impl(std::function<Element(bool)> render)
        : render_(std::move(render)) {}

   private:
    Element Render() override { return render_(Focused()) | reflect(box_); }
    bool Focusable() const override { return true; }
    bool OnEvent(Event event) override {
      if (event.is_mouse() && box_.Contain(event.mouse().x, event.mouse().y)) {
        if (!CaptureMouse(event)) {
          return false;
        }

        TakeFocus();
      }

      return false;
    }
    Box box_;

    std::function<Element(bool)> render_;
  };
  return Make<Impl>(std::move(render));
}

/// @brief Decorate a component, by decorating what it renders.
/// @param decorator the function modifying the element it renders.
/// @ingroup component
///
/// ### Example
///
/// ```cpp
/// auto screen = ScreenInteractive::TerminalOutput();
/// auto renderer =
//     Renderer([] { return text("Hello");)
///  | Renderer(bold)
///  | Renderer(inverted);
/// screen.Loop(renderer);
/// ```
ComponentDecorator Renderer(ElementDecorator decorator) {  // NOLINT
  return [decorator](Component component) {                // NOLINT
    return Renderer(component, [component, decorator] {
      return component->Render() | decorator;
    });
  };
}

}  // namespace ftxui

// Copyright 2021 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <memory>   // for __shared_ptr_access
#include <utility>  // for move

namespace ftxui {
namespace {

class ResizableSplitLeftBase : public ComponentBase {
 public:
  ResizableSplitLeftBase(Component main, Component child, int* main_size)
      : main_(std::move(main)),
        child_(std::move(child)),
        main_size_(main_size) {
    Add(Container::Horizontal({
        main_,
        child_,
    }));
  }

  bool OnEvent(Event event) final {
    if (event.is_mouse()) {
      return OnMouseEvent(std::move(event));
    }
    return ComponentBase::OnEvent(std::move(event));
  }

  bool OnMouseEvent(Event event) {
    if (captured_mouse_ && event.mouse().motion == Mouse::Released) {
      captured_mouse_.reset();
      return true;
    }

    if (event.mouse().button == Mouse::Left &&
        event.mouse().motion == Mouse::Pressed &&
        separator_box_.Contain(event.mouse().x, event.mouse().y) &&
        !captured_mouse_) {
      captured_mouse_ = CaptureMouse(event);
      return true;
    }

    if (captured_mouse_) {
      *main_size_ = event.mouse().x - box_.x_min;
      return true;
    }

    return ComponentBase::OnEvent(event);
  }

  Element Render() final {
    return hbox({
               main_->Render() | size(WIDTH, EQUAL, *main_size_),
               separator() | reflect(separator_box_),
               child_->Render() | xflex,
           }) |
           reflect(box_);
  };

 private:
  Component main_;
  Component child_;
  int* const main_size_;
  CapturedMouse captured_mouse_;
  Box separator_box_;
  Box box_;
};

class ResizableSplitRightBase : public ComponentBase {
 public:
  ResizableSplitRightBase(Component main, Component child, int* main_size)
      : main_(std::move(main)),
        child_(std::move(child)),
        main_size_(main_size) {
    Add(Container::Horizontal({
        child_,
        main_,
    }));
  }

  bool OnEvent(Event event) final {
    if (event.is_mouse()) {
      return OnMouseEvent(std::move(event));
    }
    return ComponentBase::OnEvent(std::move(event));
  }

  bool OnMouseEvent(Event event) {
    if (captured_mouse_ && event.mouse().motion == Mouse::Released) {
      captured_mouse_.reset();
      return true;
    }

    if (event.mouse().button == Mouse::Left &&
        event.mouse().motion == Mouse::Pressed &&
        separator_box_.Contain(event.mouse().x, event.mouse().y) &&
        !captured_mouse_) {
      captured_mouse_ = CaptureMouse(event);
      return true;
    }

    if (captured_mouse_) {
      *main_size_ = box_.x_max - event.mouse().x;
      return true;
    }

    return ComponentBase::OnEvent(event);
  }

  Element Render() final {
    return hbox({
               child_->Render() | xflex,
               separator() | reflect(separator_box_),
               main_->Render() | size(WIDTH, EQUAL, *main_size_),
           }) |
           reflect(box_);
  };

 private:
  Component main_;
  Component child_;
  int* const main_size_;
  CapturedMouse captured_mouse_;
  Box separator_box_;
  Box box_;
};

class ResizableSplitTopBase : public ComponentBase {
 public:
  ResizableSplitTopBase(Component main, Component child, int* main_size)
      : main_(std::move(main)),
        child_(std::move(child)),
        main_size_(main_size) {
    Add(Container::Vertical({
        main_,
        child_,
    }));
  }

  bool OnEvent(Event event) final {
    if (event.is_mouse()) {
      return OnMouseEvent(std::move(event));
    }
    return ComponentBase::OnEvent(std::move(event));
  }

  bool OnMouseEvent(Event event) {
    if (captured_mouse_ && event.mouse().motion == Mouse::Released) {
      captured_mouse_.reset();
      return true;
    }

    if (event.mouse().button == Mouse::Left &&
        event.mouse().motion == Mouse::Pressed &&
        separator_box_.Contain(event.mouse().x, event.mouse().y) &&
        !captured_mouse_) {
      captured_mouse_ = CaptureMouse(event);
      return true;
    }

    if (captured_mouse_) {
      *main_size_ = event.mouse().y - box_.y_min;
      return true;
    }

    return ComponentBase::OnEvent(event);
  }

  Element Render() final {
    return vbox({
               main_->Render() | size(HEIGHT, EQUAL, *main_size_),
               separator() | reflect(separator_box_),
               child_->Render() | yflex,
           }) |
           reflect(box_);
  };

 private:
  Component main_;
  Component child_;
  int* const main_size_;
  CapturedMouse captured_mouse_;
  Box separator_box_;
  Box box_;
};

class ResizableSplitBottomBase : public ComponentBase {
 public:
  ResizableSplitBottomBase(Component main, Component child, int* main_size)
      : main_(std::move(main)),
        child_(std::move(child)),
        main_size_(main_size) {
    Add(Container::Vertical({
        child_,
        main_,
    }));
  }

  bool OnEvent(Event event) final {
    if (event.is_mouse()) {
      return OnMouseEvent(std::move(event));
    }
    return ComponentBase::OnEvent(std::move(event));
  }

  bool OnMouseEvent(Event event) {
    if (captured_mouse_ && event.mouse().motion == Mouse::Released) {
      captured_mouse_.reset();
      return true;
    }

    if (event.mouse().button == Mouse::Left &&
        event.mouse().motion == Mouse::Pressed &&
        separator_box_.Contain(event.mouse().x, event.mouse().y) &&
        !captured_mouse_) {
      captured_mouse_ = CaptureMouse(event);
      return true;
    }

    if (captured_mouse_) {
      *main_size_ = box_.y_max - event.mouse().y;
      return true;
    }

    return ComponentBase::OnEvent(event);
  }

  Element Render() final {
    return vbox({
               child_->Render() | yflex,
               separator() | reflect(separator_box_),
               main_->Render() | size(HEIGHT, EQUAL, *main_size_),
           }) |
           reflect(box_);
  };

 private:
  Component main_;
  Component child_;
  int* const main_size_;
  CapturedMouse captured_mouse_;
  Box separator_box_;
  Box box_;
};

}  // namespace

/// @brief An horizontal split in between two components, configurable using the
/// mouse.
/// @param main The main component of size |main_size|, on the left.
/// @param back The back component taking the remaining size, on the right.
/// @param main_size The size of the |main| component.
/// @ingroup component
///
/// ### Example
///
/// ```cpp
/// auto screen = ScreenInteractive::Fullscreen();
/// int left_size = 10;
/// auto left = Renderer([] { return text("Left") | center;});
/// auto right = Renderer([] { return text("right") | center;});
/// auto split = ResizableSplitLeft(left, right, &left_size);
/// screen.Loop(split);
/// ```
///
/// ### Output
///
/// ```bash
///           │
///    left   │   right
///           │
/// ```
Component ResizableSplitLeft(Component main, Component back, int* main_size) {
  return Make<ResizableSplitLeftBase>(std::move(main), std::move(back),
                                      main_size);
}

/// @brief An horizontal split in between two components, configurable using the
/// mouse.
/// @param main The main component of size |main_size|, on the right.
/// @param back The back component taking the remaining size, on the left.
/// @param main_size The size of the |main| component.
/// @ingroup component
///
/// ### Example
///
/// ```cpp
/// auto screen = ScreenInteractive::Fullscreen();
/// int right_size = 10;
/// auto left = Renderer([] { return text("Left") | center;});
/// auto right = Renderer([] { return text("right") | center;});
/// auto split = ResizableSplitRight(right, left, &right_size);
/// screen.Loop(split);
/// ```
///
/// ### Output
///
/// ```bash
///           │
///    left   │   right
///           │
/// ```
Component ResizableSplitRight(Component main, Component back, int* main_size) {
  return Make<ResizableSplitRightBase>(std::move(main), std::move(back),
                                       main_size);
}

/// @brief An vertical split in between two components, configurable using the
/// mouse.
/// @param main The main component of size |main_size|, on the top.
/// @param back The back component taking the remaining size, on the bottom.
/// @param main_size The size of the |main| component.
/// @ingroup component
///
/// ### Example
///
/// ```cpp
/// auto screen = ScreenInteractive::Fullscreen();
/// int top_size = 1;
/// auto top = Renderer([] { return text("Top") | center;});
/// auto bottom = Renderer([] { return text("Bottom") | center;});
/// auto split = ResizableSplitTop(top, bottom, &top_size);
/// screen.Loop(split);
/// ```
///
/// ### Output
///
/// ```bash
///    top
/// ────────────
///    bottom
/// ```
Component ResizableSplitTop(Component main, Component back, int* main_size) {
  return Make<ResizableSplitTopBase>(std::move(main), std::move(back),
                                     main_size);
}

/// @brief An vertical split in between two components, configurable using the
/// mouse.
/// @param main The main component of size |main_size|, on the bottom.
/// @param back The back component taking the remaining size, on the top.
/// @param main_size The size of the |main| component.
/// @ingroup component
///
/// ### Example
///
/// ```cpp
/// auto screen = ScreenInteractive::Fullscreen();
/// int bottom_size = 1;
/// auto top = Renderer([] { return text("Top") | center;});
/// auto bottom = Renderer([] { return text("Bottom") | center;});
/// auto split = ResizableSplit::Bottom(bottom, top, &bottom_size);
/// screen.Loop(split);
/// ```
///
/// ### Output
///
/// ```bash
///    top
/// ────────────
///    bottom
/// ```
Component ResizableSplitBottom(Component main, Component back, int* main_size) {
  return Make<ResizableSplitBottomBase>(std::move(main), std::move(back),
                                        main_size);
}
}  // namespace ftxui

// Copyright 2021 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <algorithm>  // for copy, max, min
#include <array>      // for array
#include <chrono>  // for operator-, milliseconds, operator>=, duration, common_type<>::type, time_point
#include <csignal>  // for signal, SIGTSTP, SIGABRT, SIGWINCH, raise, SIGFPE, SIGILL, SIGINT, SIGSEGV, SIGTERM, __sighandler_t, size_t
#include <cstdio>   // for fileno, stdin
#include <functional>        // for function
#include <initializer_list>  // for initializer_list
#include <iostream>  // for cout, ostream, operator<<, basic_ostream, endl, flush
#include <stack>     // for stack
#include <thread>    // for thread, sleep_for
#include <tuple>     // for _Swallow_assign, ignore
#include <type_traits>  // for decay_t
#include <utility>      // for move, swap
// #include <variant>  // removed for C++11 (Task is a hand-written tagged struct)      // for visit, variant
#include <vector>       // for vector

#ifndef FTXUI_COMPONENT_TERMINAL_INPUT_PARSER
#define FTXUI_COMPONENT_TERMINAL_INPUT_PARSER

#include <memory>  // for unique_ptr
#include <string>  // for string
#include <vector>  // for vector

namespace ftxui {
struct Event;

// Parse a sequence of |char| accross |time|. Produces |Event|.
class TerminalInputParser {
 public:
  TerminalInputParser(Sender<Task> out);
  void Timeout(int time);
  void Add(char c);

 private:
  unsigned char Current();
  bool Eat();

  enum Type {
    UNCOMPLETED,
    DROP,
    CHARACTER,
    SPECIAL,
    MOUSE,
    CURSOR_REPORTING,
  };

  struct CursorReporting {
    int x;
    int y;
  };

  struct Output {
    Type type;
    union {
      Mouse mouse;
      CursorReporting cursor;
    };

    Output(Type t) : type(t) {}
  };

  void Send(Output output);
  Output Parse();
  Output ParseUTF8();
  Output ParseESC();
  Output ParseDCS();
  Output ParseCSI();
  Output ParseOSC();
  Output ParseMouse(bool altered, bool pressed, std::vector<int> arguments);
  Output ParseCursorReporting(std::vector<int> arguments);

  Sender<Task> out_;
  int position_ = -1;
  int timeout_ = 0;
  std::string pending_;
};

}  // namespace ftxui

#endif /* end of include guard: FTXUI_COMPONENT_TERMINAL_INPUT_PARSER */

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

#if defined(_WIN32)
#define DEFINE_CONSOLEV2_PROPERTIES
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef UNICODE
#error Must be compiled in UNICODE mode
#endif
#else
#include <sys/select.h>  // for select, FD_ISSET, FD_SET, FD_ZERO, fd_set, timeval
#include <termios.h>  // for tcsetattr, termios, tcgetattr, TCSANOW, cc_t, ECHO, ICANON, VMIN, VTIME
#include <unistd.h>  // for STDIN_FILENO, read
#endif

// Quick exit is missing in standard CLang headers
#if defined(__clang__) && defined(__APPLE__)
#define quick_exit(a) exit(a)
#endif

namespace ftxui {

namespace animation {
void RequestAnimationFrame() {
  auto* screen = ScreenInteractive::Active();
  if (screen) {
    screen->RequestAnimationFrame();
  }
}
}  // namespace animation

namespace {

ScreenInteractive* g_active_screen = nullptr;  // NOLINT

void Flush() {
  // Emscripten doesn't implement flush. We interpret zero as flush.
  std::cout << '\0' << std::flush;
}

constexpr int timeout_milliseconds = 20;
constexpr int timeout_microseconds = timeout_milliseconds * 1000;
#if defined(_WIN32)

void EventListener(std::atomic<bool>* quit, Sender<Task> out) {
  auto console = GetStdHandle(STD_INPUT_HANDLE);
  auto parser = TerminalInputParser(out->Clone());
  while (!*quit) {
    // Throttle ReadConsoleInput by waiting 250ms, this wait function will
    // return if there is input in the console.
    auto wait_result = WaitForSingleObject(console, timeout_milliseconds);
    if (wait_result == WAIT_TIMEOUT) {
      parser.Timeout(timeout_milliseconds);
      continue;
    }

    DWORD number_of_events = 0;
    if (!GetNumberOfConsoleInputEvents(console, &number_of_events))
      continue;
    if (number_of_events <= 0)
      continue;

    std::vector<INPUT_RECORD> records{number_of_events};
    DWORD number_of_events_read = 0;
    ReadConsoleInput(console, records.data(), (DWORD)records.size(),
                     &number_of_events_read);
    records.resize(number_of_events_read);

    for (const auto& r : records) {
      switch (r.EventType) {
        case KEY_EVENT: {
          auto key_event = r.Event.KeyEvent;
          // ignore UP key events
          if (key_event.bKeyDown == FALSE)
            continue;
          std::wstring wstring;
          wstring += key_event.uChar.UnicodeChar;
          for (auto it : to_string(wstring)) {
            parser.Add(it);
          }
        } break;
        case WINDOW_BUFFER_SIZE_EVENT:
          out->Send(Event::Special({0}));
          break;
        case MENU_EVENT:
        case FOCUS_EVENT:
        case MOUSE_EVENT:
          // TODO(mauve): Implement later.
          break;
      }
    }
  }
}

#elif defined(__EMSCRIPTEN__)
#include <emscripten.h>

// Read char from the terminal.
void EventListener(std::atomic<bool>* quit, Sender<Task> out) {
  (void)timeout_microseconds;
  auto parser = TerminalInputParser(std::move(out));

  char c;
  while (!*quit) {
    while (read(STDIN_FILENO, &c, 1), c)
      parser.Add(c);

    emscripten_sleep(1);
    parser.Timeout(1);
  }
}

extern "C" {
EMSCRIPTEN_KEEPALIVE
void ftxui_on_resize(int columns, int rows) {
  Terminal::SetFallbackSize({
      columns,
      rows,
  });
  std::raise(SIGWINCH);
}
}

#else  // POSIX (Linux & Mac)

int CheckStdinReady(int usec_timeout) {
  timeval tv = {0, usec_timeout};
  fd_set fds;
  FD_ZERO(&fds);                                          // NOLINT
  FD_SET(STDIN_FILENO, &fds);                             // NOLINT
  select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);  // NOLINT
  return FD_ISSET(STDIN_FILENO, &fds);                    // NOLINT
}

// Read char from the terminal.
void EventListener(std::atomic<bool>* quit, Sender<Task> out) {
  auto parser = TerminalInputParser(std::move(out));

  while (!*quit) {
    if (!CheckStdinReady(timeout_microseconds)) {
      parser.Timeout(timeout_milliseconds);
      continue;
    }

    const size_t buffer_size = 100;
    std::array<char, buffer_size> buffer;                        // NOLINT;
    size_t l = read(fileno(stdin), buffer.data(), buffer_size);  // NOLINT
    for (size_t i = 0; i < l; ++i) {
      parser.Add(buffer[i]);  // NOLINT
    }
  }
}
#endif

std::stack<Closure> on_exit_functions;  // NOLINT
void OnExit() {
  while (!on_exit_functions.empty()) {
    on_exit_functions.top()();
    on_exit_functions.pop();
  }
}

std::atomic<int> g_signal_exit_count{0};  // NOLINT
#if !defined(_WIN32)
std::atomic<int> g_signal_stop_count = 0;    // NOLINT
std::atomic<int> g_signal_resize_count = 0;  // NOLINT
#endif

// Async signal safe function
void RecordSignal(int signal) {
  switch (signal) {
    case SIGABRT:
    case SIGFPE:
    case SIGILL:
    case SIGINT:
    case SIGSEGV:
    case SIGTERM:
      g_signal_exit_count++;
      break;

#if !defined(_WIN32)
    case SIGTSTP:
      g_signal_stop_count++;
      break;

    case SIGWINCH:
      g_signal_resize_count++;
      break;
#endif

    default:
      break;
  }
}

void ExecuteSignalHandlers() {
  int signal_exit_count = g_signal_exit_count.exchange(0);
  while (signal_exit_count--) {
    ScreenInteractive::Private::Signal(*g_active_screen, SIGABRT);
  }

#if !defined(_WIN32)
  int signal_stop_count = g_signal_stop_count.exchange(0);
  while (signal_stop_count--) {
    ScreenInteractive::Private::Signal(*g_active_screen, SIGTSTP);
  }

  int signal_resize_count = g_signal_resize_count.exchange(0);
  while (signal_resize_count--) {
    ScreenInteractive::Private::Signal(*g_active_screen, SIGWINCH);
  }
#endif
}

void InstallSignalHandler(int sig) {
  auto old_signal_handler = std::signal(sig, RecordSignal);
  on_exit_functions.push(
      [=] { std::ignore = std::signal(sig, old_signal_handler); });
}

const std::string CSI = "\x1b[";  // NOLINT

// DEC: Digital Equipment Corporation
enum class DECMode {
  kLineWrap = 7,
  kCursor = 25,

  kMouseX10 = 9,
  kMouseVt200 = 1000,
  kMouseVt200Highlight = 1001,

  kMouseBtnEventMouse = 1002,
  kMouseAnyEvent = 1003,

  kMouseUtf8 = 1005,
  kMouseSgrExtMode = 1006,
  kMouseUrxvtMode = 1015,
  kMouseSgrPixelsMode = 1016,
  kAlternateScreen = 1049,
};

// Device Status Report (DSR) {
enum class DSRMode {
  kCursor = 6,
};

std::string Serialize(const std::vector<DECMode>& parameters) {
  bool first = true;
  std::string out;
  for (const DECMode parameter : parameters) {
    if (!first) {
      out += ";";
    }
    out += std::to_string(int(parameter));
    first = false;
  }
  return out;
}

// DEC Private Mode Set (DECSET)
std::string Set(const std::vector<DECMode>& parameters) {
  return CSI + "?" + Serialize(parameters) + "h";
}

// DEC Private Mode Reset (DECRST)
std::string Reset(const std::vector<DECMode>& parameters) {
  return CSI + "?" + Serialize(parameters) + "l";
}

// Device Status Report (DSR)
std::string DeviceStatusReport(DSRMode ps) {
  return CSI + std::to_string(int(ps)) + "n";
}

class CapturedMouseImpl : public CapturedMouseInterface {
 public:
  explicit CapturedMouseImpl(std::function<void(void)> callback)
      : callback_(std::move(callback)) {}
  ~CapturedMouseImpl() override { callback_(); }
  CapturedMouseImpl(const CapturedMouseImpl&) = delete;
  CapturedMouseImpl(CapturedMouseImpl&&) = delete;
  CapturedMouseImpl& operator=(const CapturedMouseImpl&) = delete;
  CapturedMouseImpl& operator=(CapturedMouseImpl&&) = delete;

 private:
  std::function<void(void)> callback_;
};

void AnimationListener(std::atomic<bool>* quit, Sender<Task> out) {
  // Animation at around 60fps.
  const auto time_delta = std::chrono::milliseconds(15);
  while (!*quit) {
    out->Send(AnimationTask());
    std::this_thread::sleep_for(time_delta);
  }
}

}  // namespace

ScreenInteractive::ScreenInteractive(int dimx,
                                     int dimy,
                                     Dimension dimension,
                                     bool use_alternative_screen)
    : Screen(dimx, dimy),
      dimension_(dimension),
      use_alternative_screen_(use_alternative_screen) {
  task_receiver_ = MakeReceiver<Task>();
}

ScreenInteractive::ScreenInteractive(ScreenInteractive&& other) noexcept
    : Screen(std::move(other)),
      suspended_screen_(other.suspended_screen_),
      dimension_(other.dimension_),
      use_alternative_screen_(other.use_alternative_screen_),
      task_sender_(std::move(other.task_sender_)),
      task_receiver_(std::move(other.task_receiver_)),
      set_cursor_position(std::move(other.set_cursor_position)),
      reset_cursor_position(std::move(other.reset_cursor_position)),
      quit_(other.quit_.load()),
      event_listener_(std::move(other.event_listener_)),
      animation_listener_(std::move(other.animation_listener_)),
      animation_requested_(other.animation_requested_),
      previous_animation_time_(other.previous_animation_time_),
      cursor_x_(other.cursor_x_),
      cursor_y_(other.cursor_y_),
      mouse_captured(other.mouse_captured),
      previous_frame_resized_(other.previous_frame_resized_),
      frame_valid_(other.frame_valid_) {}

// static
ScreenInteractive ScreenInteractive::FixedSize(int dimx, int dimy) {
  return {
      dimx,
      dimy,
      Dimension::Fixed,
      false,
  };
}

// static
ScreenInteractive ScreenInteractive::Fullscreen() {
  return {
      0,
      0,
      Dimension::Fullscreen,
      true,
  };
}

// static
ScreenInteractive ScreenInteractive::TerminalOutput() {
  return {
      0,
      0,
      Dimension::TerminalOutput,
      false,
  };
}

// static
ScreenInteractive ScreenInteractive::FitComponent() {
  return {
      0,
      0,
      Dimension::FitComponent,
      false,
  };
}

void ScreenInteractive::Post(Task task) {
  // Task/Events sent toward inactive screen or screen waiting to become
  // inactive are dropped.
  if (!task_sender_) {
    return;
  }

  task_sender_->Send(std::move(task));
}

void ScreenInteractive::PostEvent(Event event) {
  Post(event);
}

void ScreenInteractive::RequestAnimationFrame() {
  if (animation_requested_) {
    return;
  }
  animation_requested_ = true;
  auto now = animation::Clock::now();
  const auto time_histeresis = std::chrono::milliseconds(33);
  if (now - previous_animation_time_ >= time_histeresis) {
    previous_animation_time_ = now;
  }
}

CapturedMouse ScreenInteractive::CaptureMouse() {
  if (mouse_captured) {
    return nullptr;
  }
  mouse_captured = true;
  return ftxui::make_unique<CapturedMouseImpl>(
      [this] { mouse_captured = false; });
}

void ScreenInteractive::Loop(Component component) {  // NOLINT
  class Loop loop(this, std::move(component));
  loop.Run();
}

bool ScreenInteractive::HasQuitted() {
  return task_receiver_->HasQuitted();
}

void ScreenInteractive::PreMain() {
  // Suspend previously active screen:
  if (g_active_screen) {
    std::swap(suspended_screen_, g_active_screen);
    // Reset cursor position to the top of the screen and clear the screen.
    suspended_screen_->ResetCursorPosition();
    std::cout << suspended_screen_->ResetPosition(/*clear=*/true);
    suspended_screen_->dimx_ = 0;
    suspended_screen_->dimy_ = 0;

    // Reset dimensions to force drawing the screen again next time:
    suspended_screen_->Uninstall();
  }

  // This screen is now active:
  g_active_screen = this;
  g_active_screen->Install();

  previous_animation_time_ = animation::Clock::now();
}

void ScreenInteractive::PostMain() {
  // Put cursor position at the end of the drawing.
  ResetCursorPosition();

  g_active_screen = nullptr;

  // Restore suspended screen.
  if (suspended_screen_) {
    // Clear screen, and put the cursor at the beginning of the drawing.
    std::cout << ResetPosition(/*clear=*/true);
    dimx_ = 0;
    dimy_ = 0;
    Uninstall();
    std::swap(g_active_screen, suspended_screen_);
    g_active_screen->Install();
  } else {
    Uninstall();

    std::cout << '\r';
    // On final exit, keep the current drawing and reset cursor position one
    // line after it.
    if (!use_alternative_screen_) {
      std::cout << std::endl;
    }
  }
}

/// @brief Decorate a function. It executes the same way, but with the currently
/// active screen terminal hooks temporarilly uninstalled during its execution.
/// @param fn The function to decorate.
Closure ScreenInteractive::WithRestoredIO(Closure fn) {  // NOLINT
  return [this, fn] {
    Uninstall();
    fn();
    Install();
  };
}

// static
ScreenInteractive* ScreenInteractive::Active() {
  return g_active_screen;
}

void ScreenInteractive::Install() {
  frame_valid_ = false;

  // After uninstalling the new configuration, flush it to the terminal to
  // ensure it is fully applied:
  on_exit_functions.push([] { Flush(); });

  on_exit_functions.push([this] { ExitLoopClosure()(); });

  // Install signal handlers to restore the terminal state on exit. The default
  // signal handlers are restored on exit.
  for (const int signal : {SIGTERM, SIGSEGV, SIGINT, SIGILL, SIGABRT, SIGFPE}) {
    InstallSignalHandler(signal);
  }

// Save the old terminal configuration and restore it on exit.
#if defined(_WIN32)
  // Enable VT processing on stdout and stdin
  auto stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
  auto stdin_handle = GetStdHandle(STD_INPUT_HANDLE);

  DWORD out_mode = 0;
  DWORD in_mode = 0;
  GetConsoleMode(stdout_handle, &out_mode);
  GetConsoleMode(stdin_handle, &in_mode);
  on_exit_functions.push([=] { SetConsoleMode(stdout_handle, out_mode); });
  on_exit_functions.push([=] { SetConsoleMode(stdin_handle, in_mode); });

  // https://docs.microsoft.com/en-us/windows/console/setconsolemode
  const int enable_virtual_terminal_processing = 0x0004;
  const int disable_newline_auto_return = 0x0008;
  out_mode |= enable_virtual_terminal_processing;
  out_mode |= disable_newline_auto_return;

  // https://docs.microsoft.com/en-us/windows/console/setconsolemode
  const int enable_line_input = 0x0002;
  const int enable_echo_input = 0x0004;
  const int enable_virtual_terminal_input = 0x0200;
  const int enable_window_input = 0x0008;
  in_mode &= ~enable_echo_input;
  in_mode &= ~enable_line_input;
  in_mode |= enable_virtual_terminal_input;
  in_mode |= enable_window_input;

  SetConsoleMode(stdin_handle, in_mode);
  SetConsoleMode(stdout_handle, out_mode);
#else
  for (const int signal : {SIGWINCH, SIGTSTP}) {
    InstallSignalHandler(signal);
  }

  struct termios terminal;  // NOLINT
  tcgetattr(STDIN_FILENO, &terminal);
  on_exit_functions.push([=] { tcsetattr(STDIN_FILENO, TCSANOW, &terminal); });

  terminal.c_lflag &= ~ICANON;  // NOLINT Non canonique terminal.
  terminal.c_lflag &= ~ECHO;    // NOLINT Do not print after a key press.
  terminal.c_cc[VMIN] = 0;
  terminal.c_cc[VTIME] = 0;
  // auto oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
  // fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);
  // on_exit_functions.push([=] { fcntl(STDIN_FILENO, F_GETFL, oldf); });

  tcsetattr(STDIN_FILENO, TCSANOW, &terminal);

#endif

  auto enable = [&](const std::vector<DECMode>& parameters) {
    std::cout << Set(parameters);
    on_exit_functions.push([=] { std::cout << Reset(parameters); });
  };

  auto disable = [&](const std::vector<DECMode>& parameters) {
    std::cout << Reset(parameters);
    on_exit_functions.push([=] { std::cout << Set(parameters); });
  };

  if (use_alternative_screen_) {
    enable({
        DECMode::kAlternateScreen,
    });
  }

  on_exit_functions.push([=] {
    std::cout << "\033[?25h";  // Enable cursor.
    std::cout << "\033[?1 q";  // Cursor block blinking.
  });

  disable({
      // DECMode::kCursor,
      DECMode::kLineWrap,
  });

  enable({DECMode::kMouseVt200});
  enable({DECMode::kMouseAnyEvent});
  enable({DECMode::kMouseUrxvtMode});
  enable({DECMode::kMouseSgrExtMode});

  // After installing the new configuration, flush it to the terminal to
  // ensure it is fully applied:
  Flush();

  quit_ = false;
  task_sender_ = task_receiver_->MakeSender();
  event_listener_ =
      std::thread(&EventListener, &quit_, task_receiver_->MakeSender());
  animation_listener_ =
      std::thread(&AnimationListener, &quit_, task_receiver_->MakeSender());
}

void ScreenInteractive::Uninstall() {
  ExitNow();
  event_listener_.join();
  animation_listener_.join();
  OnExit();
}

// NOLINTNEXTLINE
void ScreenInteractive::RunOnceBlocking(Component component) {
  ExecuteSignalHandlers();
  Task task;
  if (task_receiver_->Receive(&task)) {
    HandleTask(component, task);
  }
  RunOnce(component);
}

void ScreenInteractive::RunOnce(Component component) {
  Task task;
  while (task_receiver_->ReceiveNonBlocking(&task)) {
    HandleTask(component, task);
    ExecuteSignalHandlers();
  }
  Draw(std::move(component));
}

void ScreenInteractive::HandleTask(Component component, Task& task) {
  switch (task.tag) {
    // Handle Event.
    case Task::kEvent: {
      Event& arg = task.event;
      if (arg.is_cursor_reporting()) {
        cursor_x_ = arg.cursor_x();
        cursor_y_ = arg.cursor_y();
        return;
      }

      if (arg.is_mouse()) {
        arg.mouse().x -= cursor_x_;
        arg.mouse().y -= cursor_y_;
      }

      arg.screen_ = this;
      component->OnEvent(arg);
      frame_valid_ = false;
      return;
    }

    // Handle callback
    case Task::kClosure: {
      task.closure();
      return;
    }

    // Handle Animation
    case Task::kAnimation: {
      if (!animation_requested_) {
        return;
      }

      animation_requested_ = false;
      const animation::TimePoint now = animation::Clock::now();
      const animation::Duration delta = now - previous_animation_time_;
      previous_animation_time_ = now;

      animation::Params params(delta);
      component->OnAnimation(params);
      frame_valid_ = false;
      return;
    }
  }
}

// NOLINTNEXTLINE
void ScreenInteractive::Draw(Component component) {
  if (frame_valid_) {
    return;
  }
  auto document = component->Render();
  int dimx = 0;
  int dimy = 0;
  auto terminal = Terminal::Size();
  document->ComputeRequirement();
  switch (dimension_) {
    case Dimension::Fixed:
      dimx = dimx_;
      dimy = dimy_;
      break;
    case Dimension::TerminalOutput:
      dimx = terminal.dimx;
      dimy = document->requirement().min_y;
      break;
    case Dimension::Fullscreen:
      dimx = terminal.dimx;
      dimy = terminal.dimy;
      break;
    case Dimension::FitComponent:
      dimx = std::min(document->requirement().min_x, terminal.dimx);
      dimy = std::min(document->requirement().min_y, terminal.dimy);
      break;
  }

  const bool resized = (dimx != dimx_) || (dimy != dimy_);
  ResetCursorPosition();
  std::cout << ResetPosition(/*clear=*/resized);

  // Resize the screen if needed.
  if (resized) {
    dimx_ = dimx;
    dimy_ = dimy;
    pixels_ = std::vector<std::vector<Pixel>>(dimy, std::vector<Pixel>(dimx));
    cursor_.x = dimx_ - 1;
    cursor_.y = dimy_ - 1;
  }

  // Periodically request the terminal emulator the frame position relative to
  // the screen. This is useful for converting mouse position reported in
  // screen's coordinates to frame's coordinates.
#if defined(FTXUI_MICROSOFT_TERMINAL_FALLBACK)
  // Microsoft's terminal suffers from a [bug]. When reporting the cursor
  // position, several output sequences are mixed together into garbage.
  // This causes FTXUI user to see some "1;1;R" sequences into the Input
  // component. See [issue]. Solution is to request cursor position less
  // often. [bug]: https://github.com/microsoft/terminal/pull/7583 [issue]:
  // https://github.com/ArthurSonzogni/FTXUI/issues/136
  static int i = -3;
  ++i;
  if (!use_alternative_screen_ && (i % 150 == 0)) {  // NOLINT
    std::cout << DeviceStatusReport(DSRMode::kCursor);
  }
#else
  static int i = -3;
  ++i;
  if (!use_alternative_screen_ &&
      (previous_frame_resized_ || i % 40 == 0)) {  // NOLINT
    std::cout << DeviceStatusReport(DSRMode::kCursor);
  }
#endif
  previous_frame_resized_ = resized;

  Render(*this, document);

  // Set cursor position for user using tools to insert CJK characters.
  {
    const int dx = dimx_ - 1 - cursor_.x + int(dimx_ != terminal.dimx);
    const int dy = dimy_ - 1 - cursor_.y;

    set_cursor_position = "\x1B[" + std::to_string(dy) + "A" +  //
                          "\x1B[" + std::to_string(dx) + "D";
    reset_cursor_position = "\x1B[" + std::to_string(dy) + "B" +  //
                            "\x1B[" + std::to_string(dx) + "C";

    if (cursor_.shape == Cursor::Hidden) {
      set_cursor_position += "\033[?25l";
    } else {
      set_cursor_position += "\033[?25h";
      set_cursor_position +=
          "\033[" + std::to_string(int(cursor_.shape)) + " q";
    }
  }

  std::cout << ToString() << set_cursor_position;
  Flush();
  Clear();
  frame_valid_ = true;
}

void ScreenInteractive::ResetCursorPosition() {
  std::cout << reset_cursor_position;
  reset_cursor_position = "";
}

Closure ScreenInteractive::ExitLoopClosure() {
  return [this] { Exit(); };
}

void ScreenInteractive::Exit() {
  Post([this] { ExitNow(); });
}

void ScreenInteractive::ExitNow() {
  quit_ = true;
  task_sender_.reset();
}

void ScreenInteractive::Signal(int signal) {
  if (signal == SIGABRT) {
    OnExit();
    return;
  }

// Windows do no support SIGTSTP / SIGWINCH
#if !defined(_WIN32)
  if (signal == SIGTSTP) {
    Post([&] {
      ResetCursorPosition();
      std::cout << ResetPosition(/*clear*/ true);  // Cursor to the beginning
      Uninstall();
      dimx_ = 0;
      dimy_ = 0;
      Flush();
      std::ignore = std::raise(SIGTSTP);
      Install();
    });
    return;
  }

  if (signal == SIGWINCH) {
    Post(Event::Special({0}));
    return;
  }
#endif
}

}  // namespace ftxui.

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <algorithm>                              // for max, min
#include <string>                                 // for allocator
#include <utility>                                // for move

namespace ftxui {

namespace {
Decorator flexDirection(GaugeDirection direction) {
  switch (direction) {
    case GaugeDirection::Up:
    case GaugeDirection::Down:
      return yflex;
    case GaugeDirection::Left:
    case GaugeDirection::Right:
      return xflex;
  }
  return xflex;  // NOT_REACHED()
}
}  // namespace

template <class T>
class SliderBase : public ComponentBase {
 public:
  explicit SliderBase(Ref<SliderOption<T>> options)
      : value_(options->value),
        min_(options->min),
        max_(options->max),
        increment_(options->increment),
        options_(options) {}

  Element Render() override {
    auto gauge_color = Focused() ? color(options_->color_active)
                                 : color(options_->color_inactive);
    const float percent = float(value_() - min_()) / float(max_() - min_());
    return gaugeDirection(percent, options_->direction) |
           flexDirection(options_->direction) | reflect(gauge_box_) |
           gauge_color;
  }

  void OnLeft() {
    switch (options_->direction) {
      case GaugeDirection::Right:
        value_() -= increment_();
        break;
      case GaugeDirection::Left:
        value_() += increment_();
        break;
      case GaugeDirection::Up:
      case GaugeDirection::Down:
        break;
    }
  }

  void OnRight() {
    switch (options_->direction) {
      case GaugeDirection::Right:
        value_() += increment_();
        break;
      case GaugeDirection::Left:
        value_() -= increment_();
        break;
      case GaugeDirection::Up:
      case GaugeDirection::Down:
        break;
    }
  }

  void OnUp() {
    switch (options_->direction) {
      case GaugeDirection::Up:
        value_() -= increment_();
        break;
      case GaugeDirection::Down:
        value_() += increment_();
        break;
      case GaugeDirection::Left:
      case GaugeDirection::Right:
        break;
    }
  }

  void OnDown() {
    switch (options_->direction) {
      case GaugeDirection::Down:
        value_() -= increment_();
        break;
      case GaugeDirection::Up:
        value_() += increment_();
        break;
      case GaugeDirection::Left:
      case GaugeDirection::Right:
        break;
    }
  }

  bool OnEvent(Event event) final {
    if (event.is_mouse()) {
      return OnMouseEvent(event);
    }

    T old_value = value_();
    if (event == Event::ArrowLeft || event == Event::Character('h')) {
      OnLeft();
    }
    if (event == Event::ArrowRight || event == Event::Character('l')) {
      OnRight();
    }
    if (event == Event::ArrowUp || event == Event::Character('k')) {
      OnDown();
    }
    if (event == Event::ArrowDown || event == Event::Character('j')) {
      OnUp();
    }

    value_() = util::clamp(value_(), min_(), max_());
    if (old_value != value_()) {
      return true;
    }

    return ComponentBase::OnEvent(event);
  }

  bool OnMouseEvent(Event event) {
    if (captured_mouse_ && event.mouse().motion == Mouse::Released) {
      captured_mouse_ = nullptr;
      return true;
    }

    if (gauge_box_.Contain(event.mouse().x, event.mouse().y) &&
        CaptureMouse(event)) {
      TakeFocus();
    }

    if (event.mouse().button == Mouse::Left &&
        event.mouse().motion == Mouse::Pressed &&
        gauge_box_.Contain(event.mouse().x, event.mouse().y) &&
        !captured_mouse_) {
      captured_mouse_ = CaptureMouse(event);
    }

    if (captured_mouse_) {
      switch (options_->direction) {
        case GaugeDirection::Right: {
          value_() = min_() + (event.mouse().x - gauge_box_.x_min) *
                                  (max_() - min_()) /
                                  (gauge_box_.x_max - gauge_box_.x_min);
          break;
        }
        case GaugeDirection::Left: {
          value_() = max_() - (event.mouse().x - gauge_box_.x_min) *
                                  (max_() - min_()) /
                                  (gauge_box_.x_max - gauge_box_.x_min);
          break;
        }
        case GaugeDirection::Down: {
          value_() = min_() + (event.mouse().y - gauge_box_.y_min) *
                                  (max_() - min_()) /
                                  (gauge_box_.y_max - gauge_box_.y_min);
          break;
        }
        case GaugeDirection::Up: {
          value_() = max_() - (event.mouse().y - gauge_box_.y_min) *
                                  (max_() - min_()) /
                                  (gauge_box_.y_max - gauge_box_.y_min);
          break;
        }
      }
      value_() = std::max(min_(), std::min(max_(), value_()));
      return true;
    }
    return false;
  }

  bool Focusable() const final { return true; }

 private:
  Ref<T> value_;
  ConstRef<T> min_;
  ConstRef<T> max_;
  ConstRef<T> increment_;
  Ref<SliderOption<T>> options_;
  Box gauge_box_;
  CapturedMouse captured_mouse_;
};

class SliderWithLabel : public ComponentBase {
 public:
  SliderWithLabel(ConstStringRef label, Component inner)
      : label_(std::move(label)) {
    Add(std::move(inner));
    SetActiveChild(ChildAt(0));
  }

 private:
  bool OnEvent(Event event) final {
    if (ComponentBase::OnEvent(event)) {
      return true;
    }

    if (!event.is_mouse()) {
      return false;
    }

    if (!box_.Contain(event.mouse().x, event.mouse().y)) {
      return false;
    }

    if (!CaptureMouse(event)) {
      return false;
    }

    TakeFocus();
    return true;
  }

  Element Render() override {
    auto focus_management = Focused() ? focus : Active() ? select : nothing;
    auto gauge_color = Focused() ? color(Color::White) : color(Color::GrayDark);
    return hbox({
               text(label_()) | dim | vcenter,
               hbox({
                   text("["),
                   ComponentBase::Render() | underlined,
                   text("]"),
               }) | xflex,
           }) |
           gauge_color | xflex | reflect(box_) | focus_management;
  }

  ConstStringRef label_;
  Box box_;
};

/// @brief An horizontal slider.
/// @param label The name of the slider.
/// @param value The current value of the slider.
/// @param min The minimum value.
/// @param max The maximum value.
/// @param increment The increment when used by the cursor.
/// @ingroup component
///
/// ### Example
///
/// ```cpp
/// auto screen = ScreenInteractive::TerminalOutput();
/// int value = 50;
/// auto slider = Slider("Value:", &value, 0, 100, 1);
/// screen.Loop(slider);
/// ```
///
/// ### Output
///
/// ```bash
/// Value:[██████████████████████████                          ]
/// ```
Component Slider(ConstStringRef label,
                 Ref<int> value,
                 ConstRef<int> min,
                 ConstRef<int> max,
                 ConstRef<int> increment) {
  SliderOption<int> option;
  option.value = value;
  option.min = min;
  option.max = max;
  option.increment = increment;
  auto slider = Make<SliderBase<int>>(option);
  return Make<SliderWithLabel>(std::move(label), slider);
}

Component Slider(ConstStringRef label,
                 Ref<float> value,
                 ConstRef<float> min,
                 ConstRef<float> max,
                 ConstRef<float> increment) {
  SliderOption<float> option;
  option.value = value;
  option.min = min;
  option.max = max;
  option.increment = increment;
  auto slider = Make<SliderBase<float>>(option);
  return Make<SliderWithLabel>(std::move(label), slider);
}
Component Slider(ConstStringRef label,
                 Ref<long> value,
                 ConstRef<long> min,
                 ConstRef<long> max,
                 ConstRef<long> increment) {
  SliderOption<long> option;
  option.value = value;
  option.min = min;
  option.max = max;
  option.increment = increment;
  auto slider = Make<SliderBase<long>>(option);
  return Make<SliderWithLabel>(std::move(label), slider);
}

/// @brief A slider in any direction.
/// @param option The options
/// ### Example
///
/// ```cpp
/// auto screen = ScreenInteractive::TerminalOutput();
/// int value = 50;
/// auto slider = Slider({
///   .value = &value,
///   .min = 0,
///   .max = 100,
///   .increment= 20,
/// });
/// screen.Loop(slider);
/// ```
template <typename T>
Component Slider(SliderOption<T> options) {
  return Make<SliderBase<T>>(options);
}
template Component Slider(SliderOption<int8_t>);
template Component Slider(SliderOption<int16_t>);
template Component Slider(SliderOption<int32_t>);
template Component Slider(SliderOption<int64_t>);

template Component Slider(SliderOption<uint8_t>);
template Component Slider(SliderOption<uint16_t>);
template Component Slider(SliderOption<uint32_t>);
template Component Slider(SliderOption<uint64_t>);

template Component Slider(SliderOption<float>);
template Component Slider(SliderOption<double>);

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

#include <cstdint>                    // for uint32_t
#include <map>
#include <memory>   // for unique_ptr, allocator
#include <utility>  // for move

namespace ftxui {

// NOLINTNEXTLINE
const std::map<std::string, std::string> g_uniformize = {
    // Microsoft's terminal uses a different new line character for the return
    // key. This also happens with linux with the `bind` command:
    // See https://github.com/ArthurSonzogni/FTXUI/issues/337
    // Here, we uniformize the new line character to `\n`.
    {"\r", "\n"},

    // See: https://github.com/ArthurSonzogni/FTXUI/issues/508
    {std::string({8}), std::string({127})},

    // See: https://github.com/ArthurSonzogni/FTXUI/issues/626
    //
    // Depending on the Cursor Key Mode (DECCKM), the terminal sends different
    // escape sequences:
    //
    //   Key     Normal    Application
    //   -----   --------  -----------
    //   Up      ESC [ A   ESC O A
    //   Down    ESC [ B   ESC O B
    //   Right   ESC [ C   ESC O C
    //   Left    ESC [ D   ESC O D
    //   Home    ESC [ H   ESC O H
    //   End     ESC [ F   ESC O F
    //
    {"\x1BOA", "\x1B[A"}, // UP
    {"\x1BOB", "\x1B[B"}, // DOWN
    {"\x1BOC", "\x1B[C"}, // RIGHT
    {"\x1BOD", "\x1B[D"}, // LEFT
    {"\x1BOH", "\x1B[H"}, // HOME
    {"\x1BOF", "\x1B[F"}, // END

};

TerminalInputParser::TerminalInputParser(Sender<Task> out)
    : out_(std::move(out)) {}

void TerminalInputParser::Timeout(int time) {
  timeout_ += time;
  const int timeout_threshold = 50;
  if (timeout_ < timeout_threshold) {
    return;
  }
  timeout_ = 0;
  if (!pending_.empty()) {
    Send(SPECIAL);
  }
}

void TerminalInputParser::Add(char c) {
  pending_ += c;
  timeout_ = 0;
  position_ = -1;
  Send(Parse());
}

unsigned char TerminalInputParser::Current() {
  return pending_[position_];
}

bool TerminalInputParser::Eat() {
  position_++;
  return position_ < (int)pending_.size();
}

void TerminalInputParser::Send(TerminalInputParser::Output output) {
  switch (output.type) {
    case UNCOMPLETED:
      return;

    case DROP:
      pending_.clear();
      return;

    case CHARACTER:
      out_->Send(Event::Character(std::move(pending_)));
      pending_.clear();
      return;

    case SPECIAL: {
      auto it = g_uniformize.find(pending_);
      if (it != g_uniformize.end()) {
        pending_ = it->second;
      }
      out_->Send(Event::Special(std::move(pending_)));
      pending_.clear();
    }
      return;

    case MOUSE:
      out_->Send(Event::Mouse(std::move(pending_), output.mouse));  // NOLINT
      pending_.clear();
      return;

    case CURSOR_REPORTING:
      out_->Send(Event::CursorReporting(std::move(pending_),  // NOLINT
                                        output.cursor.x,      // NOLINT
                                        output.cursor.y));    // NOLINT
      pending_.clear();
      return;
  }
  // NOT_REACHED().
}

TerminalInputParser::Output TerminalInputParser::Parse() {
  if (!Eat()) {
    return UNCOMPLETED;
  }

  switch (Current()) {
    case 24:  // CAN NOLINT
    case 26:  // SUB NOLINT
      return DROP;

    case '\x1B':
      return ParseESC();
    default:
      break;
  }

  if (Current() < 32) {  // C0 NOLINT
    return SPECIAL;
  }

  if (Current() == 127) {  // Delete // NOLINT
    return SPECIAL;
  }

  return ParseUTF8();
}

// Code point <-> UTF-8 conversion
//
// ┏━━━━━━━━┳━━━━━━━━┳━━━━━━━━┳━━━━━━━━┓
// ┃Byte 1  ┃Byte 2  ┃Byte 3  ┃Byte 4  ┃
// ┡━━━━━━━━╇━━━━━━━━╇━━━━━━━━╇━━━━━━━━┩
// │0xxxxxxx│        │        │        │
// ├────────┼────────┼────────┼────────┤
// │110xxxxx│10xxxxxx│        │        │
// ├────────┼────────┼────────┼────────┤
// │1110xxxx│10xxxxxx│10xxxxxx│        │
// ├────────┼────────┼────────┼────────┤
// │11110xxx│10xxxxxx│10xxxxxx│10xxxxxx│
// └────────┴────────┴────────┴────────┘
//
// Then some sequences are illegal if it exist a shorter representation of the
// same codepoint.
TerminalInputParser::Output TerminalInputParser::ParseUTF8() {
  auto head = static_cast<unsigned char>(Current());
  unsigned char selector = 0x80;  // NOLINT

  // The non code-point part of the first byte.
  unsigned char mask = selector;

  // Find the first zero in the first byte.
  unsigned int first_zero = 8;            // NOLINT
  for (unsigned int i = 0; i < 8; ++i) {  // NOLINT
    mask |= selector;
    if (!(head & selector)) {
      first_zero = i;
      break;
    }
    selector >>= 1U;
  }

  // Accumulate the value of the first byte.
  auto value = uint32_t(head & ~mask);  // NOLINT

  // Invalid UTF8, with more than 5 bytes.
  const unsigned int max_utf8_bytes = 5;
  if (first_zero == 1 || first_zero >= max_utf8_bytes) {
    return DROP;
  }

  // Multi byte UTF-8.
  for (unsigned int i = 2; i <= first_zero; ++i) {
    if (!Eat()) {
      return UNCOMPLETED;
    }

    // Invalid continuation byte.
    head = static_cast<unsigned char>(Current());
    if ((head & 0xc0) != 0x80) {  // NOLINT
      return DROP;
    }
    value <<= 6;                  // NOLINT
    value += head & 0x3f;  // NOLINT
  }

  // Check for overlong UTF8 encoding.
  int extra_byte = 0;
  if (value <= 0x7f) {                 // NOLINT
    extra_byte = 0;                                    // NOLINT
  } else if (value <= 0x7ff) {          // NOLINT
    extra_byte = 1;                                    // NOLINT
  } else if (value <= 0xffff) {         // NOLINT
    extra_byte = 2;                                    // NOLINT
  } else if (value <= 0x10ffff) {  // NOLINT
    extra_byte = 3;                                    // NOLINT
  } else {                                             // NOLINT
    return DROP;
  }

  if (extra_byte != position_) {
    return DROP;
  }

  return CHARACTER;
}

TerminalInputParser::Output TerminalInputParser::ParseESC() {
  if (!Eat()) {
    return UNCOMPLETED;
  }
  switch (Current()) {
    case 'P':
      return ParseDCS();
    case '[':
      return ParseCSI();
    case ']':
      return ParseOSC();
    default:
      if (!Eat()) {
        return UNCOMPLETED;
      } else {
        return SPECIAL;
      }
  }
}

TerminalInputParser::Output TerminalInputParser::ParseDCS() {
  // Parse until the string terminator ST.
  while (true) {
    if (!Eat()) {
      return UNCOMPLETED;
    }

    if (Current() != '\x1B') {
      continue;
    }

    if (!Eat()) {
      return UNCOMPLETED;
    }

    if (Current() != '\\') {
      continue;
    }

    return SPECIAL;
  }
}

TerminalInputParser::Output TerminalInputParser::ParseCSI() {
  bool altered = false;
  int argument = 0;
  std::vector<int> arguments;
  while (true) {
    if (!Eat()) {
      return UNCOMPLETED;
    }

    if (Current() == '<') {
      altered = true;
      continue;
    }

    if (Current() >= '0' && Current() <= '9') {
      argument *= 10;  // NOLINT
      argument += int(Current() - '0');
      continue;
    }

    if (Current() == ';') {
      arguments.push_back(argument);
      argument = 0;
      continue;
    }

    if (Current() >= ' ' && Current() <= '~' && Current() != '<') {
      arguments.push_back(argument);
      argument = 0;  // NOLINT
      switch (Current()) {
        case 'M':
          return ParseMouse(altered, true, std::move(arguments));
        case 'm':
          return ParseMouse(altered, false, std::move(arguments));
        case 'R':
          return ParseCursorReporting(std::move(arguments));
        default:
          return SPECIAL;
      }
    }

    // Invalid ESC in CSI.
    if (Current() == '\x1B') {
      return SPECIAL;
    }
  }
}

TerminalInputParser::Output TerminalInputParser::ParseOSC() {
  // Parse until the string terminator ST.
  while (true) {
    if (!Eat()) {
      return UNCOMPLETED;
    }
    if (Current() != '\x1B') {
      continue;
    }
    if (!Eat()) {
      return UNCOMPLETED;
    }
    if (Current() != '\\') {
      continue;
    }
    return SPECIAL;
  }
}

TerminalInputParser::Output TerminalInputParser::ParseMouse(  // NOLINT
    bool altered,
    bool pressed,
    std::vector<int> arguments) {
  if (arguments.size() != 3) {
    return SPECIAL;
  }

  (void)altered;

  Output output(MOUSE);
  output.mouse.button = Mouse::Button((arguments[0] & 3) +          // NOLINT
                                      ((arguments[0] & 64) >> 4));  // NOLINT
  output.mouse.motion = Mouse::Motion(pressed);                     // NOLINT
  output.mouse.shift = bool(arguments[0] & 4);                      // NOLINT
  output.mouse.meta = bool(arguments[0] & 8);                       // NOLINT
  output.mouse.x = arguments[1];                                    // NOLINT
  output.mouse.y = arguments[2];                                    // NOLINT
  return output;
}

// NOLINTNEXTLINE
TerminalInputParser::Output TerminalInputParser::ParseCursorReporting(
    std::vector<int> arguments) {
  if (arguments.size() != 2) {
    return SPECIAL;
  }
  Output output(CURSOR_REPORTING);
  output.cursor.y = arguments[0];  // NOLINT
  output.cursor.x = arguments[1];  // NOLINT
  return output;
}

}  // namespace ftxui

// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <functional>  // for function

namespace ftxui {

// NOLINTNEXTLINE
Component operator|(Component component, ComponentDecorator decorator) {
  return decorator(component);  // NOLINT
}

// NOLINTNEXTLINE
Component operator|(Component component, ElementDecorator decorator) {
  return component | Renderer(decorator);  // NOLINT
}

// NOLINTNEXTLINE
Component& operator|=(Component& component, ComponentDecorator decorator) {
  component = component | decorator;  // NOLINT
  return component;
}

// NOLINTNEXTLINE
Component& operator|=(Component& component, ElementDecorator decorator) {
  component = component | decorator;  // NOLINT
  return component;
}

}  // namespace ftxui

// Copyright 2022 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

#endif  // FTXUI_AMALGAMATION_HPP


// ===================== Part 3: PlotEngine.cpp =======================
// =====================================================================
// PlotEngine.cpp - 单文件 C++17 文字冒险游戏引擎 (内置 FTXUI 4.1.1)
//
// 单文件编译运行 (无需 FTXUI 源码目录, 只依赖 ftxui_amalgamation.hpp):
//   g++ -std=c++17 -O2 -finput-charset=UTF-8 -fexec-charset=UTF-8 PlotEngine.cpp -o plotengine.exe -lpthread   (Windows/MinGW)
//   g++ -std=c++17 -O2 -finput-charset=UTF-8 -fexec-charset=UTF-8 PlotEngine.cpp -o plotengine   -lpthread     (Linux)
// 或直接执行:  build_single.bat
//
// FTXUI 由 ftxui_amalgamation.hpp 合并提供, 遵循 MIT License,
// Copyright (c) 2019 Arthur Sonzogni (完整协议见 FTXUI-4.1.1/LICENSE)。
// 合并文件可用 tools/amalgamate_ftxui.py 重新生成。
// =====================================================================
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
using namespace ftxui;
using std::string;
using std::vector;
using std::map;
using std::pair;
using std::function;
#define E Element

// ============================================================
// 调试日志
// ============================================================
static std::ofstream g_log;
static bool g_logOn=false;
static void logOpen(){
    if(!g_log.is_open()){
        g_log.open("plotengine.log",std::ios::out|std::ios::trunc);
        g_logOn=g_log.is_open();
    }
}
static void dlog(const string& s){
    if(!g_logOn)return;
    g_log<<s<<std::endl;
    g_log.flush();
}
#define DLOG(x) do{logOpen();dlog(x);}while(0)

// ============================================================
// 工具函数
// ============================================================
static inline string tr(const string& s){
    size_t a=s.find_first_not_of(" \t\r\n");
    if(a==string::npos)return "";
    size_t b=s.find_last_not_of(" \t\r\n");
    return s.substr(a,b-a+1);
}
static inline string low(string s){for(auto&c:s)c=tolower((unsigned char)c);return s;}
static inline string up(string s){for(auto&c:s)c=toupper((unsigned char)c);return s;}
static vector<string> splitTok(const string& s){
    vector<string> r;string t;int q=0;char qc=0;
    for(size_t i=0;i<s.size();i++){
        char c=s[i];
        if(q){t+=c;if(c==qc)q=0;}
        else if(c=='"'||c=='\''){q=1;qc=c;t+=c;}
        else if(c==' '||c=='\t'){if(!t.empty()){r.push_back(t);t.clear();}}
        else t+=c;
    }
    if(!t.empty())r.push_back(t);
    return r;
}
static vector<string> splitBy(const string& s,char d){
    vector<string> r;string t;
    for(char c:s){if(c==d){r.push_back(t);t.clear();}else t+=c;}
    r.push_back(t);
    return r;
}
static string stripQuote(string s){
    if(s.size()>=2&&((s.front()=='"'&&s.back()=='"')||(s.front()=='\''&&s.back()=='\'')))
        return s.substr(1,s.size()-2);
    return s;
}
static string joinRemain(const vector<string>& v,size_t from){
    string o;for(size_t i=from;i<v.size();i++){if(i>from)o+=' ';o+=v[i];}
    return stripQuote(o);
}
static string toHex(const string& s){
    static const char*H="0123456789ABCDEF";
    string o;o.reserve(s.size()*2);
    for(unsigned char c:s){o+=H[c>>4];o+=H[c&15];}
    return o;
}
static string fromHex(const string& s){
    string o;o.reserve(s.size()/2);
    auto hv=[](char c)->int{return (c>='0'&&c<='9')?c-'0':(c>='A'&&c<='F')?c-'A'+10:(c>='a'&&c<='f')?c-'a'+10:0;};
    for(size_t i=0;i+1<s.size();i+=2)o+=(char)((hv(s[i])<<4)|hv(s[i+1]));
    return o;
}
// Base64 编解码: 使用 A-Z a-z 0-9 + / = (64 种可见字符,存档可读)
static const char* B64="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static string toBase64(const string& s){
    string o;o.reserve(((s.size()+2)/3)*4);
    for(size_t i=0;i<s.size();i+=3){
        unsigned a=(unsigned char)s[i];
        unsigned b=(i+1<s.size())?(unsigned char)s[i+1]:0;
        unsigned c=(i+2<s.size())?(unsigned char)s[i+2]:0;
        o+=B64[(a>>2)&63];
        o+=B64[((a&3)<<4)|(b>>4)];
        o+=(i+1<s.size())?B64[((b&15)<<2)|(c>>6)]:'=';
        o+=(i+2<s.size())?B64[c&63]:'=';
    }
    return o;
}
static string fromBase64(const string& s){
    auto iv=[](char ch)->int{
        for(int i=0;i<64;i++)if(B64[i]==ch)return i;
        if(ch=='='||ch=='\n'||ch=='\r')return -1;return 0;
    };
    string o;o.reserve((s.size()/4)*3);
    int buf=0,bits=0;
    for(char ch:s){
        int v=iv(ch);
        if(v<0)continue;
        buf=(buf<<6)|v;bits+=6;
        if(bits>=8){bits-=8;o+=(char)((buf>>bits)&0xFF);}
    }
    return o;
}
static string xorCrypt(const string& s,const string& k){
    string o;o.reserve(s.size());
    for(size_t i=0;i<s.size();i++)o+=s[i]^k[i%k.size()];
    return o;
}
static string itoa2(long long n){char b[40];sprintf(b,"%lld",n);return b;}
static string nowStr(){time_t t=time(nullptr);char b[64];strftime(b,sizeof(b),"%H:%M:%S",localtime(&t));return b;}
static long long nowMs(){
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static int atoiS(const string& s){return atoi(s.c_str());}
static Color parseColor(const string& s){
    string t=low(s);
    if(t=="black")return Color::Black;if(t=="red")return Color::Red;if(t=="green")return Color::Green;
    if(t=="yellow")return Color::Yellow;if(t=="blue")return Color::Blue;if(t=="magenta")return Color::Magenta;
    if(t=="cyan")return Color::Cyan;if(t=="white")return Color::White;    if(t=="gray"||t=="grey")return Color::GrayDark;
    if(t=="grayl"||t=="greyl")return Color::GrayLight;
    if(t=="redl"||t=="pink")return Color::RedLight;if(t=="greenl")return Color::GreenLight;
    if(t=="yellowl"||t=="orange")return Color::YellowLight;if(t=="bluel")return Color::BlueLight;
    if(t=="magental"||t=="purple")return Color::MagentaLight;if(t=="cyanl")return Color::CyanLight;
    if(t=="default")return Color::Default;
    return Color::Default;
}

// ============================================================
// 变量系统
// ============================================================
struct Var{string name;long long iv=0;string sv;bool isInt=true;};
class VarSystem{
public:
    vector<Var> vs;
    Var* find(const string& n){for(auto&v:vs)if(v.name==n)return&v;return nullptr;}
    const Var* find(const string& n)const{for(auto&v:vs)if(v.name==n)return&v;return nullptr;}
    void setI(const string& n,long long v){
        Var* p=find(n);
        if(p){p->iv=v;p->isInt=true;}
        else{Var x;x.name=n;x.iv=v;x.isInt=true;vs.push_back(x);}
    }
    void setS(const string& n,const string& v){
        Var* p=find(n);
        if(p){p->sv=v;p->isInt=false;}
        else{Var x;x.name=n;x.sv=v;x.isInt=false;vs.push_back(x);}
    }
    long long getI(const string& n)const{
        const Var*p=find(n);return p?(p->isInt?p->iv:atoll(p->sv.c_str())):0;
    }
    string getS(const string& n)const{
        const Var*p=find(n);
        if(!p)return"";
        return p->isInt?itoa2(p->iv):p->sv;
    }
    bool has(const string& n)const{return find(n)!=nullptr;}
    string resolve(const string& t)const{
        string o;o.reserve(t.size());
        for(size_t i=0;i<t.size();){
            if(t[i]=='$'&&i+1<t.size()){
                size_t j=i+1;
                while(j<t.size()&&(isalnum((unsigned char)t[j])||t[j]=='_'))j++;
                if(j>i+1){
                    string nm=t.substr(i+1,j-i-1);
                    o+=getS(nm);i=j;continue;
                }
            }
            o+=t[i++];
        }
        return o;
    }
    string serialize()const{
        string o;
        for(auto&v:vs){
            o+=v.name;o+="=";o+=(v.isInt?"I":"S");o+="=";
            o+=(v.isInt?std::to_string((long long)v.iv):v.sv);
            o+="\n";
        }
        return o;
    }
    void deserialize(const string& d){
        vs.clear();
        for(auto&l:splitBy(d,'\n')){
            if(l.empty())continue;
            size_t p1=l.find('=');if(p1==string::npos)continue;
            size_t p2=l.find('=',p1+1);if(p2==string::npos)continue;
            Var v;v.name=l.substr(0,p1);
            string typ=l.substr(p1+1,p2-p1-1);
            string val=l.substr(p2+1);
            v.isInt=(typ=="I");
            if(v.isInt)v.iv=atoll(val.c_str());else v.sv=val;
            vs.push_back(v);
        }
    }
    string dump()const{
        string o;
        for(auto&v:vs){o+="  $"+v.name+" = "+(v.isInt?itoa2(v.iv):v.sv)+" ["+(v.isInt?"int":"str")+"]\n";}
        return o;
    }
};
static VarSystem VARS;

// ============================================================
// 效果与面板
// ============================================================
struct Effect{string kind;int dur=500;int elapsed=0;int a=0;string text;};
struct Panel{
    string name;
    vector<string> lines;
    vector<Effect> fx;
    int cur=0;
    int curCol=0;
    bool visible=true;
    int scroll=0;        // 面板内容滚动偏移(行)
    bool follow=true;    // 是否自动跟随底部(新内容自动可见)
    void clear(){lines.clear();fx.clear();cur=0;curCol=0;scroll=0;follow=true;}
    void add(const string& l){if(lines.size()>=2000)lines.erase(lines.begin());lines.push_back(l);cur=(int)lines.size()-1;curCol=0; if(follow)scroll=(int)lines.size();}
    void addTop(const string& l){lines.insert(lines.begin(),l);cur=0;}
};

// ============================================================
// 全局状态
// ============================================================
struct State{
    // 引擎运行开关
    bool run=true;          // false 时主循环退出
    bool debug=false;       // F2 / ` 切换; 显示调试条

    // 等待中
    bool waiting=false;     // 等待玩家输入(CHOICE/INPUT)
    bool waitType=false;    // 打字机进行中

    // 动画
    bool shaking=false;     // 面板震动
    bool blinking=false;    // 面板闪烁

    // 打字机
    int typeSpeed=12;       // ms per char, 0 = instant (默认 12ms ~80字/秒)
    int typePos=0,typeLine=-1;
    long long typeT0=0;     // 打字机起始时间戳(用于批量推进)

    // 震动
    int shakeDur=0,shakeInt=2,shakeOff=0,shakeT0=0;

    // 闪烁
    int blinkDur=0,blinkStep=200,blinkOn=true,blinkT0=0;

    // 面板
    string curPanel="main";
    string typePanel="";    // 打字机所属面板(doSay 时记录)

    // INPUT
    int inputPos=0;         // INPUT 光标位置
    int inputMax=64;        // INPUT 最大长度
    string inputBuf,inputVar="input";

    // CHOICE
    vector<string> choices;
    int choiceIdx=0;

    // 流程控制
    int loadResumeIp=-1;    // 读档后跳转的 IP (用户主动 LOAD 时设为 g_ip+1, 脚本 LOAD 不设)
    std::vector<char> ifTaken;  // IF/ENDIF 配对栈: 1=该 IF 的 THEN 分支已执行(命中), 0=未命中
    int sleepUntil=0;       // 暂停时间戳 (ms)
    int curLine=0;          // 最近一次 execCmd 的 IP

    // 调试事件
    string lastKey,lastMouse,lastEvent;
    int wheelCount=0;       // [诊断] 收到的鼠标滚轮事件计数(用于定位滚轮不响应问题)

    // 面板集合
    map<string,Panel> panels;
    Panel& p(const string& n=""){string k=n.empty()?curPanel:n;return panels[k];}

    // 流程图覆盖层 (仅用户按 F 触发, 脚本不可主动调)
    bool showFlow=false;    // 是否显示流程图浮层
    int  flowScroll=0;      // 流程图滚动偏移(行)

    // 鼠标交互: 选项框屏幕包围盒(由 reflect 在渲染后填充, 用于点击命中检测)
    Box choiceBox;          // 选项框含边框的包围盒; x_max>x_min 表示本帧已渲染有效
};
static State S;
static int g_termH=24;   // 终端高度(行), 由 Renderer 每帧更新, 用于估算面板可见行数
static vector<string> g_script;
static int g_ip=0;
static string SCRIPT_FILE="demo.txt";
static string SAVE_FILE="save.txt";
static const char* ENC_KEY="PLOTENGINE_SECRET_KEY_2024";

// ============================================================
// 流程图: 数据结构 + 全局状态
//   - 仅用于 F 键触发的覆盖层显示
//   - 切换脚本 (RUN/CHAIN) 时清空重建
//   - 解析/渲染实现见下方"流程图: 解析 + 渲染"段
// ============================================================
enum FlowKind{
    FK_NOP=0,    // 注释/空行/标签
    FK_SAY,      // SAY/NARRATE/PRINT
    FK_BRANCH,   // IF (条件)
    FK_MERGE,    // ENDIF
    FK_GOTO,     // GOTO/JUMP
    FK_CALL,     // CALL/GOSUB
    FK_CHOICE,   // CHOICE/MENU
    FK_INPUT,    // INPUT/ASK
    FK_RUN,      // RUN/CHAIN (切脚本)
    FX_EXIT      // EXIT/QUIT/END
};
struct FlowNode{
    int ip=-1;                          // 节点起始 IP (源行号, 标签占首行)
    int ipEnd=-1;                       // 节点结束 IP (块语句的 ENDIF/ELSE 行; 普通节点 == ip)
    string label;                       // 标签名(若本行是标签定义)
    FlowKind kind=FK_NOP;
    string text;                        // 描述文本 (SAY 摘要 / 条件 / 目标 label)
    vector<int> succ;                   // 后继节点 IP (主流程: ip+1, 跳转: 目标)
    vector<int> succFalse;              // 条件为假时的后继 (仅 BRANCH 用)
    bool isCurrent=false;               // 当前 IP 命中此节点
};
static vector<FlowNode> g_flow;
static bool g_flowDirty=true;           // 脚本变化后置 true 重建
static int g_flowCurrentIdx=-1;         // 当前 IP 命中节点的下标(渲染时高亮)

// ============================================================
// 富文本解析
//   标签: [b] [u] [i] [blink] [red] [bg:blue] [reset]
//   颜色: black/red/green/yellow/blue/magenta/cyan/white/gray + Light 后缀
//   转义: \[ \] \\
// ============================================================
struct StyleState{
    Color fg=Color::Default;
    Color bg=Color::Default;
    bool b=false,u=false,i=false,bl=false;
};
static vector<pair<string,StyleState>> parseRich(const string& raw){
    vector<pair<string,StyleState>> segs;StyleState cur;string buf;
    auto flush=[&](){
        if(!buf.empty()){segs.push_back({buf,cur});buf.clear();}
    };
    for(size_t i=0;i<raw.size();){
        if(raw[i]=='\\'&&i+1<raw.size()){
            // 转义: 跳过反斜杠,加入下一个字符
            if(raw[i+1]=='n')buf+='\n';
            else buf+=raw[i+1];
            i+=2;continue;
        }
        if(raw[i]=='['){
            size_t j=raw.find(']',i+1);
            if(j!=string::npos){
                flush();
                string tag=low(raw.substr(i+1,j-i-1));
                if(tag=="b"||tag=="bold")cur.b=true;
                else if(tag=="u"||tag=="under")cur.u=true;
                else if(tag=="i"||tag=="inv"||tag=="invert")cur.i=true;
                else if(tag=="bl"||tag=="blink")cur.bl=true;
                else if(tag=="reset"||tag=="/"||tag=="0")cur=StyleState();
                else if(tag.rfind("fg:",0)==0)cur.fg=parseColor(tag.substr(3));
                else if(tag.rfind("bg:",0)==0)cur.bg=parseColor(tag.substr(3));
                else if(tag.rfind("color:",0)==0)cur.fg=parseColor(tag.substr(6));
                else{Color c=parseColor(tag);if(c!=Color::Default)cur.fg=c;}
                i=j+1;continue;
            }
        }
        if(raw[i]=='\n'){flush();buf+='\n';flush();i++;continue;}
        buf+=raw[i++];
    }
    flush();
    return segs;
}
static Element renderStyledStr(const string& txt,const StyleState& st){
    Element e=text(txt);
    if(st.b)e=e|bold;
    if(st.u)e=e|underlined;
    if(st.i)e=e|inverted;
    if(st.bl)e=e|blink;
    if(st.fg!=Color::Default)e=e|color(st.fg);
    if(st.bg!=Color::Default)e=e|bgcolor(st.bg);
    return e;
}
static Element richLine(const string& raw){
    string r=VARS.resolve(raw);
    auto segs=parseRich(r);
    if(segs.empty())return text("");
    // 处理多行(允许 \n 切分)
    Elements lines;Elements cur;string curTxt;StyleState curSt;
    auto pushCur=[&](){
        if(!curTxt.empty())cur.push_back(renderStyledStr(curTxt,curSt));
    };
    auto pushLine=[&](){
        pushCur();
        if(!cur.empty()){lines.push_back(hbox(cur));cur.clear();curTxt.clear();}
    };
    for(auto&sg:segs){
        const string&t=sg.first;const StyleState&st=sg.second;
        size_t i=0;
        while(i<t.size()){
            size_t p=t.find('\n',i);
            if(p==string::npos){
                string sub=t.substr(i);
                if(!sub.empty()){
                    if(st.fg!=curSt.fg||st.bg!=curSt.bg||st.b!=curSt.b||st.u!=curSt.u||st.i!=curSt.i||st.bl!=curSt.bl){
                        pushCur();curTxt.clear();
                        curSt=st;
                    }
                    curTxt+=sub;pushCur();curTxt.clear();
                }
                break;
            }else{
                string sub=t.substr(i,p-i);
                if(!sub.empty()){
                    if(st.fg!=curSt.fg||st.bg!=curSt.bg||st.b!=curSt.b||st.u!=curSt.u||st.i!=curSt.i||st.bl!=curSt.bl){
                        pushCur();curTxt.clear();
                        curSt=st;
                    }
                    curTxt+=sub;pushCur();
                }
                pushLine();
                i=p+1;
            }
        }
    }
    pushLine();
    if(lines.empty())return text("");
    if(lines.size()==1)return lines[0];
    return vbox(lines);
}

// 打字机效果: 截取前 N 个"可见"字符
static int visibleCharCount(const string& s){
    int n=0;
    for(size_t i=0;i<s.size();i++){
        if(s[i]=='\\'&&i+1<s.size()){i++;n++;continue;}
        if(s[i]=='['){
            size_t j=s.find(']',i+1);
            if(j!=string::npos){i=j;continue;}
        }
        if(s[i]!='\n')n++;
    }
    return n;
}
static string visibleSubstr(const string& s,int maxChars){
    if(maxChars<0)maxChars=0;
    string o;int n=0;
    for(size_t i=0;i<s.size()&&n<maxChars;i++){
        if(s[i]=='\\'&&i+1<s.size()){
            if(s[i+1]=='n')o+='\n';else o+=s[i+1];
            i++;n++;continue;
        }
        if(s[i]=='['){
            size_t j=s.find(']',i+1);
            if(j!=string::npos){
                o+=s.substr(i,j-i+1);
                i=j;continue;
            }
        }
        if(s[i]!='\n'){o+=s[i];n++;}
    }
    return o;
}
static Element richLineAnim(const string& raw,int maxChars){
    string r=VARS.resolve(raw);
    return richLine(visibleSubstr(r,maxChars));
}

// ============================================================
// 渲染面板
// ============================================================
static Element renderPanelContent(Panel&p){
    Elements ls;
    int total=(int)p.lines.size();
    // 估算面板正文可见行数(标题/分隔线/边框/输入栏等固定开销)
    int avail=g_termH-8; if(avail<1)avail=1;
    int top=p.scroll;
    if(top<0)top=0;
    int maxTop=0; if(total>avail)maxTop=total-avail;  // 最多滚到末屏首行
    if(top>maxTop)top=maxTop;
    p.scroll=top;   // 回写, 防止 CatchEvent 计算越界
    p.follow=(top>=maxTop);  // 在底部则保持自动跟随, 否则视为用户正在查看历史
    for(int i=top;i<total;i++){
        if(S.waitType&&i==S.typeLine&&S.typeLine>=0){
            ls.push_back(richLineAnim(p.lines[i],S.typePos));
        }else{
            ls.push_back(richLine(p.lines[i]));
        }
    }
    if(ls.empty())ls.push_back(text(" ")|dim);
    return vbox(ls);
}
static Element renderAll(){
    Elements panelEls;
    vector<string> names;
    for(auto&kv:S.panels)if(kv.second.visible)names.push_back(kv.first);
    sort(names.begin(),names.end());
    for(auto&nm:names){
        Panel&p=S.panels[nm];
        bool isCur=(nm==S.curPanel);
        Element title=text(" "+nm+" ")|bold|color(isCur?Color::Yellow:Color::GrayLight);
        Element body=renderPanelContent(p);
        int avail=g_termH-8; if(avail<1)avail=1;
        body=body|size(HEIGHT, EQUAL, avail);   // 固定视口高度, 溢出裁切(避免 flex 撑大布局导致滚动失效)
        Element bx=vbox({title,separator(),body})|border;
        if(isCur)bx=bx|color(Color::Yellow);
        panelEls.push_back(bx|flex);
    }
    if(panelEls.empty()){Panel pp;S.panels["main"]=pp;return text(" (no panel) ")|border;}
    if(panelEls.size()==1)return panelEls[0];
    return hbox(panelEls)|flex;
}

// 面板内容滚动: 上下键(及 PgUp/PgDn/Home/End)滚动"所有可见面板"内容.
// 始终生效(含 CHOICE/INPUT 期间), 让 UP/DOWN 在任何状态下都能翻看面板历史;
// 选项用数字键 1-9 + 回车选择, 输入框用 ←/→ 移光标, 互不冲突.
// (流程图 showFlow 期间由专门分支处理, 此处跳过)
static bool handlePanelScroll(Event ev){
    if(S.showFlow)return false;
    bool isScrollKey=(ev==Event::ArrowUp||ev==Event::ArrowDown||
                      ev==Event::PageUp||ev==Event::PageDown||
                      ev==Event::Home||ev==Event::End);
    if(!isScrollKey)return false;
    // INPUT 模式下 Home/End 留给输入框移动光标(见 CatchEvent 的 INPUT 分支), 不滚面板;
    // 其余滚动键(↑/↓/PgUp/PgDn)在 INPUT 期间仍滚面板, 与 ←/→ 移光标互不冲突
    if(S.waiting && S.lastEvent=="INPUT" && (ev==Event::Home||ev==Event::End))return false;
    for(auto&kv:S.panels){
        if(!kv.second.visible)continue;
        Panel&p=kv.second;
        if(ev==Event::ArrowUp){ if(p.scroll>0)p.scroll--; p.follow=false; }
        else if(ev==Event::ArrowDown){ p.scroll++; }
        else if(ev==Event::PageUp){ p.scroll-=10; if(p.scroll<0)p.scroll=0; p.follow=false; }
        else if(ev==Event::PageDown){ p.scroll+=10; }
        else if(ev==Event::Home){ p.scroll=0; p.follow=false; }
        else if(ev==Event::End){ p.scroll=(int)p.lines.size(); p.follow=true; }
    }
    return true;
}
static Element renderInputBox(){
    if(!S.choices.empty()){
        Elements es;
        for(size_t i=0;i<S.choices.size();i++){
            string ch=itoa2(i+1)+". "+S.choices[i];
            Element e=text(ch);
            if((int)i==S.choiceIdx)e=e|color(Color::Black)|bgcolor(Color::White)|bold;
            es.push_back(e);
        }
        // 用 reflect 包裹选项框, 渲染后把屏幕包围盒写入 S.choiceBox, 供鼠标点击命中检测
        Element box=vbox({text(" [选择] ")|bold|color(Color::Yellow),vbox(es)})|border;
        return box|reflect(S.choiceBox);
    }
    if(S.waiting&&S.lastEvent=="INPUT"){
        // 显示当前输入缓冲,光标位置用反显下划线
        string pre=S.inputBuf.substr(0,S.inputPos);
        string cur=(S.inputPos<(int)S.inputBuf.size())?string(1,S.inputBuf[S.inputPos]):"";
        string post=(S.inputPos<(int)S.inputBuf.size())?S.inputBuf.substr(S.inputPos+1):"";
        Elements es;
        es.push_back(text(" > ")|bold|color(Color::Green));
        es.push_back(text(pre));
        if(!cur.empty())es.push_back(text(cur)|inverted|underlined);
        es.push_back(text("_")|blink);
        es.push_back(text(post));
        es.push_back(text(" [回车确认] ")|dim);
        return hbox(es)|border;
    }
    return text("");
}
static Element renderDebugBar(){
    if(!S.debug)return text("");
    Elements dbg;
    dbg.push_back(text(" DEBUG  脚本:"+SCRIPT_FILE+"  行:"+itoa2(S.curLine)+"  IP:"+itoa2(g_ip)+
                       "  面板:"+S.curPanel+"  F2切换 ")|bold|color(Color::Red));
    dbg.push_back(separator());
    dbg.push_back(text(" 事件:["+S.lastKey+"] mouse:["+S.lastMouse+"]"));
    dbg.push_back(text(" wheelCount:["+itoa2(S.wheelCount)+"]  (滚动滚轮应增加, 用于定位滚轮无响应)"));
    dbg.push_back(text(" 变量:"));
    dbg.push_back(paragraph(VARS.dump())|color(Color::GrayLight));
    dbg.push_back(separator());
    return vbox(dbg)|border|color(Color::Red);
}

// ============================================================
// 指令执行
// ============================================================
static int findLabelIdx(const string& lbl,int from=0){
    // 支持 :label / ::label / label: 三种写法
    string tgt1=lbl+":";
    string tgt2=":"+lbl+":";
    string tgt3="::"+lbl+":";
    for(int i=from;i<(int)g_script.size();i++){
        string t=tr(g_script[i]);
        if(t==tgt1||t==tgt2||t==tgt3||t=="::"+lbl||t==":"+lbl)return i;
    }
    return -1;
}
static int evalExpr(const string& e){
    string t=tr(e);
    if(t.empty())return 0;
    // 剥掉最外层配对的括号, 使 ((1+1)) 等纯括号表达式也能求值
    while(t.size()>=2 && t.front()=='(' && t.back()==')'){
        int d=0;bool balanced=true;
        for(size_t i=0;i<t.size();i++){
            if(t[i]=='(')d++;
            else if(t[i]==')'){d--; if(d==0 && i!=(t.size()-1)){balanced=false;break;}}
        }
        if(balanced && d==0)t=t.substr(1,t.size()-2);
        else break;
    }
    if(VARS.has(t))return(int)VARS.getI(t);
    // 仅当整个字符串都是数字字面量时直接返回,否则交给运算符解析
    {
        bool allNum=true;
        for(size_t i=0;i<t.size();i++){
            char c=t[i];
            if(i==0&&(c=='-'||c=='+'))continue;
            if(!isdigit((unsigned char)c)){allNum=false;break;}
        }
        if(allNum)return atoiS(t);
    }
    // 优先级: 先找最低优先级运算符
    int paren=0;int minPrio=99;int opPos=-1;char op=0;
    for(size_t i=0;i<t.size();i++){
        char c=t[i];
        if(c=='('){paren++;continue;}
        if(c==')'){paren--;continue;}
        if(paren>0)continue;
        int p=99;
        if(c=='*'||c=='/'||c=='%')p=2;
        else if(c=='+'||c=='-')p=1;
        if(p<=minPrio){minPrio=p;opPos=(int)i;op=c;}
    }
    if(opPos<0)return 0;
    // 切分后 trim 再递归,避免空格导致 atoll 解析失败
    string lt=tr(t.substr(0,opPos));
    string rt=tr(t.substr(opPos+1));
    // 去除最外层配对括号 (1+2) -> 1+2
    auto stripParen=[](string s)->string{
        while(s.size()>=2&&s.front()=='('&&s.back()==')'){
            int d=0;bool ok=true;
            for(size_t i=0;i<s.size();i++){
                if(s[i]=='(')d++;
                else if(s[i]==')'){d--;if(d==0&&i!=s.size()-1){ok=false;break;}}
            }
            if(ok)s=s.substr(1,s.size()-2);else break;
        }
        return s;
    };
    long long a=evalExpr(stripParen(lt));
    long long b=evalExpr(stripParen(rt));
    if(op=='+')return(int)(a+b);
    if(op=='-')return(int)(a-b);
    if(op=='*')return(int)(a*b);
    if(op=='/')return b?int(a/b):0;
    if(op=='%')return b?int(a%b):0;
    return 0;
}
static string resolveStr(const string& e){
    string t=tr(e);
    if(t.empty())return "";
    if(t.front()=='"'||t.front()=='\'')return stripQuote(t);
    string o;
    auto parts=splitTok(t);
    for(size_t i=0;i<parts.size();i++){
        auto&p=parts[i];
        if(i>0)o+=' ';
        if(p.size()>=2&&p.front()=='"'&&p.back()=='"')o+=stripQuote(p);
        else o+=VARS.resolve(p);
    }
    return o;
}
static void doSay(const string& text,bool newline=true){
    Panel&p=S.panels[S.curPanel];
    if(!newline){
        if(p.lines.empty())p.add("");
        if(!p.lines.empty())p.lines.back()+=text;
    }else p.add(text);
    S.waitType=true;S.typeLine=(int)p.lines.size()-1;S.typePos=0;
    S.typeT0=nowMs();   // 记录起始时间,用于批量推进
    S.typePanel=S.curPanel;  // 记录打字机所属面板,避免多面板时取错行
}
// 立即完成当前打字机行(用户主动跳过时使用)
static void flushTypewriter(){
    if(S.waitType&&S.typeLine>=0){
        Panel* pp=&S.panels[S.typePanel.empty()?S.curPanel:S.typePanel];
        if(pp&&S.typeLine<(int)pp->lines.size()){
            S.typePos=visibleCharCount(pp->lines[S.typeLine]);
        }
    }
    S.waitType=false;S.typeLine=-1;
    S.sleepUntil=0;
}
// 解析面板名参数: 若空返回 S.curPanel (用于 CLEAR/EFFECT/CURSOR/HIDE/SHOW/LOG 等)
static inline string argPanel(const string& name){
    return name.empty() ? S.curPanel : name;
}
// ============================================================
// 流程图: 解析 + 渲染
// ============================================================

// 提取标签(若本行是标签定义,返回规范名;否则空)
//   支持: name: / :name / :name: / ::name / ::name:
static string flowLabelOf(const string& line){
    string t=tr(line);
    if(t.empty()||t.find(' ')!=string::npos)return "";   // 标签行无空格
    if(t.back()==':')return t.substr(0,t.size()-1);
    if(t.front()==':'){
        string body=t.substr(1);
        if(!body.empty()&&body.back()==':')body.pop_back();
        return body;
    }
    return "";
}

// 在 g_flow 中按 IP 找节点下标
static int flowIdxByIp(int ip){
    for(int i=0;i<(int)g_flow.size();i++)if(g_flow[i].ip==ip)return i;
    return -1;
}

// 从标签名找节点下标(找首个 ip==标签行的节点)
static int flowIdxByLabel(const string& lbl){
    for(int i=0;i<(int)g_flow.size();i++){
        if(!g_flow[i].label.empty()&&g_flow[i].label==lbl)return i;
    }
    return -1;
}

// 扫描 g_script 建图 (一次性, 缓存到 g_flow)
// 工具: 从 "goto xxx" / "call xxx" 提取标签
static string pp_to_label(const string& s){
    size_t sp=s.find(' ');
    if(sp==string::npos)return s;
    return s.substr(sp+1);
}

// 扫描 g_script 建图 (一次性, 缓存到 g_flow)
// 思路: 略过空行/注释/标签占位, 把 "命令行 + 紧随的标签行" 合并成一个节点
//       块语句 (IF ... ENDIF) 算一个节点, 占据从 IF 行到 ENDIF 行的范围
static void buildFlow(){
    g_flow.clear();
    g_flowCurrentIdx=-1;
    if(g_script.empty())return;

    // 第一遍: 收集所有标签行号 + 命令行
    int n=(int)g_script.size();
    for(int i=0;i<n;i++){
        string t=tr(g_script[i]);
        if(t.empty()||t[0]=='#'||t[0]==';')continue;

        string lbl=flowLabelOf(t);
        if(!lbl.empty()){
            FlowNode nd;
            nd.ip=i;
            nd.ipEnd=i;
            nd.label=lbl;
            nd.kind=FK_NOP;
            nd.text=":"+lbl;
            g_flow.push_back(nd);
            continue;
        }
        auto pp=splitTok(t);
        if(pp.empty())continue;
        string cmd=up(pp[0]);
        if(cmd=="REM"||cmd=="//")continue;

        FlowNode nd;
        nd.ip=i;
        nd.ipEnd=i;
        if(cmd=="SAY"||cmd=="S"||cmd=="PRINT"||cmd=="P"||cmd=="SAY+"||cmd=="S+"){
            nd.kind=FK_SAY;
            string s=joinRemain(pp,1);
            if(s.size()>28)s=s.substr(0,26)+"..";
            nd.text=s;
        }else if(cmd=="NARRATE"||cmd=="N"||cmd=="LN"){
            nd.kind=FK_SAY;
            string s=joinRemain(pp,1);
            if(s.size()>24)s=s.substr(0,22)+"..";
            nd.text="(旁白) "+s;
        }else if(cmd=="IF"||cmd=="IFE"){
            nd.kind=FK_BRANCH;
            string cond;
            for(size_t k=1;k<pp.size();k++){
                if(k>1)cond+=" ";
                cond+=pp[k];
            }
            if(cond.size()>26)cond=cond.substr(0,24)+"..";
            nd.text="if "+cond;
            int depth=1;
            int j=i+1;
            for(;j<n;j++){
                string u=tr(g_script[j]);auto qq=splitTok(u);
                if(qq.empty())continue;
                string cc=up(qq[0]);
                if(cc=="IF"||cc=="IFE")depth++;
                else if(cc=="ENDIF"){depth--;if(depth==0)break;}
            }
            nd.ipEnd=j;
        }else if(cmd=="GOTO"||cmd=="JUMP"){
            nd.kind=FK_GOTO;
            nd.text="goto "+(pp.size()>=2?pp[1]:"?");
        }else if(cmd=="CALL"||cmd=="GOSUB"){
            nd.kind=FK_CALL;
            nd.text="call "+(pp.size()>=2?pp[1]:"?");
        }else if(cmd=="CHOICE"||cmd=="MENU"||cmd=="CHOICES"){
            nd.kind=FK_CHOICE;
            int nopt=(int)pp.size()-1;
            if(nopt<0)nopt=0;
            nd.text="choice ["+itoa2(nopt)+"]";
        }else if(cmd=="INPUT"||cmd=="ASK"){
            nd.kind=FK_INPUT;
            nd.text="input"+(pp.size()>=2?(" ->"+pp[1]):"");
        }else if(cmd=="RUN"||cmd=="CHAIN"){
            nd.kind=FK_RUN;
            nd.text="run "+(pp.size()>=2?pp[1]:"?");
        }else if(cmd=="EXIT"||cmd=="QUIT"||cmd=="END"){
            nd.kind=FX_EXIT;
            nd.text="exit";
        }else{
            nd.kind=FK_SAY;
            string s=joinRemain(pp,0);
            if(s.size()>28)s=s.substr(0,26)+"..";
            nd.text=s;
        }
        g_flow.push_back(nd);
    }

    // 第二遍: 补边
    for(int i=0;i<(int)g_flow.size();i++){
        FlowNode& nd=g_flow[i];
        int defaultSucc=-1;
        for(int k=i+1;k<(int)g_flow.size();k++){
            if(g_flow[k].kind!=FK_NOP){defaultSucc=k;break;}
        }
        if(nd.kind==FK_BRANCH){
            int afterEnd=-1;
            int j=flowIdxByIp(nd.ipEnd);
            if(j>=0){
                for(int k=j+1;k<(int)g_flow.size();k++){
                    if(g_flow[k].kind!=FK_NOP){afterEnd=k;break;}
                }
            }
            nd.succFalse.push_back(afterEnd);
            int trueSucc=-1;
            for(int k=i+1;k<(int)g_flow.size();k++){
                if(g_flow[k].kind!=FK_NOP){trueSucc=k;break;}
            }
            nd.succ.push_back(trueSucc);
        }else if(nd.kind==FK_GOTO){
            int tgt=flowIdxByLabel(pp_to_label(nd.text));
            nd.succ.push_back(tgt);
        }else if(nd.kind==FK_CALL){
            int tgt=flowIdxByLabel(pp_to_label(nd.text));
            nd.succ.push_back(tgt);
            nd.succ.push_back(defaultSucc);
        }else if(nd.kind==FK_RUN){
            nd.succ.push_back(-1);
        }else if(nd.kind==FX_EXIT){
            // 无后继
        }else{
            nd.succ.push_back(defaultSucc);
        }
    }

    g_flowCurrentIdx=flowIdxByIp(g_ip);
    if(g_flowCurrentIdx>=0)g_flow[g_flowCurrentIdx].isCurrent=true;
}

// 渲染: 节点用一行, 节点之间用 ↓ / 条件分支用条件 true|false
//   每节点: [#ip] 形状  文本
//   边: 居于两节点之间, 独占一行
static Element renderFlow(){
    if(g_flowDirty||g_flow.empty()){
        buildFlow();
        g_flowDirty=false;
    }

    // 标题栏
    Element title=vbox({
        text(" [bg:cyan][fg:black] FLOW [/]  "+SCRIPT_FILE+
             "  ("+itoa2(g_flow.size())+" nodes, ip="+itoa2(g_ip)+")  "+
             "F/Esc:close  Up/Down:scroll  PgUp/PgDn:page  Home/End:jump ")|bold
    });
    Element hdr=title|color(Color::Cyan);

    // 内容: 把每节点 + 其下边画成"行"列表
    Elements rows;
    auto nodeShape=[](FlowKind k)->string{
        switch(k){
            case FK_SAY:    return "[SAY]";
            case FK_BRANCH: return "[IF ]";
            case FK_MERGE:  return "[END]";
            case FK_GOTO:   return "[GO ]";
            case FK_CALL:   return "[CAL]";
            case FK_CHOICE: return "[CHC]";
            case FK_INPUT:  return "[IN ]";
            case FK_RUN:    return "[RUN]";
            case FX_EXIT:   return "[END]";
            default:        return "[.. ]";
        }
    };
    for(int i=0;i<(int)g_flow.size();i++){
        const FlowNode& nd=g_flow[i];
        // 节点行
        string ipStr=itoa2(nd.ip);
        // 4 位宽 IP
        while((int)ipStr.size()<4)ipStr=" "+ipStr;
        string line;
        if(!nd.label.empty()){
            line=" L "+ipStr+" :"+nd.label;
        }else{
            line="   "+ipStr+" "+nodeShape(nd.kind)+"  "+nd.text;
        }
        Element ndEl=text(line);
        if(nd.isCurrent)ndEl=ndEl|bold|color(Color::Yellow);
        else if(!nd.label.empty())ndEl=ndEl|color(Color::Magenta);
        else if(nd.kind==FK_BRANCH)ndEl=ndEl|color(Color::Cyan);
        else if(nd.kind==FK_GOTO)ndEl=ndEl|color(Color::Green);
        else if(nd.kind==FK_CALL)ndEl=ndEl|color(Color::Blue);
        else if(nd.kind==FK_RUN)ndEl=ndEl|color(Color::Red);
        else if(nd.kind==FX_EXIT)ndEl=ndEl|dim|color(Color::Red);
        else if(nd.kind==FK_CHOICE)ndEl=ndEl|color(Color::Yellow);
        else if(nd.kind==FK_INPUT)ndEl=ndEl|color(Color::White);
        rows.push_back(ndEl);

        // 边: 每条 succ 画一行箭头; BRANCH 画两条 (true / false)
        if(nd.kind==FK_BRANCH){
            // true 分支
            string tArrow="   |    |--> true";
            if(!nd.succ.empty()&&nd.succ[0]>=0)tArrow+=" -> #"+itoa2(g_flow[nd.succ[0]].ip);
            else tArrow+=" (none)";
            rows.push_back(text(tArrow)|color(Color::Green));
            // false 分支
            string fArrow="   |    |--> false";
            if(!nd.succFalse.empty()&&nd.succFalse[0]>=0)fArrow+=" -> #"+itoa2(g_flow[nd.succFalse[0]].ip);
            else fArrow+=" (none)";
            rows.push_back(text(fArrow)|color(Color::Red));
        }else if(!nd.succ.empty()&&nd.succ[0]>=0){
            string arrow="   |    v";
            if(nd.succ.size()>=2&&nd.succ[1]>=0){
                // CALL 后的双后继
                arrow="   |    |--> call-ret -> #"+itoa2(g_flow[nd.succ[0]].ip);
                rows.push_back(text(arrow)|color(Color::Blue));
                arrow="   |    v";
            }
            arrow+=" -> #"+itoa2(g_flow[nd.succ[0]].ip);
            rows.push_back(text(arrow)|dim);
        }
    }
    if(rows.empty())rows.push_back(text(" (空脚本) ")|dim);

    // 滚动: 屏幕高度未知, 用 ftxui 提供的 screen.dim 限定
    //   简单稳妥做法: 直接把 rows 全部塞进 vbox, 由 ftxui 自然布局
    //   屏幕外的行被裁掉; 实际"滚动"由 ftxui 在我们返回 element 时按
    //   可用高度决定; 但 ftxui vbox 不支持主动指定 top 偏移, 所以
    //   下面用 S.flowScroll 裁掉前面的行
    int total=(int)rows.size();
    int top=S.flowScroll;
    if(top<0)top=0;
    if(top>total-1)top=total>0?total-1:0;
    S.flowScroll=top;   // 回写, 防止 CatchEvent 计算越界
    // 裁切: 跳过 [0, top) 行, 取 [top, end)
    Elements visible;
    visible.reserve(total-top);
    for(int k=top;k<total;k++)visible.push_back(rows[k]);

    int avail=g_termH-5; if(avail<1)avail=1;
    Element body=vbox(visible)|size(HEIGHT, EQUAL, avail);
    // 滚动指示器 (右上角显示当前位置)
    string ind="";
    if(total>0){
        int curLine=0;
        if(g_flowCurrentIdx>=0){
            // 当前 IP 节点所在行下标
            curLine=g_flowCurrentIdx*2;  // 节点 + 边 各占一行, 近似
        }
        ind="  ("+itoa2(top+1)+"/"+itoa2(total)+"  ip-row~"+itoa2(curLine+1)+")";
    }
    Element scrollInd=text(ind)|dim|color(Color::GrayLight);
    return vbox({hdr,separator(),body,scrollInd})|border|flex;
}

// 重置游戏运行态(变量、面板、等待/输入状态、打字机/震屏/闪烁)
// 用于 RUN/CHAIN 切换剧本时清空旧脚本残留
static void resetGameState(){
    VARS.vs.clear();           // 清空所有变量(int + str)
    S.panels.clear();          // 清空所有面板(包括 main)
    S.choices.clear();
    S.choiceIdx=0;
    S.inputBuf.clear();
    S.inputPos=0;
    S.waiting=false;
    S.waitType=false;
    S.lastEvent.clear();
    S.lastKey.clear();
    S.lastMouse.clear();
    S.shaking=false;S.blinking=false;
    S.shakeDur=S.shakeInt=S.shakeOff=S.shakeT0=0;
    S.blinkDur=S.blinkOn=S.blinkT0=0;
    S.typePos=0;S.typeLine=-1;S.typeT0=0;S.typePanel="";
    S.sleepUntil=0;
    S.curLine=0;
    S.showFlow=false;
    S.flowScroll=0;
    S.loadResumeIp=-1;        // 清除可能残留的读档恢复 IP (避免 RUN/CHAIN 后误跳转)
    S.ifTaken.clear();        // 清除可能残留的 IF/ELSE 配对状态 (避免跨剧本误判)
    g_flow.clear();            // 流程图缓存失效
    g_flowDirty=true;
    flushTypewriter();
}
// 用户主动存档: 序列化为 Base64 + XOR 密文写入文件
static bool userSave(const string& fn){
    string d=VARS.serialize();
    // 记录当前 IP+1, 用户 LOAD 时跳回此处继续
    char ib[96];sprintf(ib,"LINE=%d\nIP=%d\nSCRIPT=%s\nPANEL=%s\n",S.curLine,g_ip+1,SCRIPT_FILE.c_str(),S.curPanel.c_str());
    d+=ib;
    string enc=toBase64(xorCrypt(d,ENC_KEY));
    std::ofstream f(fn,std::ios::binary|std::ios::trunc);
    if(!f){doSay("[fg:red][系统] 存档失败: 无法写入 "+fn+"[/]");return false;}
    f<<enc;
    doSay("[fg:green][系统] 存档成功 -> "+fn+"[/]");
    return true;
}
// 用户主动读档: 还原变量 + 面板 + 跳回存档时的下一行
// resumeMode=true (用户主动/F9) 时, 从存档读 IP 并跳回; false (脚本 LOAD) 时不跳
static bool userLoad(const string& fn,bool resumeMode=false){
    std::ifstream f(fn,std::ios::binary);
    if(!f){doSay("[fg:red][系统] 读档失败: 找不到 "+fn+"[/]");return false;}
    string d;{std::ostringstream ss;ss<<f.rdbuf();d=ss.str();}
    if(d.empty()){doSay("[fg:red][系统] 读档失败: 存档为空[/]");return false;}
    string dec=fromBase64(d);
    dec=xorCrypt(dec,ENC_KEY);
    VARS.deserialize(dec);
    int resumeIp=-1;
    for(auto&l:splitBy(dec,'\n')){
        if(l.empty())continue;
        size_t p1=l.find('=');if(p1==string::npos)continue;
        string k=l.substr(0,p1),v=l.substr(p1+1);
        if(k=="IP"&&!v.empty())resumeIp=atoiS(v);
        else if(k=="LINE")S.curLine=atoiS(v);
        else if(k=="SCRIPT")SCRIPT_FILE=v;
        else if(k=="PANEL")S.curPanel=v;
    }
    if(resumeMode){
        doSay("[fg:green][系统] 读档成功 <- "+fn+"  继续执行 IP="+itoa2(resumeIp)+"[/]");
        S.loadResumeIp=resumeIp;
    }else{
        doSay("[fg:green][系统] 读档成功 <- "+fn+"[/]");
    }
    S.choices.clear();S.inputBuf.clear();S.inputPos=0;
    S.waiting=false;S.lastEvent="";
    flushTypewriter();
    return true;
}
// 判断一行是否为"块 IF" (IF 后无内联动作, 需配合 ENDIF 收尾)
// 用于流程跳过时正确计数块作用域, 避免单行 IF 干扰深度统计
static bool isBlockIfLine(const string& rawLine){
    string t=tr(rawLine);
    if(t.empty())return false;
    auto pp=splitTok(t);
    if(pp.empty())return false;
    string c=up(pp[0]);
    if(c!="IF"&&c!="IFE")return false;
    if(pp.size()<4)return false;            // 至少需要 IF lhs op rhs
    string op=pp[2];
    int opPos=-1;
    for(int i=1;i<(int)pp.size();i++)if(pp[i]==op){opPos=i;break;}
    if(opPos<0||opPos+1>=(int)pp.size())return false;
    string action=opPos+2<(int)pp.size()?joinRemain(pp,opPos+2):"";
    return action.empty();                  // 无内联动作 = 块 IF
}

static void execCmd(const string& line,bool& jumped){
    jumped=false;
    string t=tr(line);
    if(t.empty()||t[0]=='#'||t[0]==';')return;
    // 去除行内 ; 注释(支持 #comment 风格, 避免 ; 后续内容被当参数)
    {size_t sc=t.find(';');if(sc!=string::npos)t=tr(t.substr(0,sc));}
    if(t.empty())return;
    // 标签行: name: / :name / :name: / ::name / ::name:  (整行无空格)
    if(t.find(' ')==string::npos && (t.back()==':' || t.front()==':'))return;
    auto parts=splitTok(t);
    if(parts.empty())return;
    string cmd=up(parts[0]);
    auto getArg=[&](int i)->const string&{static string _e;return i<(int)parts.size()?parts[i]:(_e="",_e);};

    // ===== 基础输出 =====
    if(cmd=="SAY"||cmd=="S"||cmd=="PRINT"||cmd=="P"){
        string s=joinRemain(parts,1);
        if(s.empty())return;
        doSay(resolveStr(s),true);
    }else if(cmd=="SAY+"||cmd=="S+"){
        string s=joinRemain(parts,1);
        doSay(resolveStr(s),false);
    }else if(cmd=="NARRATE"||cmd=="N"||cmd=="LN"){
        string s=joinRemain(parts,1);
        doSay("[i][fg:gray]"+resolveStr(s)+"[/][/]");
    }

    // ===== 面板 =====
    else if(cmd=="CLEAR"||cmd=="CLS"){
        S.panels[argPanel(getArg(1))].clear();
    }else if(cmd=="PANEL"||cmd=="USE"||cmd=="SWITCH"){
        string pn=getArg(1);
        if(!pn.empty())S.curPanel=pn;
    }else if(cmd=="PANELNEW"||cmd=="PN"){
        string pn=getArg(1);if(pn.empty())return;
        if(S.panels.find(pn)==S.panels.end()){
            Panel pp;pp.name=pn;S.panels[pn]=pp;
        }
    }else if(cmd=="HIDE"){string pn=getArg(1);if(S.panels.count(pn))S.panels[pn].visible=false;}
    else if(cmd=="SHOW"){string pn=getArg(1);if(S.panels.count(pn))S.panels[pn].visible=true;}
    else if(cmd=="CLEARALL"){for(auto&kv:S.panels)kv.second.clear();}
    else if(cmd=="CURSOR"||cmd=="GOTOXY"){
        Panel&p=S.panels[S.curPanel];
        p.cur=parts.size()>=3?atoiS(getArg(2)):0;
        p.curCol=parts.size()>=2?atoiS(getArg(1)):0;
    }else if(cmd=="BORDER"||cmd=="B"){
        Panel&p=S.panels[S.curPanel];
        p.addTop(string(40,'='));p.add(string(40,'='));
    }else if(cmd=="LOG"){
        string s=joinRemain(parts,1);
        if(S.panels.find("log")==S.panels.end()){Panel pp;pp.name="log";S.panels["log"]=pp;}
        S.panels["log"].add(resolveStr(s));
    }

    // ===== 变量 =====
    else if(cmd=="SET"){
        if(parts.size()>=3){
            string vn=getArg(1);
            int start=2;
            if(parts.size()>start&&parts[start]=="=")start++;
            string rhs=joinRemain(parts,start);
            if(rhs.size()>=2&&rhs.front()=='"'&&rhs.back()=='"'){VARS.setS(vn,stripQuote(rhs));}
            else{
                bool isExpr=false;
                for(char c:rhs)if(c=='+'||c=='-'||c=='*'||c=='/'||c=='%'){isExpr=true;break;}
                if(isExpr)VARS.setI(vn,evalExpr(rhs));
                else{
                    string r=resolveStr(rhs);
                    bool allNum=!r.empty();
                    for(char c:r)if(!isdigit((unsigned char)c)&&c!='-'){allNum=false;break;}
                    if(allNum)VARS.setI(vn,atoll(r.c_str()));else VARS.setS(vn,r);
                }
            }
        }
    }else if(cmd=="ADD"||cmd=="INC"||cmd=="SUB"||cmd=="DEC"){
        if(parts.size()>=3){
            string vn=getArg(1);
            long long delta=evalExpr(getArg(2));
            if(cmd[0]=='S'||cmd[0]=='D')delta=-delta;
            VARS.setI(vn,VARS.getI(vn)+delta);
        }
    }else if(cmd=="RANDOM"||cmd=="RAND"){
        if(parts.size()>=3){
            long long a=evalExpr(getArg(1)),b=evalExpr(getArg(2));
            if(b<a)std::swap(a,b);
            std::uniform_int_distribution<long long>d(a,b);
            static std::mt19937_64 gen((unsigned)time(nullptr));
            long long v=d(gen);
            VARS.setI("random",v);
            if(parts.size()>=4)VARS.setI(getArg(3),v);
        }
    }

    // ===== 流程控制 =====
    else if(cmd=="IF"||cmd=="IFE"){
        if(parts.size()>=4){
            string lhs=getArg(1),op=getArg(2);
            int opPos=-1;
            for(int i=1;i<(int)parts.size();i++){
                if(parts[i]==op){opPos=i;break;}
            }
        if(opPos<0||opPos+1>=(int)parts.size())return;
        string rhsRaw=parts[opPos+1];
        string rhs=resolveStr(rhsRaw);
        string lv=VARS.getS(lhs);
        bool ok=false;
        if(op=="=="||op=="=")ok=(lv==rhs);
        else if(op=="!="||op=="<>")ok=(lv!=rhs);
        else if(op==">")ok=(VARS.getI(lhs)>atoll(rhs.c_str()));
        else if(op=="<")ok=(VARS.getI(lhs)<atoll(rhs.c_str()));
        else if(op==">=")ok=(VARS.getI(lhs)>=atoll(rhs.c_str()));
        else if(op=="<=")ok=(VARS.getI(lhs)<=atoll(rhs.c_str()));
        // 查找内联 ELSE, 支持单行 IF 的 THEN/ELSE 分界 (如 IF x==1 SAY A ELSE SAY B)
        int elsePos=-1;
        for(int i=opPos+2;i<(int)parts.size();i++){
            if(up(parts[i])=="ELSE"){elsePos=i;break;}
        }
        string action=opPos+2<(int)parts.size()?joinRemain(parts,opPos+2):"";
        string thenAct,elseAct;
        if(elsePos>=0){
            for(int i=opPos+2;i<elsePos;i++){if(i>opPos+2)thenAct+=' ';thenAct+=parts[i];}
            thenAct=stripQuote(thenAct);          // 仅拼接 ELSE 之前的部分
            elseAct=joinRemain(parts,elsePos+1);  // ELSE 之后的部分
        }else thenAct=action;
        bool isBlock=action.empty();
        S.ifTaken.push_back((char)(ok?1:0));   // 记录本 IF 是否命中, 供 ELSE/ENDIF 配对
        if(!ok){
            if(isBlock){
                int depth=1;g_ip++;
                while(g_ip<(int)g_script.size()&&depth>0){
                    string u=tr(g_script[g_ip]);auto pp=splitTok(u);
                    if(!pp.empty()){
                        string cc=up(pp[0]);
                        if(isBlockIfLine(g_script[g_ip]))depth++;   // 仅块 IF 计入作用域深度
                        else if(cc=="ENDIF"){depth--;if(depth==0)break;}
                        else if(cc=="ELSE"&&depth==1){break;}   // 落到 ELSE 行, 由 ELSE 处理器决定
                    }
                    g_ip++;
                }
                jumped=true;  // 块内已跳,避免外层 g_ip++
            }else if(elsePos>=0){
                // 单行 IF 不成立且有内联 ELSE -> 执行 ELSE 动作
                bool aj=false; execCmd(elseAct,aj); jumped=aj;
            }
            // 单行 IF 不成立且无内联 ELSE -> 不设 jumped, g_ip 自然 +1
        }else{
            if(!isBlock){
                bool aj=false; execCmd(thenAct,aj); jumped=aj;
            }
        }
    }
    }else if(cmd=="ELSE"){
        bool taken = !S.ifTaken.empty() && S.ifTaken.back()==1;
        if(taken){
            // IF 已命中 THEN 分支 -> 整个 else (内联动作 + else 体) 都应跳过, 跳到 ENDIF
            int depth=1;g_ip++;
            while(g_ip<(int)g_script.size()&&depth>0){
                string u=tr(g_script[g_ip]);auto pp=splitTok(u);
                if(!pp.empty()){
                    string cc=up(pp[0]);
                    if(isBlockIfLine(g_script[g_ip]))depth++;
                    else if(cc=="ENDIF"){depth--;if(depth==0)break;}
                }
                g_ip++;
            }
            jumped=true;
        }else{
            // IF 未命中 -> 执行 else 分支
            if(parts.size()>=2){
                string act=joinRemain(parts,1);
                bool ajumped=false;
                execCmd(act,ajumped);
                if(!ajumped){            // 内联动作未跳转(如 SAY), 仍需跳过到 ENDIF
                    int depth=1;g_ip++;
                    while(g_ip<(int)g_script.size()&&depth>0){
                        string u=tr(g_script[g_ip]);auto pp=splitTok(u);
                        if(!pp.empty()){
                            string cc=up(pp[0]);
                            if(isBlockIfLine(g_script[g_ip]))depth++;
                            else if(cc=="ENDIF"){depth--;if(depth==0)break;}
                        }
                        g_ip++;
                    }
                }
                jumped=true;
            }
            // 块 else (无内联动作): 不跳转, 自然流入 else 体 (由主循环 g_ip++)
        }
    }else if(cmd=="ENDIF"){
        if(!S.ifTaken.empty())S.ifTaken.pop_back();   // 退出一层 IF/ELSE 配对
    }
    else if(cmd=="GOTO"||cmd=="JUMP"){
        if(parts.size()>=2){
            string lbl=getArg(1);
            int idx=findLabelIdx(lbl,g_ip+1);if(idx<0)idx=findLabelIdx(lbl,0);
            if(idx>=0){g_ip=idx;jumped=true;}
        }
    }else if(cmd=="CALL"||cmd=="GOSUB"){
        if(parts.size()>=2){
            string lbl=getArg(1);
            int idx=findLabelIdx(lbl,0);if(idx>=0){g_ip=idx;jumped=true;}
        }
    }

    // ===== 用户交互 =====
    else if(cmd=="CHOICE"||cmd=="MENU"||cmd=="CHOICES"){
        S.choices.clear();S.choiceIdx=0;
        for(size_t i=1;i<parts.size();i++)S.choices.push_back(resolveStr(parts[i]));
        S.waiting=true;S.lastEvent="CHOICE";
    }else if(cmd=="INPUT"||cmd=="ASK"){
        S.inputBuf.clear();S.inputPos=0;
        S.inputMax=parts.size()>=3?atoiS(getArg(2)):64;   // 默认最大 64 字符
        S.inputVar=parts.size()>=2?getArg(1):string("input");
        S.waiting=true;S.lastEvent="INPUT";
    }else if(cmd=="WAIT"||cmd=="SLEEP"||cmd=="PAUSE"){
        int ms=parts.size()>=2?atoiS(getArg(1)):500;
        S.sleepUntil=(int)nowMs()+ms;
    }

    // ===== 效果 =====
    else if(cmd=="TYPESPEED"||cmd=="TYPERATE"){
        S.typeSpeed=parts.size()>=2?atoiS(getArg(1)):20;
    }else if(cmd=="SHAKE"){
        S.shaking=true;
        S.shakeInt=parts.size()>=2?atoiS(getArg(1)):2;
        S.shakeDur=parts.size()>=3?atoiS(getArg(2)):500;
        S.shakeT0=(int)nowMs();
    }else if(cmd=="BLINK"){
        S.blinking=true;
        S.blinkStep=parts.size()>=2?atoiS(getArg(1)):300;
        S.blinkDur=parts.size()>=3?atoiS(getArg(2)):2000;
        S.blinkT0=(int)nowMs();S.blinkOn=true;
    }else if(cmd=="EFFECT"||cmd=="FX"){
        string pn=argPanel(getArg(1));
        string kind=getArg(2);
        int dur=parts.size()>=4?atoiS(getArg(3)):500;
        if(S.panels.count(pn)){Effect e;e.kind=kind;e.dur=dur;S.panels[pn].fx.push_back(e);}
    }else if(cmd=="BEEP"||cmd=="SOUND"){printf("\a");fflush(stdout);}

    // ===== 存档 =====
    else if(cmd=="SAVE"){
        string fn=parts.size()>=2?getArg(1):SAVE_FILE;
        // 脚本 SAVE: 不记 IP, 由脚本作者用 GOTO 决定 LOAD 后续位置
        S.loadResumeIp=-1;
        userSave(fn);
    }else if(cmd=="LOAD"){
        string fn=parts.size()>=2?getArg(1):SAVE_FILE;
        // 脚本 LOAD: 还原变量但不自动跳转, 由脚本显式 GOTO
        userLoad(fn,false);
    }else if(cmd=="USERSAVE"||cmd=="F5"){
        // 脚本中调用相当于 F5 主动存档: 记 IP+1, 供 userLoad 跳回
        string fn=parts.size()>=2?getArg(1):SAVE_FILE;
        S.loadResumeIp=g_ip+1;   // 下一行 IP
        userSave(fn);
    }else if(cmd=="USERLOAD"||cmd=="F9"){
        // 脚本中调用相当于 F9 主动读档: 跳回上次 userSave 的下一行
        string fn=parts.size()>=2?getArg(1):SAVE_FILE;
        if(userLoad(fn,true)){jumped=true;}   // jumped 让主循环下一轮 tick 用 loadResumeIp 跳
    }else if(cmd=="RUN"||cmd=="CHAIN"){
        if(parts.size()>=2){
            string fn=getArg(1);
            std::ifstream f(fn);if(f){
                resetGameState();   // 清空旧剧本的变量/面板/状态
                S.curPanel="main";  // 重置当前面板指针
                g_script.clear();string l;while(getline(f,l))g_script.push_back(l);
                g_ip=0;jumped=true;SCRIPT_FILE=fn;
            }
        }
    }

    // ===== 调试/帮助/退出 =====
    else if(cmd=="DEBUG"){S.debug=!S.debug;}
    else if(cmd=="HELP"||cmd=="?"||cmd=="H"){
        Panel&p=S.panels[S.curPanel];
        p.add("[u][fg:yellow]=== PlotEngine 指令集 ===[/][/]");
        p.add("[b]SAY[/] <text>         显示文本(打字机效果)");
        p.add("[b]NARRATE[/] <text>     灰色旁白");
        p.add("[b]SET[/] v expr         设置变量(int/str/表达式)");
        p.add("[b]ADD/SUB[/] v n        增减");
        p.add("[b]RANDOM[/] a b [v]     随机数存入v");
        p.add("[b]IF[/] v op val        条件");
        p.add("[b]ELSE/ENDIF[/]         分支");
        p.add("[b]GOTO[/] label         跳转");
        p.add("[b]CHOICE[/] a b c...    选项(数字键1-9)");
        p.add("[b]INPUT[/] [var]        玩家输入");
        p.add("[b]WAIT[/] ms            等待");
        p.add("[b]PANEL[/] name         切换面板");
        p.add("[b]PANELNEW[/] name      新建面板");
        p.add("[b]CLEAR[/] [name]       清空面板");
        p.add("[b]SHAKE n ms[/]         抖动效果");
        p.add("[b]BLINK t ms[/]         闪烁效果");
        p.add("[b]SAVE/LOAD[/] [file]   加密存档");
        p.add("[b]DEBUG[/]              切换调试");
    }else if(cmd=="EXIT"||cmd=="QUIT"||cmd=="END"){S.run=false;}
    else if(cmd=="REM"||cmd=="//"){}
    else{
        // 未知: 当作 SAY
        string s=joinRemain(parts,0);
        if(!s.empty())doSay(resolveStr(s),true);
    }
    S.curLine=g_ip;
}

// ============================================================
// 脚本 & 主循环
// ============================================================
static bool loadScript(const string& fn){
    std::ifstream f(fn);if(!f)return false;
    g_script.clear();string l;
    while(getline(f,l))g_script.push_back(l);
    g_ip=0;
    return !g_script.empty();
}
static void tick(long long t){
    // 流程图覆盖层期间冻结游戏: 不推进 IP, 不推进打字机
    if(S.showFlow)return;
    // 读档恢复: 主循环开头先处理 S.loadResumeIp (由 userLoad 设置)
    if(S.loadResumeIp>=0){
        g_ip=S.loadResumeIp;
        S.loadResumeIp=-1;
        S.sleepUntil=0;   // 立即可执行
    }
    if(S.shaking){
        if((int)t-S.shakeT0>=S.shakeDur)S.shaking=false;
        else S.shakeOff=((rand()%(S.shakeInt*2+1))-S.shakeInt);
    }
    if(S.blinking){
        if((int)t-S.blinkT0>=S.blinkDur)S.blinking=false;
        else S.blinkOn=(((int)(t-S.blinkT0)/S.blinkStep)%2)==0;
    }
    if(S.waitType&&S.typeLine>=0){
        // 直接用 doSay 记录的面板,避免遍历查找出错
        Panel* pp=&S.panels[S.typePanel.empty()?S.curPanel:S.typePanel];
        if(!pp||S.typeLine>=(int)pp->lines.size()){
            S.waitType=false;S.typeLine=-1;
        }else{
            string&ln=pp->lines[S.typeLine];
            int total=visibleCharCount(ln);
            if(S.typeSpeed<=0){
                // 即时模式: 一次显示完
                S.typePos=total;
            }else{
                // 基于时间批量推进: 经过的毫秒数 / 单字符毫秒
                long long elapsed=t-S.typeT0;
                int want=(int)(elapsed/S.typeSpeed);
                S.typePos+=want;
                S.typeT0+=want*(long long)S.typeSpeed;
            }
            if(S.typePos>=total){
                S.typePos=total;
                S.waitType=false;S.typeLine=-1;
            }
            S.sleepUntil=(int)t+S.typeSpeed;   // 下次再推进
        }
    }
    if(S.sleepUntil>(int)t){if(S.debug)DLOG("[tick] sleepUntil return, t="+itoa2(t)+" sleepUntil="+itoa2(S.sleepUntil));return;}
    if(S.waiting){if(S.debug)DLOG("[tick] waiting return, choices="+itoa2(S.choices.size()));return;}
    if(g_ip>=(int)g_script.size())return;
    int guard=100;
    while(g_ip<(int)g_script.size()&&S.run&&!S.waiting&&S.sleepUntil<=(int)t&&guard-->0){
        bool jumped=false;
        S.curLine=g_ip;
        if(S.debug)DLOG("[tick] exec ip="+itoa2(g_ip)+" line=["+g_script[g_ip]+"]");
        execCmd(g_script[g_ip],jumped);
        if(!jumped)g_ip++;
        if(S.waitType||S.waiting)break;
    }
}

// ============================================================
// 自检模式: --selftest <script>
//   模拟按 1 走完所有 CHOICE, 模拟填 abc/backspace/x/enter 走 INPUT
//   用于无头环境下回归测试
// ============================================================
static int runSelftest(const char* scriptFile){
    SCRIPT_FILE=scriptFile;
    if(!loadScript(SCRIPT_FILE)){std::cerr<<"无法打开 "<<SCRIPT_FILE<<std::endl;return 1;}
    S.panels["main"]=Panel();
    // selftest 也不默认创建 log 面板
    S.curPanel="main";
    VARS.setI("choice",0);
    // 跑 5000 步,记录日志
    S.typeSpeed=0; // 即时显示
    DLOG("[selftest] start script="+SCRIPT_FILE);
    for(int step=0;step<5000;step++){
        if(g_ip>=(int)g_script.size()){DLOG("[selftest] END ip="+itoa2(g_ip));break;}
        // selftest 防死循环: 检测 IP 序列周期性重复
        static int ipHist[20]={0};static int ipHistIdx=0;static int cycleDet=0;
        int prev=ipHist[ipHistIdx];
        ipHist[ipHistIdx]=g_ip;
        ipHistIdx=(ipHistIdx+1)%20;
        if(prev==g_ip&&g_ip!=0)cycleDet++;
        else cycleDet=0;
        if(cycleDet>=15){DLOG("[selftest] ABORT: 死循环 ip="+itoa2(g_ip)+" 重复 "+itoa2(cycleDet)+" 次");break;}
        DLOG("[selftest] step="+itoa2(step)+" ip="+itoa2(g_ip)+
             " line=["+g_script[g_ip]+"]"+
             " waiting="+itoa2(S.waiting)+
             " waitType="+itoa2(S.waitType));
        // 如果遇到 CHOICE,模拟选第一个
        string t1=tr(g_script[g_ip]);
        if(t1.find("CHOICE")==0||t1.find("MENU")==0){
            bool jumped=false;
            execCmd(g_script[g_ip],jumped);
            DLOG("[selftest] simulated choice 1");
            VARS.setI("choice",1);
            S.choices.clear();
            S.waiting=false;S.lastEvent="";
            flushTypewriter();
            g_ip++;
            continue;
        }
        // 如果遇到 INPUT,模拟填默认值并提交(不超过 inputMax)
        if(t1.find("INPUT")==0||t1.find("ASK")==0){
            bool jumped=false;
            execCmd(g_script[g_ip],jumped);
            // 严格模拟键序列: 'a' 'b' 'c' Backspace 'x' Enter
            // 验证 Event::Backspace 和 Event::Return 路径
            S.inputBuf.clear();S.inputPos=0;
            auto pressChar=[&](char c){
                if(c==127||c==8||c=='\b'){
                    if(S.inputPos>0){S.inputBuf.erase(S.inputPos-1,1);S.inputPos--;}
                }else if(c=='\r'||c=='\n'){
                    VARS.setS(S.inputVar,S.inputBuf);
                    S.waiting=false;S.lastEvent="";
                    S.inputBuf.clear();S.inputPos=0;
                    flushTypewriter();
                }else if(c>=32&&c<127){
                    if((int)S.inputBuf.size()<S.inputMax){
                        S.inputBuf.insert(S.inputBuf.begin()+S.inputPos,c);
                        S.inputPos++;
                    }
                }
            };
            pressChar('a');pressChar('b');pressChar('c');
            DLOG("[selftest] after abc buf=["+S.inputBuf+"] pos="+itoa2(S.inputPos));
            pressChar(127);  // backspace
            DLOG("[selftest] after bsp buf=["+S.inputBuf+"] pos="+itoa2(S.inputPos));
            pressChar('x');
            DLOG("[selftest] after x   buf=["+S.inputBuf+"] pos="+itoa2(S.inputPos));
            pressChar('\r');  // enter (commit)
            DLOG("[selftest] simulated input "+S.inputVar+"=[abx]");
            g_ip++;
            continue;
        }
        bool jumped=false;
        // selftest 也需处理 loadResumeIp (真实模式由 tick() 开头处理)
        if(S.loadResumeIp>=0){g_ip=S.loadResumeIp;S.loadResumeIp=-1;}
        execCmd(g_script[g_ip],jumped);
        if(!jumped)g_ip++;
        if(S.waitType){
            // 立即完成
            Panel&p=S.panels[S.curPanel];
            if(S.typeLine>=0&&S.typeLine<(int)p.lines.size()){
                S.typePos=visibleCharCount(p.lines[S.typeLine]);
            }
            S.waitType=false;S.typeLine=-1;
        }
    }
    DLOG("[selftest] done. panels:");
    for(auto&kv:S.panels){
        for(auto&ln:kv.second.lines)DLOG("  ["+kv.first+"] "+ln);
    }
    // 面板滚动自检: 直接调用 handlePanelScroll (真实事件逻辑) + 完整 renderAll() 渲染
    {
        g_termH=40;
        Panel& pp=S.panels[S.curPanel];
        // 灌入 50 行, 置 scroll=10, follow=true, 模拟"在底部附近"
        pp.lines.clear(); pp.fx.clear(); pp.scroll=10; pp.follow=true;
        for(int i=0;i<50;i++)pp.lines.push_back("[fg:cyan]日志行 "+itoa2(i)+"[/] 这是一段用于测试滚动的较长文本内容");
        // 模拟用户按 ↑: 事件逻辑应把 scroll 减 1 并暂停 follow
        bool handled=handlePanelScroll(Event::ArrowUp);
        int afterUp=pp.scroll;
        bool followAfter=pp.follow;
        // 渲染按 ↑ 前后的输出
        auto renderAt=[&](int sc)->std::string{
            pp.scroll=sc; pp.follow=true;
            Element doc=renderAll();
            auto screen=Screen::Create(Dimension::Fixed(100), Dimension::Fixed(40));
            Render(screen, doc);
            return screen.ToString();
        };
        std::string before=renderAt(10);
        std::string after=renderAt(afterUp);
        bool diff=(before!=after);
        std::cerr<<"PANEL_EVENT_TEST handled="<<(handled?"YES":"NO")
                 <<" scrollBefore=10 scrollAfter="<<afterUp
                 <<" followAfter="<<(followAfter?"true":"false")
                 <<" RENDER_DIFFERENT="<<(diff?"YES":"NO")<<std::endl;
        DLOG(std::string("[selftest] PANEL_EVENT_TEST handled=")+(handled?"YES":"NO")+
             " scrollAfter="+itoa2(afterUp)+" RENDER_DIFFERENT="+(diff?"YES":"NO"));
        // 子测试: CHOICE 状态下 ↑ 也应滚动面板(守卫已移除)
        S.choices.clear();
        for(int i=0;i<3;i++)S.choices.push_back("选项"+itoa2(i));
        S.waiting=true; S.lastEvent="CHOICE";
        int scBefore=(int)pp.lines.size(); pp.scroll=scBefore; pp.follow=true;
        bool handled2=handlePanelScroll(Event::ArrowUp);
        int scAfter=pp.scroll;
        std::cerr<<"PANEL_EVENT_TEST_DURING_CHOICE handled="<<(handled2?"YES":"NO")
                 <<" scrollBefore="<<scBefore<<" scrollAfter="<<scAfter<<std::endl;
        S.choices.clear(); S.waiting=false; S.lastEvent="";
    }
    return 0;
}

int main(int argc,char**argv){
#if defined(_WIN32)
    // 统一控制台输入/输出代码页为 UTF-8。否则中文在读写控制台时会被按系统
    // 代码页 (通常为 GBK/CP936) 转码, 产生乱码。FTXUI 自身会在渲染时设置
    // 输出代码页, 这里在程序启动即设置输入+输出, 覆盖所有可能的读写路径。
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif
    srand((unsigned)time(nullptr));
    SCRIPT_FILE=argc>1?argv[1]:"demo.txt";
    SAVE_FILE=argc>2?argv[2]:"save.txt";
    // 自检模式: --selftest <script> - 模拟按键走完整脚本
    if(argc>=2&&string(argv[1])=="--selftest"){
        if(argc<3){std::cerr<<"用法: "<<argv[0]<<" --selftest <script.txt>"<<std::endl;return 1;}
        return runSelftest(argv[2]);
    }
    if(!loadScript(SCRIPT_FILE)){
        std::cerr<<"无法打开脚本: "<<SCRIPT_FILE<<std::endl;
        std::cerr<<"用法: "<<argv[0]<<" <script.txt> [save.txt]"<<std::endl;
        return 1;
    }
    S.panels["main"]=Panel();
    // 不再默认创建 log 面板,LOG 命令按需创建
    S.curPanel="main";
    VARS.setI("random",0);
    VARS.setS("input","");
    VARS.setI("choice",0);

    auto screen=ScreenInteractive::Fullscreen();
    auto comp=Renderer([&]()->Element{
        static int frameNo=0;frameNo++;
        g_termH=screen.dimy();   // 每帧更新终端高度, 供面板滚动估算可见行数
        long long t=nowMs();
        // 仅调试模式或每 60 帧输出,避免每帧刷盘导致 27MB+ 日志
        if(S.debug || frameNo%60==0){
            DLOG("[render] frame="+itoa2(frameNo)+" ip="+itoa2(g_ip)+
                 " waitType="+itoa2(S.waitType)+" waiting="+itoa2(S.waiting)+
                 " choices="+itoa2(S.choices.size())+
                 " sleepUntil="+itoa2(S.sleepUntil));
        }
        tick(t);
        Element doc;
        if(S.showFlow){
            // 流程图覆盖层: 冻结游戏画面, 只渲染流程图
            // (tick() 仍会被调用, 但 showFlow=true 期间不推进 IP, 见 tick() 入口)
            doc=renderFlow();
        }else{
            Element main=renderAll();
            Element dbg=renderDebugBar();
            Element inp=renderInputBox();
            if(S.debug)doc=vbox({main|flex,inp,dbg});
            else doc=vbox({main|flex,inp});
        }
        // 如果还有进行中的动画/打字机/等待,请求 FTXUI 在下一帧再次渲染
        bool needFrame=S.waitType||S.shaking||S.blinking||
                       S.sleepUntil>(int)nowMs()||S.waiting||
                       (g_ip<(int)g_script.size()&&S.run);
        if(needFrame){
            screen.RequestAnimationFrame();
        }
        return doc;
    });
    comp|=CatchEvent([&](Event ev)->bool{
        S.lastKey=ev.input();
        DLOG(string("[ev] is_char=")+((ev.is_character())?"1":"0")+
             " is_mouse="+((ev.is_mouse())?"1":"0")+
             (ev.is_mouse()?(" btn="+itoa2(ev.mouse().button)+
                             " motion="+itoa2((int)ev.mouse().motion)+
                             " x="+itoa2(ev.mouse().x)+" y="+itoa2(ev.mouse().y)):"")+
             " wheelCount="+itoa2(S.wheelCount)+
             " waiting="+itoa2(S.waiting)+
             " choices="+itoa2(S.choices.size())+
             " input=["+ev.input()+"]");
        if(ev.is_mouse()){
            auto&m=ev.mouse();
            char buf[128];
            sprintf(buf,"(%d,%d) btn=%d %s",m.x,m.y,m.button,m.motion==Mouse::Pressed?"D":"U");
            S.lastMouse=buf;
            // A. 鼠标滚轮: 流程图打开时滚流程图, 否则滚面板(复用键盘滚动逻辑)
            if(m.button==Mouse::WheelUp||m.button==Mouse::WheelDown){
                S.wheelCount++;   // [诊断] 确认滚轮事件是否到达本回调
                if(S.showFlow){
                    if(m.button==Mouse::WheelUp){if(S.flowScroll>0)S.flowScroll--;}
                    else S.flowScroll++;
                }else{
                    handlePanelScroll(m.button==Mouse::WheelUp?Event::ArrowUp:Event::ArrowDown);
                }
                return true;
            }
            // B. 鼠标左键按下: 命中选项框则选中并确认(与数字键/回车一致)
            if(m.button==Mouse::Left && m.motion==Mouse::Pressed){
                if(!S.choices.empty() && S.choiceBox.x_max>S.choiceBox.x_min
                   && S.choiceBox.Contain(m.x,m.y)){
                    // 选项框结构: 边框顶线(y_min) + 标题行(y_min+1) + 选项0(y_min+2)...
                    int idx=m.y-(S.choiceBox.y_min+2);
                    if(idx>=0 && idx<(int)S.choices.size()){
                        S.choiceIdx=idx;
                        VARS.setI("choice",idx+1);
                        S.choices.clear();
                        S.waiting=false;S.lastEvent="";
                        flushTypewriter();
                        DLOG("[ev] mouse click -> choice="+itoa2(idx+1));
                        return true;
                    }
                }
            }
            return true; // 消费所有鼠标事件, 避免误触发键盘逻辑
        }
        // 全局快捷
        if(ev==Event::Escape){
            // 流程图打开时: Esc 仅关闭流程图, 不退出游戏
            if(S.showFlow){S.showFlow=false;return true;}
            S.run=false;screen.Exit();return true;
        }
        if(ev==Event::F2){S.debug=!S.debug;return true;}
        if(ev==Event::F5){userSave(SAVE_FILE);return true;}     // F5 = 立即存档
        if(ev==Event::F9){userLoad(SAVE_FILE,true);return true;} // F9 = 立即读档 (跳回存档点的下一行)
        // 流程图: 用户按 F 触发(不区分大小写). 优先级放在 INPUT 块之前,
        //        这样 INPUT 模式下 f/F 不会作为输入字符
        if(ev==Event::Character('f')||ev==Event::Character('F')){
            S.showFlow=!S.showFlow;
            S.flowScroll=0;
            g_flowDirty=true;   // 重新计算当前 IP 高亮
            if(S.showFlow)flushTypewriter();   // 弹流程图前先刷完打字机
            return true;
        }
        // 流程图打开时: 拦截滚动键
        if(S.showFlow){
            if(ev==Event::ArrowUp){
                if(S.flowScroll>0)S.flowScroll--;
                return true;
            }
            if(ev==Event::ArrowDown){
                S.flowScroll++;
                return true;
            }
            if(ev==Event::PageUp){
                S.flowScroll-=10;
                if(S.flowScroll<0)S.flowScroll=0;
                return true;
            }
            if(ev==Event::PageDown){
                S.flowScroll+=10;
                return true;
            }
            if(ev==Event::Home){
                S.flowScroll=0;
                return true;
            }
            if(ev==Event::End){
                // 跳到当前 IP 节点所在行
                if(g_flowCurrentIdx>=0){
                    S.flowScroll=g_flowCurrentIdx*2-2;  // 留 2 行上下文
                    if(S.flowScroll<0)S.flowScroll=0;
                }
                return true;
            }
        }

        // 面板内容滚动: 始终优先(含 CHOICE/INPUT 期间), UP/DOWN 在任何状态都能翻看面板
        if(handlePanelScroll(ev))return true;

        // 1) 方向键: 切换选项(优先于 is_character)
        if(!S.choices.empty()){
            if(ev==Event::ArrowUp){
                S.choiceIdx=(S.choiceIdx-1+(int)S.choices.size())%(int)S.choices.size();
                DLOG("[ev] ArrowUp -> idx="+itoa2(S.choiceIdx));
                return true;
            }
            if(ev==Event::ArrowDown){
                S.choiceIdx=(S.choiceIdx+1)%(int)S.choices.size();
                DLOG("[ev] ArrowDown -> idx="+itoa2(S.choiceIdx));
                return true;
            }
            // Enter/Return
            if(ev==Event::Return){
                VARS.setI("choice",S.choiceIdx+1);
                S.choices.clear();
                S.waiting=false;S.lastEvent="";
                flushTypewriter();
                DLOG("[ev] Return -> choice="+itoa2(S.choiceIdx+1));
                return true;
            }
        }
        // INPUT 模式: 左右键移动光标 / 回车提交 / 退格删除 (Special 事件, 必须在 is_character 块外)
        if(S.waiting&&S.lastEvent=="INPUT"){
            if(ev==Event::ArrowLeft){
                if(S.inputPos>0)S.inputPos--;
                return true;
            }
            if(ev==Event::ArrowRight){
                if(S.inputPos<(int)S.inputBuf.size())S.inputPos++;
                return true;
            }
            if(ev==Event::Home){S.inputPos=0;return true;}
            if(ev==Event::End){S.inputPos=(int)S.inputBuf.size();return true;}
            if(ev==Event::Return||ev==Event::Backspace){
                if(ev==Event::Return){
                    // 回车提交
                    VARS.setS(S.inputVar,S.inputBuf);
                    S.waiting=false;S.lastEvent="";
                    S.inputBuf.clear();S.inputPos=0;
                    flushTypewriter();
                    DLOG("[ev] INPUT Return commit");
                    return true;
                }
                // Backspace 删除光标前一字符
                if(S.inputPos>0){S.inputBuf.erase(S.inputPos-1,1);S.inputPos--;}
                return true;
            }
        }

        if(ev.is_character()){
            char c=ev.character().empty()?0:ev.character()[0];
            DLOG("[ev] char=["+string(1,c)+"] choices="+itoa2(S.choices.size())+" waiting="+itoa2(S.waiting));
            // 选项 - 数字键 1-9
            if(!S.choices.empty()){
                if(c>='1'&&c<='9'){
                    int idx=c-'1';
                    if(idx<(int)S.choices.size()){
                        S.choiceIdx=idx;
                        VARS.setI("choice",idx+1);
                        S.choices.clear();
                        S.waiting=false;S.lastEvent="";
                        flushTypewriter();
                        DLOG("[ev] choice num="+itoa2(idx+1));
                        return true;
                    }
                }
                // Enter 字符也接受
                if(c=='\r'||c=='\n'){
                    VARS.setI("choice",S.choiceIdx+1);
                    S.choices.clear();
                    S.waiting=false;S.lastEvent="";
                    flushTypewriter();
                    DLOG("[ev] choice enter -> choice="+itoa2(S.choiceIdx+1));
                    return true;
                }
                // 吸收其它按键,避免误触发
                return true;
            }
            // 打字机进行中: 空格/回车 = 立即显示完
            if(S.waitType&&S.typeLine>=0&&(c==' '||c=='\r'||c=='\n')){
                flushTypewriter();
                return true;
            }
            // 输入
            if(S.waiting&&S.lastEvent=="INPUT"){
                if(c=='\r'||c=='\n'){
                    VARS.setS(S.inputVar,S.inputBuf);
                    S.waiting=false;S.lastEvent="";
                    S.inputBuf.clear();S.inputPos=0;
                    flushTypewriter();
                    return true;
                }else if(c==127||c==8||c=='\b'){
                    if(S.inputPos>0){S.inputBuf.erase(S.inputPos-1,1);S.inputPos--;}
                    return true;
                }else if(c==21){// Ctrl-U 清空
                    S.inputBuf.clear();S.inputPos=0;
                    return true;
                }else if(c>=32&&c<127){
                    if((int)S.inputBuf.size()<S.inputMax){
                        S.inputBuf.insert(S.inputBuf.begin()+S.inputPos,c);
                        S.inputPos++;
                    }
                    return true;
                }
            }
            if(c=='`'){S.debug=!S.debug;return true;}
            if(c=='Q'||c=='q'){S.run=false;screen.Exit();return true;}
        }
        return false;
    });
    screen.Loop(comp);
    return 0;
}

