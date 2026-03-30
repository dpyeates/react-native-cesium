#include "TaskProcessor.hpp"

namespace reactnativecesium {

TaskProcessor::TaskProcessor(uint32_t numThreads) {
  workers_.reserve(numThreads);
  for (uint32_t i = 0; i < numThreads; ++i) {
    workers_.emplace_back(&TaskProcessor::workerLoop, this);
  }
}

TaskProcessor::~TaskProcessor() {
  shutdown_.store(true, std::memory_order_release);
  cv_.notify_all();
  for (auto& t : workers_) {
    if (t.joinable()) {
      t.join();
    }
  }
}

void TaskProcessor::startTask(std::function<void()> f) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_.push(std::move(f));
  }
  cv_.notify_one();
}

void TaskProcessor::workerLoop() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] {
        return shutdown_.load(std::memory_order_acquire) || !tasks_.empty();
      });
      if (shutdown_.load(std::memory_order_acquire) && tasks_.empty()) {
        return;
      }
      task = std::move(tasks_.front());
      tasks_.pop();
    }
    task();
  }
}

} // namespace reactnativecesium
