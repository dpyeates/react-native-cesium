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

  // Block the calling thread until the task queue is empty and every
  // in-flight task has returned.  Worker tasks may post main-thread
  // callbacks (via the AsyncSystem) and return without waiting for them,
  // so this does not deadlock when called from the main thread.
  void waitUntilIdle();

private:
  void workerLoop();

  std::vector<std::thread>           workers_;
  std::queue<std::function<void()>>  tasks_;
  std::mutex                         mutex_;
  std::condition_variable            cv_;
  std::condition_variable            idleCv_;   // signalled when queue + active reach 0
  std::atomic<bool>                  shutdown_{false};
  uint32_t                           activeTasks_{0}; // guarded by mutex_
};

} // namespace reactnativecesium
