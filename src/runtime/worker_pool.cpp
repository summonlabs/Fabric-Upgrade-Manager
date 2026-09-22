#include "fum/runtime/worker_pool.hpp"

#include <algorithm>

namespace fum {

WorkerPool::WorkerPool(Options options) : options_(options) {
  if (options_.threads == 0) {
    options_.threads = 1;
  }
  if (options_.max_queue == 0) {
    options_.max_queue = 1;
  }
  workers_.reserve(options_.threads);
  worker_ids_.reserve(options_.threads);
  for (std::size_t i = 0; i < options_.threads; ++i) {
    workers_.emplace_back([this, i] { run_worker(i); });
  }
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    worker_ids_.clear();
    for (const auto& worker : workers_) {
      worker_ids_.push_back(worker.get_id());
    }
  }
}

WorkerPool::~WorkerPool() { stop(); }

bool WorkerPool::is_pool_thread() const {
  const std::thread::id self = std::this_thread::get_id();
  const std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& id : worker_ids_) {
    if (id == self) {
      return true;
    }
  }
  return false;
}

void WorkerPool::run_worker(std::size_t index) {
  static_cast<void>(index);
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      work_available_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (stopping_) {
        return;
      }
      task = std::move(queue_.front());
      queue_.pop_front();
      ++active_;
    }
    if (cancel_requested_.load()) {
      // Cancelled work is retired without executing; accounting stays honest.
      const std::lock_guard<std::mutex> lock(mutex_);
      --active_;
      ++stats_.discarded;
      if (active_ == 0 && queue_.empty()) {
        idle_.notify_all();
      }
      continue;
    }
    task();
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      --active_;
      ++stats_.completed;
      if (active_ == 0 && queue_.empty()) {
        idle_.notify_all();
      }
    }
  }
}

Status WorkerPool::submit(std::function<void()> task) {
  if (!task) {
    return make_error(ErrorCode::invalid_argument, "task must be callable");
  }
  if (stopped_.load()) {
    const std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.rejected;
    return make_error(ErrorCode::closed, "worker pool is stopped");
  }
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      ++stats_.rejected;
      return make_error(ErrorCode::closed, "worker pool is stopping");
    }
    if (queue_.size() >= options_.max_queue) {
      ++stats_.rejected;
      return make_error(ErrorCode::resource_exhausted, "worker pool queue is full",
                        std::to_string(queue_.size()));
    }
    queue_.push_back(std::move(task));
    ++stats_.submitted;
  }
  work_available_.notify_one();
  return ok_status();
}

Status WorkerPool::drain() {
  if (is_pool_thread()) {
    return make_error(ErrorCode::internal,
                      "drain() must not be called from a pool worker thread");
  }
  std::unique_lock<std::mutex> lock(mutex_);
  idle_.wait(lock, [this] { return queue_.empty() && active_ == 0; });
  return ok_status();
}

void WorkerPool::stop() {
  if (stopped_.load()) {
    return;
  }
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      // Another thread is already stopping; wait for the workers to finish.
    } else {
      stopping_ = true;
      cancel_requested_.store(true);
      stats_.discarded += queue_.size();
      queue_.clear();
    }
  }
  work_available_.notify_all();
  for (auto& worker : workers_) {
    if (worker.joinable()) {
      if (worker.get_id() == std::this_thread::get_id()) {
        worker.detach();
        continue;
      }
      worker.join();
    }
  }
  stopped_.store(true);
}

std::size_t WorkerPool::pending() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

std::size_t WorkerPool::active() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return active_;
}

WorkerPool::Stats WorkerPool::stats() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

}  // namespace fum
