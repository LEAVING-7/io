#pragma once
#include "Reactor.hpp"
#include "Runtime.hpp"
#include "Slab.hpp"
#include "ThreadPool.hpp"
#include "concepts.hpp"
#include <future>
namespace async {

template <typename ExecutorType>
class BlockingExecutor {
public:
  template <std::invocable Fn, typename... Args>
  auto blockSpawn(Fn&& fn, Args&&... args)
  {
    std::call_once(mBlockingPoolFlag, [this]() { mBlockingPool = std::make_unique<BlockingThreadPool>(500); });
    using Result = std::invoke_result_t<Fn, Args...>;
    auto function = std::bind_front(std::forward<Fn>(fn), std::forward<Args>(args)...);
    if constexpr (std::is_void_v<Result>) {
      struct Awaiter {
        BlockingExecutor<ExecutorType>& e;
        decltype(function) fn;
        auto await_ready() -> bool { return false; }
        auto await_suspend(std::coroutine_handle<> in) -> void
        {
          auto task = [](std::invocable auto&& fn) -> ContinueTask {
            fn();
            co_return;
          }(std::move(fn));
          e.execute(task.setContinue(in).handle);
        }
        auto await_resume() -> Result { return; }
      };
      return Awaiter {*this, std::move(function)};
    } else {
      struct Awaiter {
        BlockingExecutor<ExecutorType>& e;
        decltype(function) fn;
        std::optional<Result> result;
        auto await_ready() -> bool { return false; }
        auto await_suspend(std::coroutine_handle<> in) -> void
        {
          auto task = [this](std::invocable auto&& fn) -> ContinueTask {
            this->result = std::move(fn());
            co_return;
          }(std::move(fn));
          e.execute(task.setContinue(in).handle);
        }
        auto await_resume() -> Result
        {
          assert(result.has_value());
          return std::move(result.value());
        }
      };
      return Awaiter {*this, std::move(function)};
    }
  }
  auto execute(std::coroutine_handle<> handle) -> void { mBlockingPool->execute(handle); }

protected:
  std::once_flag mBlockingPoolFlag;
  std::unique_ptr<BlockingThreadPool> mBlockingPool {nullptr};
};

class MultiThreadExecutor {
public:
  MultiThreadExecutor(size_t n) : mPool(n) {}

  auto spawnDetach(Task<> in) -> void
  {
    mSpawnCount.fetch_add(1, std::memory_order_acquire);
    auto afterDoneFn = [this]() {
      mSpawnCount.fetch_sub(1, std::memory_order_release);
      Runtime<MultiThreadExecutor>::GetReactor().notify();
    };
    auto handle = [](Task<> task)
        -> DetachTask<void> { co_return co_await task; }(std::move(in)).afterDestroy(afterDoneFn).handle;
    mPool.execute(handle);
  }
  template <typename T>
  [[nodiscard]] auto block(Task<T> in) -> T
  {
    auto promise = std::make_shared<std::promise<T>>();
    auto future = promise->get_future();

    if constexpr (std::is_void_v<T>) { // return void
      auto afterDoneFn = [this, promise]() {
        promise->set_value();
        Runtime<MultiThreadExecutor>::GetReactor().notify();
      };
      auto handle = [](Task<T> task)
          -> DetachTask<T> { co_return co_await task; }(std::move(in)).afterDestroy(std::move(afterDoneFn)).handle;
      mPool.execute(handle);
    } else {
      auto afterDoneFn = [promise](auto&& value) {
        promise->set_value(std::forward<T>(value));
        Runtime<MultiThreadExecutor>::GetReactor().notify();
      };
      auto newTask = [](Task<T> task) -> DetachTask<T> {
        auto value = co_await task;
        co_return value;
      }(std::move(in));
      mPool.execute(newTask.afterDestroy(std::move(afterDoneFn)).handle);
    }

    using namespace std::chrono_literals;
    while (true) {
      if (future.wait_for(0s) == std::future_status::ready) {
        if (mSpawnCount.load(std::memory_order_acquire) == 0) {
          break;
        }
      }
      Runtime<MultiThreadExecutor>::GetReactor().lock().react(std::nullopt, mPool);
    }
    return future.get();
  }

  template <typename... Args>
  auto blockSpawn(Args&&... args)
  {
    return mBlockingExecutor.blockSpawn(std::forward<Args>(args)...);
  }
  auto execute(std::coroutine_handle<> handle) -> void { mPool.execute(handle); }

private:
  BlockingExecutor<MultiThreadExecutor> mBlockingExecutor;

  std::atomic_size_t mSpawnCount;
  StealingThreadPool mPool;
};

class InlineExecutor final {
public:
  InlineExecutor() : mQueue(), mSpawnCount(0) {}

  auto spawnDetach(Task<> task) -> void
  {
    mSpawnCount += 1;
    auto afterDestroyFn = [this]() {
      mSpawnCount -= 1;
      Runtime<InlineExecutor>::GetReactor().notify();
    };
    auto handle = [](Task<> task) -> DetachTask<void> { co_return co_await task; }(std::move(task))
                                         .afterDestroy(std::move(afterDestroyFn))
                                         .handle;
    mQueue.push(handle);
  }

  template <typename T>
    requires(not std::is_void_v<T>)
  auto block(Task<T> task) -> T
  {
    auto returnValue = std::optional<T>(std::nullopt);
    auto afterDestroyFn = [&returnValue](T&& value) {
      returnValue.emplace(std::move(value));
      Runtime<InlineExecutor>::GetReactor().notify();
    };
    auto handle = [this](Task<T> task)
        -> DetachTask<T> { co_return co_await task; }(std::move(task)).afterDestroy(std::move(afterDestroyFn)).handle;
    handle.resume();

    while (true) {
      while (!mQueue.empty()) {
        auto handle = mQueue.front();
        mQueue.pop();
        handle.resume();
      }
      if (mSpawnCount == 0 && returnValue.has_value() && mQueue.empty()) {
        break;
      }
      Runtime<InlineExecutor>::GetReactor().lock().react(std::nullopt, *this);
    }

    return std::move(returnValue.value());
  }
  auto block(Task<> task) -> void
  {
    auto hasValue = false;
    auto afterDestroyFn = [&hasValue]() {
      hasValue = true;
      Runtime<InlineExecutor>::GetReactor().notify();
    };
    auto handle = [](Task<> task) -> DetachTask<void> { co_return co_await task; }(std::move(task))
                                         .afterDestroy(std::move(afterDestroyFn))
                                         .handle;
    handle.resume();
    while (true) {
      while (!mQueue.empty()) {
        auto handle = mQueue.front();
        mQueue.pop();
        handle.resume();
      }
      if (mSpawnCount == 0 && hasValue && mQueue.empty()) {
        break;
      }
      Runtime<InlineExecutor>::GetReactor().lock().react(std::nullopt, *this);
    }
  }

  template <typename... Args>
  [[nodiscard]] auto blockSpawn(Args&&... args)
  {
    return mBlockingExecutor.blockSpawn(std::forward<Args>(args)...);
  }
  auto execute(std::coroutine_handle<> handle) -> void { handle.resume(); }

private:
  BlockingExecutor<InlineExecutor> mBlockingExecutor;

  std::queue<std::coroutine_handle<>> mQueue;
  size_t mSpawnCount;
};
} // namespace async
