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

namespace CesiumUtility {
class CreditSystem;
}

namespace reactnativecesium {

struct EngineConfig {
  std::string ionAccessToken;
  int64_t     ionAssetId = 1;
  std::string cacheDatabasePath;
  std::string tlsCaBundlePath;
  double maximumScreenSpaceError      = 32.0;
  int32_t maximumSimultaneousTileLoads = 12;
  int32_t loadingDescendantLimit       = 20;
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

  void updateFrame(double viewportWidth, double viewportHeight, FrameResult& result);

  void setImageryAssetId(int64_t assetId);

  GlobeCamera&       camera()       { return camera_; }
  const GlobeCamera& camera() const { return camera_; }

  ResourcePreparer* getResourcePreparer() const { return resourcePreparer_.get(); }

private:
  void createTileset(const std::string& token,
                     int64_t            assetId,
                     int64_t            imageryAssetId = 1);
  void destroyTileset();

  bool tilesetOptionsMatch(const EngineConfig& a, const EngineConfig& b) const;

  // Builds a tessellated WGS84 ellipsoid mesh (inset ~50 m) used as a
  // permanent background so flat terrain is always visible even when tiles
  // are absent or not yet loaded.
  void buildEllipsoidMesh();
  void appendEllipsoidDraws(FrameResult& result) const;

  IGPUBackend* gpu_ = nullptr;
  EngineConfig config_;

  std::shared_ptr<TaskProcessor>               taskProcessor_;
  std::shared_ptr<CesiumAsync::IAssetAccessor> assetAccessor_;
  CesiumAsync::AsyncSystem                     asyncSystem_;
  std::shared_ptr<ResourcePreparer>            resourcePreparer_;
  std::shared_ptr<CesiumUtility::CreditSystem> creditSystem_;

  std::unique_ptr<Cesium3DTilesSelection::Tileset> tileset_;

  int64_t currentImageryAssetId_ = 1;

  GlobeCamera          camera_;
  TileLifecycleManager lifecycle_;

  uint64_t frameCount_ = 0;

  // Pre-computed fallback ellipsoid geometry (ECEF, absolute).
  std::vector<glm::dvec3> ellipsoidPositions_;
  std::vector<uint32_t>   ellipsoidIndices_;
};

} // namespace reactnativecesium
