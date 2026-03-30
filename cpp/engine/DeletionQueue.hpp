#pragma once

#include "../renderer/RenderTypes.hpp"

#include <cstdint>
#include <deque>

namespace reactnativecesium {

// CPU-only resource deletion with a frame delay (avoids deleting in-flight data).
class DeletionQueue {
public:
  static constexpr uint64_t MIN_FRAME_DELAY       = 3;
  static constexpr int      MAX_DISPOSALS_PER_FRAME = 8;

  void enqueue(TileGPUResources* resources, uint64_t currentFrame);
  void processFrame(uint64_t currentFrame);
  void flush();

private:
  struct Entry {
    TileGPUResources* resources;
    uint64_t          enqueueFrame;
  };
  std::deque<Entry> queue_;
};

} // namespace reactnativecesium
