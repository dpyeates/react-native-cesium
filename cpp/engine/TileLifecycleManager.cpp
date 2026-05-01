#include "TileLifecycleManager.hpp"

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

} // namespace reactnativecesium
