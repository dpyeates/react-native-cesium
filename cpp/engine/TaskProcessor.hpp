#pragma once

#include <CesiumAsync/ITaskProcessor.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace reactnativecesium {

class TaskProcessor : public CesiumAsync::ITaskProcessor {
public:
  explicit TaskProcessor(uint32_t numThreads = 4);
  ~TaskProcessor() override;

  TaskProcessor(const TaskProcessor&) = delete;
  TaskProcessor& operator=(const TaskProcessor&) = delete;

  void startTask(std::function<void()> f) override;

private:
  void workerLoop();

  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> tasks_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::atomic<bool> shutdown_{false};
};

} // namespace reactnativecesium
