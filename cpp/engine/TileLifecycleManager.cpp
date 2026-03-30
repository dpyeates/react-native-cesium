#include "TileLifecycleManager.hpp"

#include <Cesium3DTilesSelection/TileContent.h>
#include <Cesium3DTilesSelection/TileSelectionState.h>

namespace reactnativecesium {

void TileLifecycleManager::stampTileUsed(TileGPUResources* resources) {
  if (resources) {
    resources->lastUsedFrame = frameNumber_;
  }
}

bool TileLifecycleManager::shouldDeferFree(
    const TileGPUResources* resources) const {
  if (!resources)
    return false;
  return resources->lastUsedFrame >= frameNumber_;
}

bool TileLifecycleManager::isRefinementLocked(
    const Cesium3DTilesSelection::Tile& /*tile*/) const {
  // Cesium Native guarantees that free() is not called on tiles that are
  // currently needed for rendering (including refined parent tiles whose
  // children are loading). Therefore, explicit refinement locking is not
  // required here.
  return false;
}

} // namespace reactnativecesium
