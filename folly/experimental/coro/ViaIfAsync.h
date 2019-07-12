/*
 * Copyright 2017-present Facebook, Inc.
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

#include <experimental/coroutine>
#include <memory>

#include <folly/Executor.h>
#include <folly/Traits.h>
#include <folly/experimental/coro/Traits.h>
#include <folly/experimental/coro/detail/InlineTask.h>
#include <folly/io/async/Request.h>
#include <folly/lang/CustomizationPoint.h>

#include <glog/logging.h>

namespace folly {

class InlineExecutor;

namespace coro {

namespace detail {

class ViaCoroutine {
 public:
  class promise_type {
   public:
    template <typename SuspendPointHandle>
    ViaCoroutine get_return_object(SuspendPointHandle sp) noexcept {
      return ViaCoroutine{sp};
    }

    auto done() {
      return continuation_;
    }

    [[noreturn]] void unhandled_exception() noexcept {
      std::terminate();
    }

    void return_void() noexcept {}

   private:
    friend class ViaCoroutine;

    std::experimental::continuation_handle continuation_;
  };

  using handle_t = std::experimental::suspend_point_handle<
      std::experimental::with_destroy,
      std::experimental::with_resume,
      std::experimental::with_promise<promise_type>>;

  ViaCoroutine() noexcept {}

  explicit ViaCoroutine(handle_t coro) noexcept : coro_(coro) {}

  ViaCoroutine(ViaCoroutine&& other) noexcept
      : coro_(std::exchange(other.coro_, {})) {}

  ~ViaCoroutine() {
    if (coro_) {
      coro_.destroy();
    }
  }

  ViaCoroutine& operator=(ViaCoroutine other) noexcept {
    std::swap(coro_, other.coro_);
    return *this;
  }

  auto start(std::experimental::continuation_handle continuation) noexcept {
    coro_.promise().continuation_ = continuation;
    return coro_.resume();
  }

 private:
  handle_t coro_;
};

} // namespace detail

template <typename Awaiter>
class ViaIfAsyncAwaiter {
 public:
  static_assert(
      folly::coro::is_awaiter_v<Awaiter>,
      "Awaiter type does not implement the Awaiter interface.");

  template <typename Awaitable>
  explicit ViaIfAsyncAwaiter(
      folly::Executor::KeepAlive<> executor,
      Awaitable&& awaitable)
      : awaiter_(folly::coro::get_awaiter(static_cast<Awaitable&&>(awaitable))),
        executor_(std::move(executor)) {}

  bool await_ready() noexcept(
      noexcept(std::declval<Awaiter&>().await_ready())) {
    return awaiter_.await_ready();
  }

  // NOTE: We are using a heuristic here to determine when is the correct
  // time to capture the RequestContext. We want to capture the context just
  // before the coroutine suspends and execution is returned to the executor.
  //
  // In cases where we are awaiting another coroutine and symmetrically
  // transferring execution to another coroutine we are not yet returning
  // execution to the executor so we want to defer capturing the context until
  // the ViaCoroutine is resumed and suspends in final_suspend() before
  // scheduling the resumption on the executor.
  //
  // In cases where the awaitable may suspend without transferring execution
  // to another coroutine and will therefore return back to the executor we
  // want to capture the execution context before calling into the wrapped
  // awaitable's await_suspend() method (since it's await_suspend() method
  // might schedule resumption on another thread and could resume and destroy
  // the ViaCoroutine before the await_suspend() method returns).
  //
  // The heuristic is that if await_suspend() returns a coroutine_handle
  // then we assume it's the first case. Otherwise if await_suspend() returns
  // void/bool then we assume it's the second case.
  //
  // This heuristic isn't perfect since a coroutine_handle-returning
  // await_suspend() method could return noop_coroutine() in which case we
  // could fail to capture the current context. Awaitable types that do this
  // would need to provide a custom implementation of co_viaIfAsync() that
  // correctly captures the RequestContext to get correct behaviour in this
  // case.

 private:
  class InnerAwaiter {
   public:
    explicit InnerAwaiter(Awaiter& awaiter) noexcept : awaiter_(awaiter) {}

    bool await_ready() noexcept {
      return false;
    }

    template <typename SuspendPointHandle>
    auto await_suspend(SuspendPointHandle sp) {
      using await_suspend_result_t = decltype(awaiter_.await_suspend(sp));
      if constexpr (
          std::is_void_v<await_suspend_result_t> ||
          std::is_same_v<bool, await_suspend_result_t>) {
        context_ = RequestContext::saveContext();
      }

      return awaiter_.await_suspend(sp);
    }

    void await_resume() noexcept {
      if (!context_) {
        context_ = RequestContext::saveContext();
      }
    }

    std::shared_ptr<RequestContext> stealContext() {
      return std::move(context_);
    }

   private:
    Awaiter& awaiter_;
    std::shared_ptr<RequestContext> context_;
  };

  class ScheduleAwaiter {
    folly::Executor::KeepAlive<> executor_;
    std::shared_ptr<RequestContext> context_;

   public:
    explicit ScheduleAwaiter(
        folly::Executor::KeepAlive<> executor,
        std::shared_ptr<RequestContext> context) noexcept
        : executor_(std::move(executor)), context_(std::move(context)) {}

    bool await_ready() noexcept {
      return false;
    }

    FOLLY_CORO_AWAIT_SUSPEND_NONTRIVIAL_ATTRIBUTES void await_suspend(
        std::experimental::suspend_point_handle<std::experimental::with_resume>
            sp) {
      executor_->add([sp, ctx = std::move(context_)]() mutable {
        RequestContextScopeGuard contextScope{std::move(ctx)};
        sp.resume()();
      });
    }

    void await_resume() noexcept {}
  };

 public:
  std::experimental::continuation_handle await_suspend(
      std::experimental::suspend_point_handle<std::experimental::with_resume>
          sp) {
    viaCoroutine_ =
        [](Awaiter& awaiter,
           folly::Executor::KeepAlive<> executor) -> detail::ViaCoroutine {
      InnerAwaiter inner{awaiter};
      co_await inner;
      co_await ScheduleAwaiter{std::move(executor), inner.stealContext()};
    }(awaiter_, std::move(executor_));
    return viaCoroutine_.start(sp.resume());
  }

  decltype(auto) await_resume() {
    return awaiter_.await_resume();
  }

 private:
  Awaiter awaiter_;
  folly::Executor::KeepAlive<> executor_;
  detail::ViaCoroutine viaCoroutine_;
};

template <typename Awaitable>
class ViaIfAsyncAwaitable {
 public:
  explicit ViaIfAsyncAwaitable(
      folly::Executor::KeepAlive<> executor,
      Awaitable&&
          awaitable) noexcept(std::is_nothrow_move_constructible<Awaitable>::
                                  value)
      : executor_(std::move(executor)),
        awaitable_(static_cast<Awaitable&&>(awaitable)) {}

  template <typename Awaitable2>
  friend auto operator co_await(ViaIfAsyncAwaitable<Awaitable2>&& awaitable)
      -> ViaIfAsyncAwaiter<folly::coro::awaiter_type_t<Awaitable2>>;

  template <typename Awaitable2>
  friend auto operator co_await(ViaIfAsyncAwaitable<Awaitable2>& awaitable)
      -> ViaIfAsyncAwaiter<folly::coro::awaiter_type_t<Awaitable2&>>;

  template <typename Awaitable2>
  friend auto operator co_await(
      const ViaIfAsyncAwaitable<Awaitable2>&& awaitable)
      -> ViaIfAsyncAwaiter<folly::coro::awaiter_type_t<const Awaitable2&&>>;

  template <typename Awaitable2>
  friend auto operator co_await(
      const ViaIfAsyncAwaitable<Awaitable2>& awaitable)
      -> ViaIfAsyncAwaiter<folly::coro::awaiter_type_t<const Awaitable2&>>;

 private:
  folly::Executor::KeepAlive<> executor_;
  Awaitable awaitable_;
};

template <typename Awaitable>
auto operator co_await(ViaIfAsyncAwaitable<Awaitable>&& awaitable)
    -> ViaIfAsyncAwaiter<folly::coro::awaiter_type_t<Awaitable>> {
  return ViaIfAsyncAwaiter<folly::coro::awaiter_type_t<Awaitable>>{
      awaitable.executor_, static_cast<Awaitable&&>(awaitable.awaitable_)};
}

template <typename Awaitable>
auto operator co_await(ViaIfAsyncAwaitable<Awaitable>& awaitable)
    -> ViaIfAsyncAwaiter<folly::coro::awaiter_type_t<Awaitable&>> {
  return ViaIfAsyncAwaiter<folly::coro::awaiter_type_t<Awaitable&>>{
      awaitable.executor_, awaitable.awaitable_};
}

template <typename Awaitable>
auto operator co_await(const ViaIfAsyncAwaitable<Awaitable>&& awaitable)
    -> ViaIfAsyncAwaiter<folly::coro::awaiter_type_t<const Awaitable&&>> {
  return ViaIfAsyncAwaiter<folly::coro::awaiter_type_t<const Awaitable&&>>{
      awaitable.executor_,
      static_cast<const Awaitable&&>(awaitable.awaitable_)};
}

template <typename Awaitable>
auto operator co_await(const ViaIfAsyncAwaitable<Awaitable>& awaitable)
    -> ViaIfAsyncAwaiter<folly::coro::awaiter_type_t<const Awaitable&>> {
  return ViaIfAsyncAwaiter<folly::coro::awaiter_type_t<const Awaitable&>>{
      awaitable.executor_, awaitable.awaitable_};
}

namespace detail {

template <typename SemiAwaitable, typename = void>
struct HasViaIfAsyncMethod : std::false_type {};

template <typename SemiAwaitable>
struct HasViaIfAsyncMethod<
    SemiAwaitable,
    void_t<decltype(std::declval<SemiAwaitable>().viaIfAsync(
        std::declval<folly::Executor::KeepAlive<>>()))>> : std::true_type {};

namespace adl {

template <typename SemiAwaitable>
auto co_viaIfAsync(
    folly::Executor::KeepAlive<> executor,
    SemiAwaitable&&
        awaitable) noexcept(noexcept(static_cast<SemiAwaitable&&>(awaitable)
                                         .viaIfAsync(std::move(executor))))
    -> decltype(static_cast<SemiAwaitable&&>(awaitable).viaIfAsync(
        std::move(executor))) {
  return static_cast<SemiAwaitable&&>(awaitable).viaIfAsync(
      std::move(executor));
}

template <
    typename Awaitable,
    std::enable_if_t<
        is_awaitable_v<Awaitable> && !HasViaIfAsyncMethod<Awaitable>::value,
        int> = 0>
auto co_viaIfAsync(folly::Executor::KeepAlive<> executor, Awaitable&& awaitable)
    -> ViaIfAsyncAwaitable<Awaitable> {
  static_assert(
      folly::coro::is_awaitable_v<Awaitable>,
      "co_viaIfAsync() argument 2 is not awaitable.");
  return ViaIfAsyncAwaitable<Awaitable>{std::move(executor),
                                        static_cast<Awaitable&&>(awaitable)};
}

struct ViaIfAsyncFunction {
  template <typename Awaitable>
  auto operator()(folly::Executor::KeepAlive<> executor, Awaitable&& awaitable)
      const noexcept(noexcept(co_viaIfAsync(
          std::move(executor),
          static_cast<Awaitable&&>(awaitable))))
          -> decltype(co_viaIfAsync(
              std::move(executor),
              static_cast<Awaitable&&>(awaitable))) {
    return co_viaIfAsync(
        std::move(executor), static_cast<Awaitable&&>(awaitable));
  }
};

} // namespace adl
} // namespace detail

/// Returns a new awaitable that will resume execution of the awaiting
/// coroutine on a specified executor in the case that the operation does not
/// complete synchronously.
///
/// If the operation completes synchronously then the awaiting coroutine
/// will continue execution on the current thread without transitioning
/// execution to the specified executor.
FOLLY_DEFINE_CPO(detail::adl::ViaIfAsyncFunction, co_viaIfAsync)

template <typename T, typename = void>
struct is_semi_awaitable : std::false_type {};

template <typename T>
struct is_semi_awaitable<
    T,
    void_t<decltype(folly::coro::co_viaIfAsync(
        std::declval<folly::Executor::KeepAlive<>>(),
        std::declval<T>()))>> : std::true_type {};

template <typename T>
constexpr bool is_semi_awaitable_v = is_semi_awaitable<T>::value;

template <typename T>
using semi_await_result_t = await_result_t<decltype(folly::coro::co_viaIfAsync(
    std::declval<folly::Executor::KeepAlive<>>(),
    std::declval<T>()))>;

namespace detail {

template <typename Awaiter>
class TryAwaiter {
 public:
  TryAwaiter(Awaiter&& awaiter) : awaiter_(std::move(awaiter)) {}

  bool await_ready() {
    return awaiter_.await_ready();
  }

  template <typename SuspendPointHandle>
  auto await_suspend(SuspendPointHandle sp) {
    return awaiter_.await_suspend(sp);
  }

  auto await_resume() {
    return awaiter_.await_resume_try();
  }

 private:
  Awaiter awaiter_;
};

template <typename Awaiter>
auto makeTryAwaiter(Awaiter&& awaiter) {
  return TryAwaiter<std::decay_t<Awaiter>>(std::move(awaiter));
}

template <typename SemiAwaitable>
class TrySemiAwaitable {
 public:
  explicit TrySemiAwaitable(SemiAwaitable&& semiAwaitable)
      : semiAwaitable_(std::move(semiAwaitable)) {}

  friend auto co_viaIfAsync(
      Executor::KeepAlive<> executor,
      TrySemiAwaitable&& self) noexcept {
    return makeTryAwaiter(get_awaiter(
        co_viaIfAsync(std::move(executor), std::move(self.semiAwaitable_))));
  }

 private:
  SemiAwaitable semiAwaitable_;
};
} // namespace detail

template <
    typename SemiAwaitable,
    typename = std::enable_if_t<is_semi_awaitable_v<SemiAwaitable>>>
auto co_awaitTry(SemiAwaitable&& semiAwaitable) {
  return detail::TrySemiAwaitable<SemiAwaitable>(std::move(semiAwaitable));
}

} // namespace coro
} // namespace folly
