#pragma once

#include "../renderer/IGPUBackend.hpp"
#include "../renderer/RenderTypes.hpp"
#include "GlobeCamera.hpp"
#include "ResourcePreparer.hpp"
#include "TaskProcessor.hpp"
#include "TileLifecycleManager.hpp"

#include <CesiumAsync/AsyncSystem.h>
#include <CesiumAsync/IAssetAccessor.h>

#include <memory>
#include <string>

namespace Cesium3DTilesSelection {
class Tileset;
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

  // Fills |result| with merged eye-relative geometry + matrices for one frame.
  // Passing a persistent FrameResult from the caller avoids per-frame heap
  // allocation: clear() keeps the vector capacity, so after the first frame
  // the internal std::vectors never reallocate.
  void updateFrame(double viewportWidth, double viewportHeight, FrameResult& result);

  // Switch the active imagery overlay. assetId==1 means terrain-only (no overlay).
  // Tears down and recreates the tileset so the overlay is applied cleanly.
  void setImageryAssetId(int64_t assetId);

  GlobeCamera&       camera()       { return camera_; }
  const GlobeCamera& camera() const { return camera_; }

  ResourcePreparer* getResourcePreparer() const { return resourcePreparer_.get(); }

private:
  void createTileset(const std::string& token,
                     int64_t            assetId,
                     int64_t            imageryAssetId = 1);
  void destroyTileset();

  IGPUBackend* gpu_ = nullptr;
  EngineConfig config_;

  std::shared_ptr<TaskProcessor>               taskProcessor_;
  std::shared_ptr<CesiumAsync::IAssetAccessor> assetAccessor_;
  CesiumAsync::AsyncSystem                     asyncSystem_;
  std::shared_ptr<ResourcePreparer>            resourcePreparer_;

  std::unique_ptr<Cesium3DTilesSelection::Tileset> tileset_;

  // Asset ID of the active raster overlay (1 = terrain-only, no overlay).
  // Stored so it can be re-applied when the tileset is rebuilt.
  int64_t currentImageryAssetId_ = 1;

  GlobeCamera          camera_;
  TileLifecycleManager lifecycle_;

  uint64_t frameCount_ = 0;
};

} // namespace reactnativecesium
