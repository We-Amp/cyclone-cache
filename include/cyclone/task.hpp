// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>
#include <variant>

namespace cyclone {

template <typename T>
class Task;

namespace detail {

struct FinalAwaiter {
  [[nodiscard]] bool await_ready() const noexcept { return false; }

  template <typename Promise>
  std::coroutine_handle<> await_suspend(
      std::coroutine_handle<Promise> h) noexcept {
    auto &promise = h.promise();
    if (promise.continuation) {
      return promise.continuation;
    }
    return std::noop_coroutine();
  }

  void await_resume() noexcept {}
};

template <typename T>
struct TaskPromise {
  std::variant<std::monostate, T, std::exception_ptr> result;
  std::coroutine_handle<> continuation;

  Task<T> get_return_object();

  std::suspend_always initial_suspend() noexcept { return {}; }

  FinalAwaiter final_suspend() noexcept { return {}; }

  void return_value(T value) { result.template emplace<1>(std::move(value)); }

  void unhandled_exception() {
    result.template emplace<2>(std::current_exception());
  }

  T &get_value() {
    if (std::holds_alternative<std::exception_ptr>(result)) {
      std::rethrow_exception(std::get<std::exception_ptr>(result));
    }
    return std::get<T>(result);
  }
};

template <>
struct TaskPromise<void> {
  std::exception_ptr exception;
  std::coroutine_handle<> continuation;

  Task<void> get_return_object();

  std::suspend_always initial_suspend() noexcept { return {}; }

  FinalAwaiter final_suspend() noexcept { return {}; }

  void return_void() {}

  void unhandled_exception() { exception = std::current_exception(); }

  void get_value() const {
    if (exception) {
      std::rethrow_exception(exception);
    }
  }
};

}  // namespace detail

template <typename T = void>
class Task {
 public:
  using promise_type = detail::TaskPromise<T>;
  using handle_type = std::coroutine_handle<promise_type>;

  Task() noexcept : _handle(nullptr) {}

  explicit Task(handle_type h) noexcept : _handle(h) {}

  Task(Task &&other) noexcept
      : _handle(std::exchange(other._handle, nullptr)) {}

  Task &operator=(Task &&other) noexcept {
    if (this != &other) {
      if (_handle) {
        _handle.destroy();
      }
      _handle = std::exchange(other._handle, nullptr);
    }
    return *this;
  }

  Task(const Task &) = delete;
  Task &operator=(const Task &) = delete;

  ~Task() {
    if (_handle) {
      _handle.destroy();
    }
  }

  [[nodiscard]] bool await_ready() const noexcept {
    return !_handle || _handle.done();
  }

  std::coroutine_handle<> await_suspend(
      std::coroutine_handle<> continuation) noexcept {
    _handle.promise().continuation = continuation;
    return _handle;
  }

  T await_resume() { return std::move(_handle.promise().get_value()); }

  [[nodiscard]] bool done() const { return !_handle || _handle.done(); }

  void resume() {
    if (_handle && !_handle.done()) {
      _handle.resume();
    }
  }

  T sync_wait() {
    while (!done()) {
      resume();
    }
    return await_resume();
  }

  [[nodiscard]] handle_type handle() const noexcept { return _handle; }

 private:
  handle_type _handle;
};

template <>
inline void Task<void>::await_resume() {
  _handle.promise().get_value();
}

template <>
inline void Task<void>::sync_wait() {
  while (!done()) {
    resume();
  }
  await_resume();
}

namespace detail {

template <typename T>
Task<T> TaskPromise<T>::get_return_object() {
  return Task<T>{std::coroutine_handle<TaskPromise<T>>::from_promise(*this)};
}

inline Task<void> TaskPromise<void>::get_return_object() {
  return Task<void>{
      std::coroutine_handle<TaskPromise<void>>::from_promise(*this)};
}

}  // namespace detail

struct SuspendAlways {
  [[nodiscard]] bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<>) const noexcept {}
  void await_resume() const noexcept {}
};

template <typename T>
struct ReadyAwaiter {
  T value;

  [[nodiscard]] bool await_ready() const noexcept { return true; }
  void await_suspend(std::coroutine_handle<>) const noexcept {}
  T await_resume() { return std::move(value); }
};

template <typename T>
ReadyAwaiter<T> make_ready(T value) {
  return ReadyAwaiter<T>{std::move(value)};
}

}  // namespace cyclone
