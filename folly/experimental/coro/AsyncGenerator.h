/*
 * Copyright 2019-present Facebook, Inc.
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

#include <folly/Traits.h>
#include <folly/experimental/coro/CurrentExecutor.h>
#include <folly/experimental/coro/Utils.h>
#include <folly/experimental/coro/ViaIfAsync.h>
#include <folly/experimental/coro/detail/ManualLifetime.h>

#include <glog/logging.h>

#include <experimental/coroutine>
#include <iterator>
#include <type_traits>

namespace folly {
namespace coro {

template <typename Reference, typename Value>
class AsyncGenerator;

namespace detail {

template <typename Reference, typename Value>
class AsyncGeneratorPromise {
  class YieldAwaiter {
   public:
    bool await_ready() noexcept {
      return false;
    }

    template <typename SuspendPointHandle>
    std::experimental::continuation_handle await_suspend(
        SuspendPointHandle sp) noexcept {
      auto& promise = sp.promise();
      promise.suspendPoint_ = sp;
      return std::exchange(promise.continuation_, {});
    }

    void await_resume() noexcept {}
  };

 public:
  ~AsyncGeneratorPromise() {
    if (hasValue_) {
      value_.destruct();
    }
  }

  template <typename SuspendPointHandle>
  AsyncGenerator<Reference, Value> get_return_object(
      SuspendPointHandle sp) noexcept;

  auto done() noexcept {
    DCHECK(continuation_);
    return continuation_;
  }

  YieldAwaiter yield_value(Reference&& value) noexcept(
      std::is_nothrow_move_constructible<Reference>::value) {
    DCHECK(!hasValue_);
    value_.construct(static_cast<Reference&&>(value));
    hasValue_ = true;
    executor_.reset();
    return YieldAwaiter{};
  }

  // In the case where 'Reference' is not actually a reference-type we
  // allow implicit conversion from the co_yield argument to Reference.
  // However, we don't want to allow this for cases where 'Reference' _is_
  // a reference because this could result in the reference binding to a
  // temporary that results from an implicit conversion.
  template <
      typename U,
      std::enable_if_t<
          !std::is_reference_v<Reference> &&
              std::is_convertible_v<U&&, Reference>,
          int> = 0>
  YieldAwaiter yield_value(U&& value) noexcept(
      std::is_nothrow_constructible_v<Reference, U>) {
    DCHECK(!hasValue_);
    value_.construct(static_cast<U&&>(value));
    hasValue_ = true;
    executor_.reset();
    return {};
  }

  void unhandled_exception() noexcept {
    DCHECK(!hasValue_);
    suspendPoint_ = {};
    exception_ = std::current_exception();
  }

  void return_void() noexcept {
    suspendPoint_ = {};
    DCHECK(!hasValue_);
  }

  template <typename U>
  auto await_transform(U&& value) {
    return folly::coro::co_viaIfAsync(
        executor_.get_alias(), static_cast<U&&>(value));
  }

  auto await_transform(folly::coro::co_current_executor_t) noexcept {
    return AwaitableReady<folly::Executor*>{executor_.get()};
  }

  void setExecutor(folly::Executor::KeepAlive<> executor) noexcept {
    executor_ = std::move(executor);
  }

  void cancel() noexcept {
    if (hasValue_) {
      clearValue();
    }

    if (suspendPoint_) {
      DCHECK(!continuation_);
      continuation_ = std::experimental::noop_continuation();
      suspendPoint_.set_done()();
    }
  }

  std::experimental::continuation_handle resume(
      std::experimental::continuation_handle continuation) noexcept {
    DCHECK(!continuation_);
    DCHECK(suspendPoint_);
    continuation_ = continuation;
    return suspendPoint_.resume();
  }

  bool hasStarted() const noexcept {
    return isComplete() || hasValue();
  }

  bool isComplete() const noexcept {
    return !suspendPoint_;
  }

  void throwIfException() {
    DCHECK(!hasValue_);
    if (exception_) {
      std::rethrow_exception(exception_);
    }
  }

  Reference value() noexcept {
    DCHECK(hasValue_);
    return value_.get();
  }

  std::add_pointer_t<Reference> valuePointer() noexcept {
    DCHECK(hasValue_);
    return std::addressof(value_.get());
  }

  void clearValue() noexcept {
    DCHECK(hasValue_);
    hasValue_ = false;
    value_.destruct();
  }

  bool hasValue() const noexcept {
    return hasValue_;
  }

 private:
  std::experimental::suspend_point_handle<
      std::experimental::with_resume,
      std::experimental::with_set_done>
      suspendPoint_;
  std::experimental::continuation_handle continuation_;
  folly::Executor::KeepAlive<> executor_;
  std::exception_ptr exception_;
  ManualLifetime<Reference> value_;
  bool hasValue_ = false;
};

} // namespace detail

// The AsyncGenerator class represents a sequence of asynchronously produced
// values where the values are produced by a coroutine.
//
// Values are produced by using the 'co_yield' keyword and the coroutine can
// also consume other asynchronous operations using the 'co_await' keyword.
// The end of the sequence is indicated by executing 'co_return;' either
// explicitly or by letting execution run off the end of the coroutine.
//
// Reference Type
// --------------
// The first template parameter controls the 'reference' type.
// i.e. the type returned when you dereference the iterator using operator*().
// This type is typically specified as an actual reference type.
// eg. 'const T&' (non-mutable), 'T&' (mutable)  or 'T&&' (movable) depending
// what access you want your consumers to have to the yielded values.
//
// It's also possible to specify the 'Reference' template parameter as a value
// type. In this case the generator takes a copy of the yielded value (either
// copied or move-constructed) and you get a copy of this value every time
// you dereference the iterator with '*iter'.
// This can be expensive for types that are expensive to copy, but can provide
// a small performance win for types that are cheap to copy (like built-in
// integer types).
//
// Value Type
// ----------
// The second template parameter is optional, but if specified can be used as
// the value-type that should be used to take a copy of the value returned by
// the Reference type.
// By default this type is the same as 'Reference' type stripped of qualifiers
// and references. However, in some cases it can be a different type.
// For example, if the 'Reference' type was a non-reference proxy type.
//
// Example:
//  AsyncGenerator<std::tuple<const K&, V&>, std::tuple<K, V>> getItems() {
//    auto firstMap = co_await getFirstMap();
//    for (auto&& [k, v] : firstMap) {
//      co_yield {k, v};
//    }
//    auto secondMap = co_await getSecondMap();
//    for (auto&& [k, v] : secondMap) {
//      co_yield {k, v};
//    }
//  }
//
// This is mostly useful for generic algorithms that need to take copies of
// elements of the sequence.
//
// Executor Affinity
// -----------------
// An AsyncGenerator coroutine has similar executor-affinity to that of the
// folly::coro::Task coroutine type. Every time a consumer requests a new value
// from the generator using 'co_await ++it' the generator inherits the caller's
// current executor. The coroutine will ensure that it always resumes on the
// associated executor when resuming from `co_await' expression until it hits
// the next 'co_yield' or 'co_return' statement.
// Note that the executor can potentially change at a 'co_yield' statement if
// the next element of the sequence is requested from a consumer coroutine that
// is associated with a different executor.
//
// Example: Writing an async generator.
//
//  folly::coro::AsyncGenerator<Record&&> getRecordsAsync() {
//    auto resultSet = executeQuery(someQuery);
//    for (;;) {
//      auto resultSetPage = co_await resultSet.nextPage();
//      if (resultSetPage.empty()) break;
//      for (auto& row : resultSetPage) {
//        co_yield Record{row.get("name"), row.get("email")};
//      }
//    }
//  }
//
// Example: Consuming items from an async generator
//
//  folly::coro::Task<void> consumer() {
//    auto records = getRecordsAsync();
//    for (auto it = co_await records.begin();
//         it != records.end();
//         co_await ++it) {
//      auto&& record = *it;
//      process(record);
//    }
//  }
//
template <typename Reference, typename Value = remove_cvref_t<Reference>>
class FOLLY_NODISCARD AsyncGenerator {
  static_assert(
      std::is_constructible<Value, Reference>::value,
      "AsyncGenerator 'value_type' must be constructible from a 'reference'.");

 public:
  using promise_type = detail::AsyncGeneratorPromise<Reference, Value>;

 private:
  using handle_t = std::experimental::suspend_point_handle<
      std::experimental::with_destroy,
      std::experimental::with_promise<promise_type>>;

 public:
  using value_type = Value;
  using reference = Reference;
  using pointer = std::add_pointer_t<Reference>;

  struct sentinel {};

  class async_iterator {
    class FOLLY_NODISCARD AdvanceAwaiter {
     public:
      explicit AdvanceAwaiter(async_iterator& iter) noexcept : iter_(iter) {}

      bool await_ready() noexcept {
        return false;
      }

      template <typename SuspendPointHandle>
      std::experimental::continuation_handle await_suspend(
          SuspendPointHandle sp) noexcept {
        auto& promise = *iter_.promise_;
        promise.clearValue();
        return promise.resume(sp.resume());
      }

      async_iterator& await_resume() {
        auto& promise = *iter_.promise_;
        if (promise.isComplete()) {
          promise.throwIfException();
        }
        return iter_;
      }

     private:
      async_iterator& iter_;
    };

    class FOLLY_NODISCARD AdvanceSemiAwaitable {
     public:
      explicit AdvanceSemiAwaitable(async_iterator& iter) noexcept
          : iter_(iter) {}

      friend AdvanceAwaiter co_viaIfAsync(
          folly::Executor::KeepAlive<> executor,
          AdvanceSemiAwaitable awaitable) noexcept {
        awaitable.iter_.promise_->setExecutor(std::move(executor));
        return AdvanceAwaiter{awaitable.iter_};
      }

     private:
      async_iterator& iter_;
    };

    friend class AdvanceAwaiter;
    friend class AdvanceSemiAwaitable;

   public:
    using async_iterator_category = std::input_iterator_tag;
    using value_type = typename AsyncGenerator::value_type;
    using reference = typename AsyncGenerator::reference;
    using pointer = typename AsyncGenerator::pointer;

    async_iterator() noexcept = default;

    explicit async_iterator(promise_type& promise) noexcept
        : promise_(&promise) {}

    async_iterator(const async_iterator& other) noexcept
        : promise_(other.promise_) {}

    async_iterator& operator=(const async_iterator& other) noexcept {
      promise_ = other.promise_;
      return *this;
    }

    AdvanceSemiAwaitable operator++() noexcept {
      return AdvanceSemiAwaitable(*this);
    }

    typename AsyncGenerator::reference operator*() const
        noexcept(std::is_nothrow_copy_constructible<Reference>::value) {
      return promise_->value();
    }

    typename AsyncGenerator::pointer operator->() const noexcept {
      return promise_->valuePointer();
    }

    friend bool operator==(const async_iterator& it, sentinel) noexcept {
      return it.promise_ == nullptr || it.promise_->isComplete();
    }

    friend bool operator!=(const async_iterator& it, sentinel s) noexcept {
      return !(it == s);
    }

    friend bool operator==(sentinel s, const async_iterator& it) noexcept {
      return it == s;
    }

    friend bool operator!=(sentinel s, const async_iterator& it) noexcept {
      return it != s;
    }

   private:
    promise_type* promise_ = nullptr;
  };

 private:
  class FOLLY_NODISCARD BeginAwaiter {
   public:
    BeginAwaiter(promise_type* promise) noexcept : promise_(promise) {}

    bool await_ready() noexcept {
      return promise_ == nullptr;
    }

    template <typename SuspendPointHandle>
    std::experimental::continuation_handle await_suspend(
        SuspendPointHandle sp) noexcept {
      return promise_->resume(sp.resume());
    }

    FOLLY_NODISCARD async_iterator await_resume() {
      if (promise_ != nullptr) {
        if (promise_->isComplete()) {
          promise_->throwIfException();
        }
        return async_iterator{*promise_};
      }
      return async_iterator{};
    }

   private:
    promise_type* promise_;
  };

  class FOLLY_NODISCARD BeginSemiAwaitable {
   public:
    BeginSemiAwaitable() noexcept : promise_(nullptr) {}

    explicit BeginSemiAwaitable(promise_type& promise) noexcept
        : promise_(&promise) {}

    // A BeginSemiAwaitable requires an executor to be injected by calling
    // the folly::coro::co_viaIfAsync() function. This is done implicitly
    // by coroutine-types such as Task<T> and AsyncGenerator<T> which call
    // co_viaIfAsync() from their promise_type::await_transform() method
    // to inject the awaiting coroutine's current executor.
    friend BeginAwaiter co_viaIfAsync(
        folly::Executor::KeepAlive<> executor,
        BeginSemiAwaitable&& awaitable) noexcept {
      if (awaitable.promise_ != nullptr) {
        awaitable.promise_->setExecutor(std::move(executor));
      }
      return BeginAwaiter{awaitable.promise_};
    }

   private:
    promise_type* promise_;
  };

 public:
  AsyncGenerator() noexcept : coro_() {}

  AsyncGenerator(AsyncGenerator&& other) noexcept
      : coro_(std::exchange(other.coro_, {})) {}

  ~AsyncGenerator() {
    if (coro_) {
      coro_.promise().cancel();
      coro_.destroy();
    }
  }

  AsyncGenerator& operator=(AsyncGenerator other) noexcept {
    swap(other);
    return *this;
  }

  void swap(AsyncGenerator& other) noexcept {
    std::swap(coro_, other.coro_);
  }

  // begin() returns a SemiAwaitable type that must either be awaited within
  // the context of a coroutine that has an associated folly::Executor (eg.
  // a folly::coro::Task<T> or a folly::coro::AsyncGenerator<T>) or otherwise
  // must have an executor explicitly injected by calling
  // folly::co_viaIfAsync(executor, gen.begin()).
  //
  // The result of `co_await this->begin()` is an 'async_iterator' that can
  // be used to access the current elements of the sequence and advance to
  // the next element.
  //
  // Note that the AsyncGenerator is an input-range and the elements can only
  // be consumed once. It is undefined behaviour to call the .begin() method
  // multiple times for the same generator object.
  FOLLY_NODISCARD BeginSemiAwaitable begin() noexcept {
    DCHECK(!hasStarted());
    if (coro_) {
      return BeginSemiAwaitable{coro_.promise()};
    }
    return BeginSemiAwaitable{};
  }

  FOLLY_NODISCARD sentinel end() noexcept {
    return {};
  }

 private:
  friend class detail::AsyncGeneratorPromise<Reference, Value>;

  explicit AsyncGenerator(handle_t coro) noexcept : coro_(coro) {}

  bool hasStarted() const noexcept {
    return coro_ && coro_.promise().hasStarted();
  }

  handle_t coro_;
};

namespace detail {

template <typename Reference, typename Value>
template <typename SuspendPointHandle>
AsyncGenerator<Reference, Value>
AsyncGeneratorPromise<Reference, Value>::get_return_object(
    SuspendPointHandle sp) noexcept {
  suspendPoint_ = sp;
  return AsyncGenerator<Reference, Value>{sp};
}

template <typename T>
inline constexpr bool is_async_generator_v = false;

template <typename Reference, typename Value>
inline constexpr bool is_async_generator_v<AsyncGenerator<Reference, Value>> =
    true;

} // namespace detail

// Helper for immediately invoking a lambda with captures that returns an
// AsyncGenerator to keep the lambda alive until the generator completes.
//
// Example:
//   auto gen = co_invoke([min, max]() -> AsyncGenerator<T> {
//     for (int i = min; i <= max; ++i) {
//       co_yield co_await doSomething(i);
//     }
//   });
template <typename Func, typename... Args>
auto co_invoke(Func func, Args... args) -> std::enable_if_t<
    detail::is_async_generator_v<invoke_result_t<Func, Args...>>,
    invoke_result_t<Func, Args...>> {
  auto asyncRange =
      folly::invoke(static_cast<Func&&>(func), static_cast<Args&&>(args)...);
  const auto itEnd = asyncRange.end();
  auto it = co_await asyncRange.begin();
  while (it != itEnd) {
    co_yield* it;
    co_await++ it;
  }
}

} // namespace coro
} // namespace folly
