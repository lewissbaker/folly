/*
 * Copyright 2018-present Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <folly/Try.h>
#include <folly/executors/ManualExecutor.h>
#include <folly/experimental/coro/Task.h>
#include <folly/experimental/coro/Traits.h>
#include <folly/experimental/coro/ViaIfAsync.h>
#include <folly/experimental/coro/detail/Traits.h>
#include <folly/fibers/Baton.h>
#include <folly/synchronization/Baton.h>

#include <cassert>
#include <exception>
#include <experimental/coroutine>
#include <type_traits>
#include <utility>

namespace folly {
namespace coro {

namespace detail {

template <typename T>
class BlockingWaitTask;

template <typename T>
class BlockingWaitPromise {
 public:
  using StorageType =
      detail::decay_rvalue_reference_t<detail::lift_lvalue_reference_t<T>>;

  BlockingWaitPromise() noexcept = default;

  ~BlockingWaitPromise() = default;

  template <typename SuspendPointHandle>
  BlockingWaitTask<T> get_return_object(SuspendPointHandle sp) noexcept;

  std::experimental::continuation_handle done() noexcept {
    baton_.post();
    return std::experimental::noop_continuation();
  }

  void unhandled_exception() noexcept {
    assert(result_ != nullptr);
    result_->emplaceException(
        folly::exception_wrapper::from_exception_ptr(std::current_exception()));
  }

  template <
      typename U,
      std::enable_if_t<std::is_convertible<U, T>::value, int> = 0>
  void return_value(U&& value) noexcept(
      std::is_nothrow_constructible<T, U&&>::value) {
    assert(result_ != nullptr);
    result_->emplace(static_cast<U&&>(value));
  }

  template <typename U = T, std::enable_if_t<std::is_void_v<U>, int> = 0>
  void return_void() {
    assert(result_ != nullptr);
    result_->emplace();
  }

  void setTry(folly::Try<StorageType>* result) noexcept {
    result_ = result;
  }

  bool ready() const noexcept {
    return baton_.ready();
  }

  void wait() noexcept {
    baton_.wait();
  }

 private:
  folly::fibers::Baton baton_;
  folly::Try<StorageType>* result_ = nullptr;
};

template <typename T>
class BlockingWaitTask {
 public:
  using StorageType =
      detail::decay_rvalue_reference_t<detail::lift_lvalue_reference_t<T>>;
  using promise_type = BlockingWaitPromise<T>;
  using handle_t = std::experimental::suspend_point_handle<
      std::experimental::with_resume,
      std::experimental::with_destroy,
      std::experimental::with_promise<promise_type>>;

  BlockingWaitTask() noexcept = default;

  explicit BlockingWaitTask(handle_t coro) noexcept : coro_(coro) {}

  BlockingWaitTask(BlockingWaitTask&& other) noexcept
      : coro_(std::exchange(other.coro_, {})) {}

  BlockingWaitTask& operator=(BlockingWaitTask&& other) noexcept = delete;

  ~BlockingWaitTask() {
    if (coro_) {
      coro_.destroy();
    }
  }

  folly::Try<StorageType> getAsTry() && {
    folly::Try<StorageType> result;
    auto& promise = coro_.promise();
    promise.setTry(&result);
    coro_.resume()();
    promise.wait();
    return result;
  }

  T get() && {
    return std::move(*this).getAsTry().value();
  }

  T getVia(folly::DrivableExecutor* executor) && {
    folly::Try<StorageType> result;
    auto& promise = coro_.promise();
    promise.setTry(&result);
    coro_.resume()();
    while (!promise.ready()) {
      executor->drive();
    }
    return std::move(result).value();
  }

 private:
  handle_t coro_;
};

template <typename T>
template <typename SuspendPointHandle>
BlockingWaitTask<T> BlockingWaitPromise<T>::get_return_object(
    SuspendPointHandle sp) noexcept {
  return BlockingWaitTask<T>{sp};
}

template <
    typename Awaitable,
    typename Result = decay_rvalue_reference_t<await_result_t<Awaitable>>>
BlockingWaitTask<Result> makeBlockingWaitTask(Awaitable&& awaitable) {
  co_return co_await static_cast<Awaitable&&>(awaitable);
}

} // namespace detail

/// blockingWait(Awaitable&&) -> await_result_t<Awaitable>
///
/// This function co_awaits the passed awaitable object and blocks the current
/// thread until the operation completes.
///
/// This is useful for launching an asynchronous operation from the top-level
/// main() function or from unit-tests.
///
/// WARNING:
/// Avoid using this function within any code that might run on the thread
/// of an executor as this can potentially lead to deadlock if the operation
/// you are waiting on needs to do some work on that executor in order to
/// complete.
template <typename Awaitable>
auto blockingWait([[maybe_unused]] Awaitable&& awaitable)
    -> detail::decay_rvalue_reference_t<await_result_t<Awaitable>> {
  return detail::makeBlockingWaitTask(static_cast<Awaitable&&>(awaitable))
      .get();
}

template <
    typename SemiAwaitable,
    std::enable_if_t<!is_awaitable_v<SemiAwaitable>, int> = 0>
auto blockingWait(SemiAwaitable&& awaitable)
    -> detail::decay_rvalue_reference_t<semi_await_result_t<SemiAwaitable>> {
  folly::ManualExecutor executor;
  return detail::makeBlockingWaitTask(
             folly::coro::co_viaIfAsync(
                 folly::getKeepAliveToken(executor),
                 static_cast<SemiAwaitable&&>(awaitable)))
      .getVia(&executor);
}

} // namespace coro
} // namespace folly
