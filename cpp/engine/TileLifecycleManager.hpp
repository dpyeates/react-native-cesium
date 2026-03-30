#pragma once

#include "../renderer/RenderTypes.hpp"

#include <Cesium3DTilesSelection/Tile.h>

#include <cstdint>

namespace reactnativecesium {

class TileLifecycleManager {
public:
  uint64_t currentFrame() const { return frameNumber_; }
  void advanceFrame() { ++frameNumber_; }

  void stampTileUsed(TileGPUResources* resources);

  bool shouldDeferFree(const TileGPUResources* resources) const;
  bool isRefinementLocked(const Cesium3DTilesSelection::Tile& tile) const;

private:
  uint64_t frameNumber_ = 0;
};

} // namespace reactnativecesium
