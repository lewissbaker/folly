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

#include <folly/Benchmark.h>
#include <thread>

struct ExpensiveCopy {
  ExpensiveCopy() {}

  ExpensiveCopy(const ExpensiveCopy&) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
};

#if FOLLY_HAS_COROUTINES
#include <experimental/coroutine>
#include <future>

class Wait {
 public:
  class promise_type {
   public:
    template <typename SuspendPointHandle>
    Wait get_return_object(SuspendPointHandle sp) {
      sp_ = sp;
      // Get the future first as the call to resume() may complete
      // concurrently on another thread and destroy the promise before
      // resume returns.
      auto f = promise_.get_future();
      sp.resume()();
      return Wait(std::move(f));
    }

    auto done() {
      sp_.destroy();
      return std::experimental::noop_continuation();
    }

    void return_void() {
      promise_.set_value();
    }

    void unhandled_exception() {
      promise_.set_exception(std::current_exception());
    }

   private:
    std::experimental::suspend_point_handle<std::experimental::with_destroy>
        sp_;
    std::promise<void> promise_;
  };

  explicit Wait(std::future<void> future) : future_(std::move(future)) {}

  Wait(Wait&&) = default;

  void detach() {
    future_ = {};
  }

  ~Wait() {
    if (future_.valid()) {
      future_.get();
    }
  }

 private:
  std::future<void> future_;
};

template <typename T>
class InlineTask {
 public:
  class promise_type {
   public:
    template <typename SuspendPointHandle>
    InlineTask get_return_object(SuspendPointHandle sp) {
      return InlineTask(sp);
    }

    template <typename U>
    void return_value(U&& value) {
      *valuePtr_ = std::forward<U>(value);
    }

    void unhandled_exception() {
      std::terminate();
    }

    auto done() {
      return continuation_;
    }

   private:
    friend class InlineTask;

    T* valuePtr_;
    std::experimental::continuation_handle continuation_;
  };

  using handle_t = std::experimental::suspend_point_handle<
      std::experimental::with_resume,
      std::experimental::with_destroy,
      std::experimental::with_promise<promise_type>>;

  InlineTask(const InlineTask&) = delete;
  InlineTask(InlineTask&& other) : coro_(std::exchange(other.coro_, {})) {}

  ~InlineTask() {
    DCHECK(!coro_);
  }

  bool await_ready() const {
    return false;
  }

  template <typename SuspendPointHandle>
  std::experimental::continuation_handle await_suspend(SuspendPointHandle sp) {
    auto& promise = coro_.promise();
    promise.valuePtr_ = &value_;
    promise.continuation_ = sp.resume();
    return coro_.resume();
  }

  T await_resume() {
    std::exchange(coro_, {}).destroy();
    T value = std::move(value_);
    return value;
  }

 private:
  friend class promise_type;

  explicit InlineTask(handle_t coro) : coro_(coro) {}

  T value_;
  handle_t coro_;
};

InlineTask<ExpensiveCopy> co_nestedCalls(size_t times) {
  ExpensiveCopy ret;
  if (times > 0) {
    ret = co_await co_nestedCalls(times - 1);
  }
  co_return ret;
}

void coroNRVO(size_t times, size_t iters) {
  for (size_t iter = 0; iter < iters; ++iter) {
    [](size_t times) -> Wait {
      (void)co_await co_nestedCalls(times);
      co_return;
    }(times);
  }
}

BENCHMARK(coroNRVOOneAwait, iters) {
  coroNRVO(1, iters);
}

BENCHMARK(coroNRVOTenAwaits, iters) {
  coroNRVO(10, iters);
}
#endif

ExpensiveCopy nestedCalls(size_t times) {
  ExpensiveCopy ret;
  if (times > 0) {
    ret = nestedCalls(times - 1);
  }

  return ret;
}

void NRVO(size_t times, size_t iters) {
  for (size_t iter = 0; iter < iters; ++iter) {
    nestedCalls(times);
  }
}

BENCHMARK(NRVOOneAwait, iters) {
  NRVO(1, iters);
}

BENCHMARK(NRVOTenAwaits, iters) {
  NRVO(10, iters);
}

int main(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  folly::runBenchmarks();
  return 0;
}
