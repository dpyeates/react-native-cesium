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

  // Returns the number of tasks currently executing on worker threads.
  // Queued-but-not-yet-started tasks are not counted.
  uint32_t getActiveTaskCount() const;
  uint32_t getQueuedTaskCount() const;

private:
  void workerLoop();

  std::vector<std::thread>           workers_;
  std::queue<std::function<void()>>  tasks_;
  mutable std::mutex                 mutex_; // mutable: const getters may lock
  std::condition_variable            cv_;
  std::condition_variable            idleCv_;   // signalled when queue + active reach 0
  std::atomic<bool>                  shutdown_{false};
  uint32_t                           activeTasks_{0}; // guarded by mutex_
};

} // namespace reactnativecesium
