#include "DeletionQueue.hpp"

namespace reactnativecesium {

void DeletionQueue::enqueue(TileGPUResources* resources, uint64_t currentFrame) {
  if (!resources) return;
  queue_.push_back({resources, currentFrame});
}

void DeletionQueue::processFrame(uint64_t currentFrame) {
  int disposed = 0;
  while (!queue_.empty() && disposed < MAX_DISPOSALS_PER_FRAME) {
    auto& front = queue_.front();
    if (currentFrame - front.enqueueFrame < MIN_FRAME_DELAY) break;
    delete front.resources;
    queue_.pop_front();
    ++disposed;
  }
}

void DeletionQueue::flush() {
  while (!queue_.empty()) {
    delete queue_.front().resources;
    queue_.pop_front();
  }
}

} // namespace reactnativecesium
