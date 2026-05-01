#pragma once

#include "../renderer/RenderTypes.hpp"
#include "EngineTunables.hpp"
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

namespace CesiumUtility {
class CreditSystem;
}

namespace CesiumRasterOverlays {
class RasterOverlay;
}

namespace reactnativecesium {

// Mirror of Cesium Native TilesetOptions / SqliteCache + a few of our own
// knobs. Default values match the values previously hardcoded in
// CesiumEngine.cpp so existing call sites that pass an EngineConfig with only
// the few "core" fields populated continue to behave as before.
struct EngineConfig {
  // ── Cesium Ion / network ──────────────────────────────────────────────
  std::string ionAccessToken;
  int64_t     ionAssetId        = 1;
  int64_t     ionImageryAssetId = 1; // 1 == "no overlay (use built-in colours)"
  std::string cacheDatabasePath;
  std::string tlsCaBundlePath;

  // ── Tileset selection (TilesetOptions) ───────────────────────────────
  double  maximumScreenSpaceError      = tunables::kDefaultMaximumScreenSpaceError;
  int32_t maximumSimultaneousTileLoads = tunables::kDefaultMaximumSimultaneousTileLoads;
  int32_t loadingDescendantLimit       = tunables::kDefaultLoadingDescendantLimit;

  // RAM budget for decoded geometry/textures held by the live tileset.
  int64_t maximumCachedBytes = tunables::kDefaultMaxCachedBytes;

  // Pre-load extra tiles around what is strictly needed for this view —
  // smoothes panning / hides pop-in but increases peak load pressure.
  bool preloadAncestors = true;
  bool preloadSiblings  = true;

  // Refuse to render holes in the terrain (Cesium will keep ancestor tiles
  // visible until all children load). Relax during fast pans to lower
  // upload pressure.
  bool forbidHoles = true;

  // Water-mask textures (carries coastline shading data). Cheap to disable on
  // memory-constrained devices.
  bool enableWaterMask = true;

  // ── Optional / off-by-default selection knobs ────────────────────────
  bool   enableFogCulling             = false;
  bool   enforceCulledScreenSpaceError = true;
  double culledScreenSpaceError       = 64.0;
  bool   enableLodTransitionPeriod    = false;
  double lodTransitionLength          = 1.0;
  // Seconds an unused tile remains in memory before being evicted.
  double tileCacheUnloadTimeInSeconds = 5.0;

  // ── Disk cache (SqliteCache) ─────────────────────────────────────────
  int32_t sqliteCacheMaxRows = tunables::kDefaultSqliteCacheMaxRows;

  // ── Worker pool ──────────────────────────────────────────────────────
  // 0 means "auto-detect" via std::thread::hardware_concurrency() clamped to
  // [2, 8]. Override only for benchmarking / regression testing.
  int32_t taskProcessorThreads = 0;
};

class CesiumEngine {
public:
  CesiumEngine();
  ~CesiumEngine();

  CesiumEngine(const CesiumEngine&)            = delete;
  CesiumEngine& operator=(const CesiumEngine&) = delete;

  void initialize(const EngineConfig& config);
  void shutdown();

  // Apply config changes. Splits cleanly into:
  //   - "tileset must be rebuilt" (token / assetId changed) → destroy + recreate,
  //   - "runtime-mutable" (SSE / load limits / cache bytes / ...) →
  //     mutate tileset_->getOptions() in place,
  //   - "imagery overlay only" → IRasterOverlay add/remove.
  void updateConfig(const EngineConfig& config);

  void updateFrame(double viewportWidth, double viewportHeight, FrameResult& result);

  void setImageryAssetId(int64_t assetId);

  GlobeCamera&       camera()       { return camera_; }
  const GlobeCamera& camera() const { return camera_; }

  ResourcePreparer* getResourcePreparer() const { return resourcePreparer_.get(); }

  // Expose a few useful read-only bits to platform bridges (avoids leaking
  // EngineConfig everywhere).
  bool tilesetReady() const { return tileset_ != nullptr; }
  int64_t currentImageryAssetId() const { return currentImageryAssetId_; }

private:
  void createTileset();
  void destroyTileset();

  // Apply runtime-mutable bits of `cfg` to the live tileset. Returns true if a
  // value actually changed (callers can use this to trigger force-render).
  bool applyRuntimeConfig(const EngineConfig& cfg);

  // Apply imagery overlay change (target assetId of 1 means "no overlay").
  void applyImageryOverlay(int64_t targetAssetId);

  static int32_t resolveTaskThreadCount(int32_t requested);

  // Builds a tessellated WGS84 ellipsoid mesh (inset ~20 m) used as a
  // fallback background while terrain tiles are not yet ready.
  void buildEllipsoidMesh();
  void appendEllipsoidDraws(FrameResult& result) const;

  EngineConfig config_;

  std::shared_ptr<TaskProcessor>               taskProcessor_;
  std::shared_ptr<CesiumAsync::IAssetAccessor> assetAccessor_;
  CesiumAsync::AsyncSystem                     asyncSystem_;
  std::shared_ptr<ResourcePreparer>            resourcePreparer_;
  std::shared_ptr<CesiumUtility::CreditSystem> creditSystem_;

  std::unique_ptr<Cesium3DTilesSelection::Tileset> tileset_;

  // Currently-active overlay — kept so we can remove it cheaply when the
  // imagery asset id changes.
  CesiumUtility::IntrusivePointer<CesiumRasterOverlays::RasterOverlay>
      currentOverlay_;
  int64_t currentImageryAssetId_ = 1;

  GlobeCamera          camera_;
  TileLifecycleManager lifecycle_;

  // Pre-computed fallback ellipsoid geometry (ECEF, absolute).
  std::vector<glm::dvec3> ellipsoidPositions_;
  std::vector<uint32_t>   ellipsoidIndices_;
};

} // namespace reactnativecesium
