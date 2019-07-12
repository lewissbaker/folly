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

#include <cassert>
#include <exception>
#include <experimental/coroutine>
#include <type_traits>
#include <utility>

namespace folly {
namespace coro {

template <typename T>
class Generator {
 public:
  class promise_type final {
   public:
    promise_type() noexcept
        : value_(nullptr),
          exception_(nullptr),
          root_(this),
          parentOrLeaf_(this) {}

    promise_type(const promise_type&) = delete;
    promise_type(promise_type&&) = delete;

    template <typename SuspendPointHandle>
    auto get_return_object(SuspendPointHandle sp) noexcept {
      suspendPoint_ = sp;
      return Generator<T>{sp};
    }

    std::experimental::continuation_handle done() noexcept {
      if (this == root_) {
        suspendPoint_ = {};
        return std::experimental::noop_continuation();
      } else {
        // This was a nested generator.
        // We're about to resume the parent generator so update the root
        // generator's pointer to the new leaf (ie. our parent)
        root_->parentOrLeaf_ = parentOrLeaf_;

        if (suspendPoint_) {
          // We reached done() without passing through either
          // return_void() or unhandled_exception().
          // This means we have been cancelled and so should
          // propagate this cancellation signal to the parent.
          suspendPoint_ = {};
          return parentOrLeaf_->suspendPoint_.set_done();
        } else {
          return parentOrLeaf_->suspendPoint_.resume();
        }
      }
    }

    void unhandled_exception() noexcept {
      suspendPoint_ = {};
      exception_ = std::current_exception();
    }

    void return_void() noexcept {
      suspendPoint_ = {};
    }

    struct YieldValueAwaiter {
      bool await_ready() {
        return false;
      }
      template <typename SuspendPointHandle>
      void await_suspend(SuspendPointHandle sp) {
        sp.promise().suspendPoint_ = sp;
      }
      void await_resume() {}
    };

    YieldValueAwaiter yield_value(T&& value) noexcept {
      value_ = std::addressof(value);
      return {};
    }

    class YieldSequenceAwaiter {
     public:
      explicit YieldSequenceAwaiter(promise_type* childPromise) noexcept
          : childPromise_(childPromise) {}

      bool await_ready() noexcept {
        return childPromise_ == nullptr;
      }

      template <typename SuspendPointHandle>
      auto await_suspend(SuspendPointHandle sp) noexcept {
        sp.promise().suspendPoint_ = sp;
        return childPromise_->suspendPoint_.resume();
      }

      void await_resume() {
        if (childPromise_ != nullptr) {
          childPromise_->throwIfException();
        }
      }

     private:
      promise_type* childPromise_;
    };

    YieldSequenceAwaiter yield_value(Generator&& generator) noexcept {
      if (generator.promise_ != nullptr) {
        root_->parentOrLeaf_ = generator.promise_;
        generator.promise_->root_ = root_;
        generator.promise_->parentOrLeaf_ = this;
      }
      return YieldSequenceAwaiter{generator.promise_};
    }

    // Don't allow any use of 'co_await' inside the Generator
    // coroutine.
    template <typename U>
    std::experimental::suspend_never await_transform(U&& value) = delete;

    void throwIfException() {
      if (exception_) {
        std::rethrow_exception(std::move(exception_));
      }
    }

    // Cancel this generator.
    // Only valid to call this on the root generator.
    void cancel() noexcept {
      assert(this == root_);
      if (!isComplete()) {
        // Start cancelling from the leaf.
        // This should proceed to unwind from the leaf back up to the
        // parent.
        parentOrLeaf_->suspendPoint_.set_done()();
        assert(isComplete());
      }
    }

    void pull() {
      assert(this == root_);
      assert(!isComplete());
      parentOrLeaf_->suspendPoint_.resume()();
      if (isComplete()) {
        throwIfException();
      }
    }

    bool isComplete() noexcept {
      return !suspendPoint_;
    }

    T& value() noexcept {
      assert(this == root_);
      assert(!isComplete());
      return *(parentOrLeaf_->value_);
    }

   private:
    using suspend_point_t = std::experimental::suspend_point_handle<
        std::experimental::with_resume,
        std::experimental::with_set_done>;

    suspend_point_t suspendPoint_;

    std::add_pointer_t<T> value_;
    std::exception_ptr exception_;

    promise_type* root_;

    // If this is the promise of the root generator then this field
    // is a pointer to the leaf promise.
    // For non-root generators this is a pointer to the parent promise.
    promise_type* parentOrLeaf_;
  };

  using handle_t = std::experimental::suspend_point_handle<
      std::experimental::with_destroy,
      std::experimental::with_promise<promise_type>>;

  Generator() noexcept = default;

  explicit Generator(handle_t coro) noexcept : coro_(coro) {}

  Generator(Generator&& other) noexcept
      : coro_(std::exchange(other.coro_, {})) {}

  Generator(const Generator& other) = delete;
  Generator& operator=(const Generator& other) = delete;

  ~Generator() {
    if (coro_) {
      coro_.promise().cancel();
      coro_.destroy();
    }
  }

  Generator& operator=(Generator other) noexcept {
    swap(other);
    return *this;
  }

  class sentinel {};

  class iterator {
   public:
    using iterator_category = std::input_iterator_tag;
    // What type should we use for counting elements of a potentially infinite
    // sequence?
    using difference_type = std::ptrdiff_t;
    using value_type = std::remove_reference_t<T>;
    using reference = std::conditional_t<std::is_reference_v<T>, T, T&>;
    using pointer = std::add_pointer_t<T>;

    iterator() noexcept : promise_(nullptr) {}

    explicit iterator(promise_type& promise) noexcept : promise_(&promise) {}

    friend bool operator==(const iterator& it, sentinel) noexcept {
      return it.promise_ == nullptr || it.promise_->isComplete();
    }

    friend bool operator!=(const iterator& it, sentinel s) noexcept {
      return !(it == s);
    }

    friend bool operator==(sentinel s, const iterator& it) noexcept {
      return (it == s);
    }

    friend bool operator!=(sentinel s, const iterator& it) noexcept {
      return !(it == s);
    }

    iterator& operator++() {
      assert(promise_ != nullptr);
      assert(!promise_->isComplete());
      promise_->pull();
      return *this;
    }

    void operator++(int) {
      (void)operator++();
    }

    reference operator*() const noexcept {
      assert(promise_ != nullptr);
      return static_cast<reference>(promise_->value());
    }

    pointer operator->() const noexcept {
      return std::addressof(operator*());
    }

   private:
    promise_type* promise_;
  };

  iterator begin() {
    if (coro_) {
      auto& promise = coro_.promise();
      assert(!promise.isComplete());
      promise.pull();
      return iterator{promise};
    }
    return iterator{};
  }

  sentinel end() noexcept {
    return {};
  }

  void swap(Generator& other) noexcept {
    std::swap(coro_, other.coro_);
  }

 private:
  friend class promise_type;

  handle_t coro_;
};

template <typename T>
void swap(Generator<T>& a, Generator<T>& b) noexcept {
  a.swap(b);
}
} // namespace coro
} // namespace folly
