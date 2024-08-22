#include <iostream>
#include <vector>
#include <mutex>
#include <coroutine>

#ifdef TELEMETRY
struct ExecutorStats {
  size_t total_awaitable_ready = 0;
  size_t total_awaitable_need_to_wait = 0;
  size_t total_immediate_ready = 0;
};

struct Executor {
  ExecutorStats stats;
};

Executor& Executor() {
  static Executor executor;
  return executor;
}
#endif

template <typename RETVAL>
struct CoroutineRetvalHolder {
  std::mutex mut;
  bool returned = false;
  RETVAL value;
  std::vector<std::coroutine_handle<>> to_resume;
};

template <>
struct CoroutineRetvalHolder<void> {
  std::mutex mut;
  bool returned = false;
  std::vector<std::coroutine_handle<>> to_resume;
};

template <typename RETVAL>
struct CoroutineAwaitResume {
    CoroutineRetvalHolder<RETVAL>* pself;
    RETVAL immediate_value;
    bool is_immediate;

    CoroutineAwaitResume(RETVAL immediate) 
        : pself(nullptr), immediate_value(std::move(immediate)), is_immediate(true) {}

    explicit CoroutineAwaitResume(CoroutineRetvalHolder<RETVAL>& self) 
        : pself(&self), is_immediate(false) {}

    RETVAL await_resume() const noexcept {
        if (!is_immediate) {
            std::lock_guard<std::mutex> lock(pself->mut);
            if (!pself->returned) {
                // Internal error: `await_resume()` should only be called once the result is available.
                std::terminate();
            }
            return pself->value;
        } else {
            return immediate_value;
        }
    }
};

template <>
struct CoroutineAwaitResume<void> {
    CoroutineRetvalHolder<void>* pself;
    bool is_immediate;

    CoroutineAwaitResume() 
        : pself(nullptr), is_immediate(true) {}

    explicit CoroutineAwaitResume(CoroutineRetvalHolder<void>& self) 
        : pself(&self), is_immediate(false) {}

    void await_resume() const noexcept {
        if (!is_immediate) {
            std::lock_guard<std::mutex> lock(pself->mut);
            if (!pself->returned) {
                // Internal error: `await_resume()` should only be called once the result is available.
                std::terminate();
            }
        }
    }
};

template <typename RETVAL = void>
struct Async : CoroutineAwaitResume<RETVAL> {
    using CoroutineAwaitResume<RETVAL>::CoroutineAwaitResume;

    bool await_ready() const noexcept {
        if (!this->is_immediate) {
            std::lock_guard<std::mutex> lock(this->pself->mut);
#ifndef TELEMETRY
            return this->pself->returned;
#else
            if (this->pself->returned) {
                ++Executor().stats.total_awaitable_ready;
                return true;
            } else {
                ++Executor().stats.total_awaitable_need_to_wait;
                return false;
            }
#endif
        } else {
#ifdef TELEMETRY
            ++Executor().stats.total_immediate_ready;
#endif
            return true;
        }
    }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        if (!this->is_immediate) {
            std::lock_guard<std::mutex> lock(this->pself->mut);
            if (this->pself->returned) {
                h.resume();
            } else {
                this->pself->to_resume.push_back(h);
            }
        } else {
            std::cout << "FATAL: Should never attempt to `await_suspend` an immediate value." << std::endl;
            std::terminate();
        }
    }
};

// Forward declaration of Task
template <typename T = void>
struct Task;

template <typename T = void>
struct Promise {
  std::suspend_never initial_suspend() { return {}; }
  std::suspend_never final_suspend() noexcept { return {}; }
  void unhandled_exception() { std::terminate(); }
  CoroutineRetvalHolder<T> holder;
  
  Task<T> get_return_object();

  void return_value(T value) {
    std::lock_guard<std::mutex> lock(holder.mut);
    holder.value = std::move(value);
    holder.returned = true;
    for (auto& h : holder.to_resume) {
      h.resume();
    }
  }
};

template <>
struct Promise<void> {
  std::suspend_never initial_suspend() { return {}; }
  std::suspend_never final_suspend() noexcept { return {}; }
  void unhandled_exception() { std::terminate(); }
  CoroutineRetvalHolder<void> holder;
  
  Task<void> get_return_object();

  void return_void() {
    std::lock_guard<std::mutex> lock(holder.mut);
    holder.returned = true;
    for (auto& h : holder.to_resume) {
      h.resume();
    }
  }
};

template <typename T>
struct [[nodiscard]] Task {
  using promise_type = Promise<T>;
  std::coroutine_handle<promise_type> handle;

  Task(std::coroutine_handle<promise_type> h) : handle(h) {}
  Task(const Task&) = delete;
  Task(Task&& rhs) : handle(rhs.handle) { rhs.handle = nullptr; }
  ~Task() {
    if (handle) handle.destroy();
  }

  bool await_ready() const noexcept {
    return handle.promise().holder.returned;
  }

  void await_suspend(std::coroutine_handle<> h) noexcept {
    std::lock_guard<std::mutex> lock(handle.promise().holder.mut);
    if (handle.promise().holder.returned) {
      h.resume();
    } else {
      handle.promise().holder.to_resume.push_back(h);
    }
  }

  T await_resume() const {
    if constexpr (std::is_same_v<T, void>) {
      return;
    } else {
      return handle.promise().holder.value;
    }
  }
};

// Define get_return_object after Task is fully defined
template <typename T>
Task<T> Promise<T>::get_return_object() {
  return Task<T>{std::coroutine_handle<Promise<T>>::from_promise(*this)};
}

Task<void> Promise<void>::get_return_object() {
  return Task<void>{std::coroutine_handle<Promise<void>>::from_promise(*this)};
}

Task<int> foo() {
  co_return 42;
}

Task<> bar() {
  co_await foo();
  co_return;
}

int main() {
  bar().handle.resume();
  return 0;
}
