#pragma once
#include "Async/Task.hpp"
#include "Async/Reactor.hpp"
#include "Async/Slab.hpp"
#include <cassert>
#include <concepts>
#include <deque>
#include <future>
#include <queue>
#include <random>
#include <shared_mutex>

namespace async {
class BlockingThreadPool {
public:
  BlockingThreadPool(size_t threadLimit) : mIdleCount(0), mThreadLimits(threadLimit), mThreadCount(0) {}
  ~BlockingThreadPool()
  {
    auto lk = std::unique_lock(mQueueMt);
    mQueueCv.wait(lk, [this]() { return mQueue.empty(); });
  }

  auto execute(std::coroutine_handle<> handle) -> void
  {
    auto lk = std::unique_lock(mQueueMt);
    mQueue.push_back(std::move(handle));
    mQueueCv.notify_one();
    growPool();
  }

private:
  auto loop() -> void
  {
    using namespace std::chrono_literals;
    auto lk = std::unique_lock(mQueueMt);
    while (true) {
      mIdleCount -= 1;
      while (!mQueue.empty()) {
        growPool();
        auto task = std::move(mQueue.front());
        mQueue.pop_front();
        lk.unlock();
        task.resume();
        lk.lock();
      }
      mIdleCount += 1;
      auto r = mQueueCv.wait_for(lk, 500ms);
      if (r == std::cv_status::timeout && mQueue.empty()) {
        mIdleCount -= 1;
        mThreadCount -= 1;
        break;
      }
    }
  }

  auto growPool() -> void
  {
    assert(!mQueueMt.try_lock());
    while (mQueue.size() > mIdleCount * 5 && mThreadCount < mThreadLimits) {
      mThreadCount += 1;
      mIdleCount += 1;
      mQueueCv.notify_all();
      std::thread([this]() { loop(); }).detach();
    }
  }

  std::mutex mQueueMt;
  std::condition_variable mQueueCv;
  std::deque<std::coroutine_handle<>> mQueue;

  size_t mIdleCount;
  size_t mThreadCount;
  size_t mThreadLimits;
};

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

class ThreadPool {
public:
  ThreadPool(size_t threadCount) : mThreadCount(threadCount), mThreads(std::make_unique<std::thread[]>(threadCount))
  {
    create_threads();
  }
  ~ThreadPool()
  {
    waitEmpty();
    destroy_threads();
  }
  auto execute(std::coroutine_handle<> handle) -> void
  {
    {
      auto lk = std::scoped_lock(mTaskMt);
      mTasks.push(std::move(handle));
    }
    ++mTaskTotal;
    mTaskAvailableCV.notify_one();
  }
  void waitEmpty()
  {
    mWaiting = true;
    std::unique_lock<std::mutex> tasks_lock(mTaskMt);
    mTaskDoneCV.wait(tasks_lock, [this] { return (mTaskTotal == 0); });
    mWaiting = false;
  }

private:
  auto create_threads() -> void
  {
    mRunning = true;
    for (size_t i = 0; i < mThreadCount; ++i) {
      mThreads[i] = std::thread(&ThreadPool::worker, this);
    }
  }

  auto destroy_threads() -> void
  {
    mRunning = false;
    mTaskAvailableCV.notify_all();
    for (size_t i = 0; i < mThreadCount; ++i) {
      mThreads[i].join();
    }
  }

  auto worker() -> void
  {
    while (mRunning) {
      std::function<void()> task;
      std::unique_lock<std::mutex> tasks_lock(mTaskMt);
      mTaskAvailableCV.wait(tasks_lock, [this] { return !mTasks.empty() || !mRunning; });
      if (mRunning) {
        task = std::move(mTasks.front());
        mTasks.pop();
        tasks_lock.unlock();
        task();
        tasks_lock.lock();
        --mTaskTotal;
        if (mWaiting) {
          mTaskDoneCV.notify_one();
        }
      }
    }
  }

  std::atomic_bool mRunning = false;
  std::condition_variable mTaskAvailableCV = {};
  std::condition_variable mTaskDoneCV = {};
  std::queue<std::coroutine_handle<>> mTasks = {};
  std::atomic_size_t mTaskTotal = 0;
  mutable std::mutex mTaskMt = {};
  size_t mThreadCount = 0;
  std::unique_ptr<std::thread[]> mThreads = nullptr;
  std::atomic_bool mWaiting = false;
};

class MutilThreadExecutor {
public:
  MutilThreadExecutor(size_t n) : mPool(n) {}

  auto spawnDetach(Task<> in, async::Reactor& reactor) -> void
  {
    mSpawnCount.fetch_add(1, std::memory_order_acquire);
    auto afterDoneFn = [this, &reactor]() {
      mSpawnCount.fetch_sub(1, std::memory_order_release);
      reactor.notify();
    };
    auto handle = [this](Task<> task)
        -> DetachTask<void> { co_return co_await task; }(std::move(in)).afterDestroy(afterDoneFn).handle;
    mPool.execute(handle);
  }

  template <typename T>
  [[nodiscard]] auto block(Task<T> in, async::Reactor& reactor) -> T
  {
    auto promise = std::make_shared<std::promise<T>>();
    auto future = promise->get_future();

    if constexpr (std::is_void_v<T>) { // return void
      auto afterDoneFn = [this, promise, &reactor]() {
        promise->set_value();
        reactor.notify();
      };
      auto handle = [](Task<T> task)
          -> DetachTask<T> { co_return co_await task; }(std::move(in)).afterDestroy(std::move(afterDoneFn)).handle;
      mPool.execute(handle);
    } else {
      auto afterDoneFn = [promise, &reactor](auto&& value) {
        promise->set_value(std::forward<T>(value));
        reactor.notify();
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
      reactor.lock().react(std::nullopt, mPool);
    }
    return future.get();
  }

  template <typename... Args>
  auto blockSpawn(Args&&... args)
  {
    return mBlockingExecutor.blockSpawn(std::forward<Args>(args)...);
  }

private:
  BlockingExecutor<MutilThreadExecutor> mBlockingExecutor;

  std::atomic_size_t mSpawnCount;
  ThreadPool mPool;
};

class InlineExecutor {
public:
  InlineExecutor() : mSpawnCount(0), mQueue() {}
  auto spawnDetach(Task<> task, async::Reactor& reactor) -> void
  {
    mSpawnCount += 1;
    auto afterDestroyFn = [this, &reactor]() {
      mSpawnCount -= 1;
      reactor.notify();
    };
    auto handle = [this](Task<> task) -> DetachTask<void> { co_return co_await task; }(std::move(task))
                                             .afterDestroy(std::move(afterDestroyFn))
                                             .handle;
    mQueue.push(handle);
  }

  template <typename T>
    requires(not std::is_void_v<T>)
  auto block(Task<T> task, async::Reactor& reactor) -> T
  {
    auto returnValue = std::optional<T>(std::nullopt);
    auto afterDestroyFn = [&reactor, &returnValue](T&& value) {
      returnValue.emplace(std::move(value));
      reactor.notify();
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
      reactor.lock().react(std::nullopt, *this);
    }

    return std::move(returnValue.value());
  }
  auto block(Task<> task, async::Reactor& reactor) -> void
  {
    auto hasValue = false;
    auto afterDestroyFn = [&reactor, &hasValue]() {
      hasValue = true;
      reactor.notify();
    };
    auto handle = [this](Task<> task) -> DetachTask<void> { co_return co_await task; }(std::move(task))
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
      reactor.lock().react(std::nullopt, *this);
    }
  }

  template <typename... Args>
  auto blockSpawn(Args&&... args)
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