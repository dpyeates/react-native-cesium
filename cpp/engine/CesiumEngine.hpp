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
#include <thread>

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

  // Returns merged eye-relative geometry + matrices for one frame.
  FrameResult updateFrame(double viewportWidth, double viewportHeight);

  // Switch the active imagery overlay. assetId==1 means terrain-only (no overlay).
  // Tears down and recreates the tileset to ensure a clean slate; the overlay is
  // applied on the next updateFrame() call.
  void setImageryAssetId(int64_t assetId);

  // Directly queue an imagery overlay to be applied on the next updateFrame().
  // Only safe to call on a freshly-initialised engine (no prior async state).
  void queueImageryOverlay(int64_t assetId);

  GlobeCamera&       camera()       { return camera_; }
  const GlobeCamera& camera() const { return camera_; }

  ResourcePreparer* getResourcePreparer() const { return resourcePreparer_.get(); }

private:
  // Creates a fresh Tileset.  If imageryAssetId != 1, the IonRasterOverlay is
  // added IMMEDIATELY after construction (before any async work has started)
  // to avoid cross-thread IntrusivePointer assertions that arise when the
  // overlay is added after the async pipeline has begun.
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

  // Thread on which this engine was constructed.  Used by instrumentation to
  // verify that overlay operations are always performed on the same thread.
  std::thread::id constructionThreadId_;

  GlobeCamera          camera_;
  TileLifecycleManager lifecycle_;

  uint64_t frameCount_ = 0;
};

} // namespace reactnativecesium
