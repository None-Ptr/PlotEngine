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
