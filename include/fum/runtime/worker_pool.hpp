// Bounded worker pool with real cancellation and shutdown accounting.
//
// Lock ordering: the pool has exactly one mutex; it is never held while
// invoking a task, and drain()/stop() are never called from a pool thread.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "fum/core/result.hpp"

namespace fum {

class [[nodiscard]] WorkerPool {
 public:
  struct Options {
    std::size_t threads = 4;
    std::size_t max_queue = 1024;
  };

  struct Stats {
    std::uint64_t submitted = 0;
    std::uint64_t completed = 0;
    std::uint64_t rejected = 0;
    std::uint64_t discarded = 0;
  };

  explicit WorkerPool(Options options);
  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;
  ~WorkerPool();

  // Admission is refused when the queue is full or the pool is stopping, so a
  // caller can never block on an unbounded queue.
  [[nodiscard]] Status submit(std::function<void()> task);

  // Waits until every queued task has completed. Returns an error when called
  // from a pool thread (that would be a self-join deadlock).
  [[nodiscard]] Status drain();

  // Stops admission, discards queued work, cancels running work and joins.
  void stop();

  void request_cancel() noexcept { cancel_requested_.store(true); }
  [[nodiscard]] bool cancelled() const noexcept { return cancel_requested_.load(); }

  [[nodiscard]] std::size_t pending() const;
  [[nodiscard]] std::size_t active() const;
  [[nodiscard]] std::size_t threads() const noexcept { return workers_.size(); }
  [[nodiscard]] bool stopped() const noexcept { return stopped_.load(); }
  [[nodiscard]] Stats stats() const;

 private:
  void run_worker(std::size_t index);
  [[nodiscard]] bool is_pool_thread() const;

  Options options_;
  mutable std::mutex mutex_;
  std::condition_variable work_available_;
  std::condition_variable idle_;
  std::deque<std::function<void()>> queue_;
  std::vector<std::thread> workers_;
  std::vector<std::thread::id> worker_ids_;
  std::size_t active_ = 0;
  bool stopping_ = false;
  std::atomic<bool> stopped_{false};
  std::atomic<bool> cancel_requested_{false};
  Stats stats_;
};

}  // namespace fum
