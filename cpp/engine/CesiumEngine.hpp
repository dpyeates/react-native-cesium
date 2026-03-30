#pragma once

#include "../renderer/IGPUBackend.hpp"
#include "../renderer/RenderTypes.hpp"
#include "GlobeCamera.hpp"
#include "ResourcePreparer.hpp"
#include "TaskProcessor.hpp"
#include "TileLifecycleManager.hpp"

#include <CesiumAsync/AsyncSystem.h>
#include <CesiumAsync/IAssetAccessor.h>
#include <CesiumUtility/IntrusivePointer.h>

#include <memory>
#include <string>

namespace Cesium3DTilesSelection {
class Tileset;
}
namespace CesiumRasterOverlays {
class RasterOverlay;
}

namespace reactnativecesium {

struct EngineConfig {
  std::string ionAccessToken;
  int64_t     ionAssetId         = 1;
  std::string cacheDatabasePath;
};

class CesiumEngine {
public:
  CesiumEngine();
  ~CesiumEngine();

  CesiumEngine(const CesiumEngine&)            = delete;
  CesiumEngine& operator=(const CesiumEngine&) = delete;

  void initialize(IGPUBackend& gpu, const EngineConfig& config);
  void shutdown();
  void updateConfig(const EngineConfig& config);

  // Returns merged eye-relative geometry + matrices for one frame.
  FrameResult updateFrame(double viewportWidth, double viewportHeight);

  // Switch the active imagery overlay. assetId==1 means terrain-only (no overlay).
  void setImageryAssetId(int64_t assetId);

  GlobeCamera&       camera()       { return camera_; }
  const GlobeCamera& camera() const { return camera_; }

  ResourcePreparer* getResourcePreparer() const { return resourcePreparer_.get(); }

private:
  void createTileset(const std::string& token, int64_t assetId);
  void destroyTileset();

  IGPUBackend* gpu_ = nullptr;
  EngineConfig config_;

  std::shared_ptr<TaskProcessor>               taskProcessor_;
  std::shared_ptr<CesiumAsync::IAssetAccessor> assetAccessor_;
  CesiumAsync::AsyncSystem                     asyncSystem_;
  std::shared_ptr<ResourcePreparer>            resourcePreparer_;

  std::unique_ptr<Cesium3DTilesSelection::Tileset> tileset_;

  // Intrusive pointer to the current raster overlay (kept alive here; removed from
  // tileset_->getOverlays() when switching layers).
  CesiumUtility::IntrusivePointer<CesiumRasterOverlays::RasterOverlay> currentOverlay_;

  GlobeCamera          camera_;
  TileLifecycleManager lifecycle_;

  uint64_t frameCount_ = 0;
};

} // namespace reactnativecesium
