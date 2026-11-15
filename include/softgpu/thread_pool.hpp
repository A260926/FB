#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace softgpu {

// Fixed set of workers plus the calling thread. run(n, fn) hands out indices 0..n-1 through
// one atomic counter and returns when every index has been processed. No work stealing:
// items (tiles, vertex chunks) are uniform enough that a shared counter balances fine.
class ThreadPool {
 public:
  explicit ThreadPool(int threads) : threads_(threads < 1 ? 1 : threads) {
    for (int i = 1; i < threads_; ++i) workers_.emplace_back([this] { worker(); });
  }
  ~ThreadPool() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      quit_ = true;
      ++generation_;
    }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
  }
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  int size() const { return threads_; }

  void run(size_t n, const std::function<void(size_t)>& fn) {
    if (n == 0) return;
    if (threads_ == 1 || n == 1) {
      for (size_t i = 0; i < n; ++i) fn(i);
      return;
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      job_n_ = n;
      job_fn_ = &fn;
      next_.store(0);
      active_ = static_cast<int>(workers_.size());
      ++generation_;
    }
    cv_.notify_all();
    work();
    std::unique_lock<std::mutex> lk(mu_);
    done_cv_.wait(lk, [this] { return active_ == 0; });
    job_fn_ = nullptr;
  }

 private:
  void work() {
    for (;;) {
      const size_t i = next_.fetch_add(1);
      if (i >= job_n_) break;
      (*job_fn_)(i);
    }
  }
  void worker() {
    uint64_t seen = 0;
    for (;;) {
      {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [&] { return generation_ != seen; });
        seen = generation_;
        if (quit_) return;
      }
      work();
      std::lock_guard<std::mutex> lk(mu_);
      if (--active_ == 0) done_cv_.notify_one();
    }
  }

  int threads_;
  std::vector<std::thread> workers_;
  std::mutex mu_;
  std::condition_variable cv_, done_cv_;
  uint64_t generation_ = 0;
  bool quit_ = false;
  size_t job_n_ = 0;
  const std::function<void(size_t)>* job_fn_ = nullptr;
  std::atomic<size_t> next_{0};
  int active_ = 0;
};

}  // namespace softgpu
