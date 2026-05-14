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

#include <chrono>
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
  // visible until all children load).  Cesium Native's default is `false`.
  //
  // We default to `false` (matching Cesium Native) because `true` serialises
  // the load tree on the deepest-child critical path: a parent at LOD N
  // cannot be rendered until every descendant down to the selected LOD has
  // arrived.  For a typical Alps view that's 5-6 levels deep × ~4 tiles per
  // level fan-out = a ~30-60 s critical path even with cache hits.  Setting
  // this to `false` lets the renderer show the best tile it has *now* and
  // refine progressively — the same UX as CesiumJS / Google Earth.
  // Brief low-LOD pop-ins during fast pans are an acceptable trade for
  // first-frame correctness in seconds rather than tens of seconds.
  bool forbidHoles = false;

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

  // Soft cap (ms) on per-frame main-thread tile finalisation work.  Cesium
  // Native's default of 0.0 means "complete every pending main-thread load
  // every tick", which on a heavy startup burst can stall the render thread
  // for hundreds of ms at a time — the user sees the scene "lock up" while
  // tiles upload.  20 ms keeps the render thread responsive at 30 Hz minimum
  // (16.67 ms = one frame at 60 Hz; we sacrifice the worst-case frame during
  // startup bursts but never go below 30 Hz interactivity).  Lower values
  // (5–10 ms) make individual frames smoother but visibly extend total load
  // time because finalisation is throughput-bound: ~5 ms per tile, so a
  // 5 ms cap → only 1 tile finalised per frame → ~60 tiles/sec ceiling
  // regardless of how many CPUs are decoding in parallel.
  double mainThreadLoadingTimeLimitMs = 20.0;

  // ── Disk cache (SqliteCache) ─────────────────────────────────────────
  int32_t sqliteCacheMaxRows = tunables::kDefaultSqliteCacheMaxRows;

  // ── Worker pool ──────────────────────────────────────────────────────
  // 0 means "auto-detect" via std::thread::hardware_concurrency() clamped to
  // [2, 16]. Override only for benchmarking / regression testing.
  int32_t taskProcessorThreads = 0;

  // ── Camera constraints ───────────────────────────────────────────────
  // Minimum altitude above the loaded terrain surface (metres MSL).
  // 0 = no constraint (default). Typical use: 2.0 to prevent going underground.
  float minAltitudeAboveTerrain = 0.0f;
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

  const EngineConfig& getConfig() const { return config_; }

  // Ellipsoid height (metres) of the terrain vertex nearest to the camera nadir,
  // updated each frame from the loaded tile mesh. 0.0 until the first terrain
  // tile covering the camera position has been rendered.
  float terrainFloorEllipsoidMeters() const { return terrainFloorEllipsoidM_; }

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

  // Ellipsoid height of the terrain surface at the camera nadir, updated each
  // frame. Initialised to 0 (sea level) until a terrain tile is first loaded.
  float terrainFloorEllipsoidM_ = 0.0f;

  // Wall-clock timestamp of the previous updateFrame call. Used to compute
  // deltaTime for Tileset::updateViewGroup, which drives LOD transition fading.
  // Zero-initialised "not yet set" state — first frame passes 0 deltaTime.
  std::chrono::steady_clock::time_point lastFrameTime_{};
  bool                                  hasLastFrameTime_ = false;

  // Pre-computed fallback ellipsoid geometry (ECEF, absolute).
  std::vector<glm::dvec3> ellipsoidPositions_;
  std::vector<uint32_t>   ellipsoidIndices_;

  // ── frame-signature cache ─────────────────────────────────────────
  // Signature is hashed over the tilesToRenderThisFrame primitive pointers
  // and counts. When the same signature appears for kMaxFramesInFlight
  // consecutive frames *and* no LOD fades are active, we know every backend
  // slot's persistent geometry buffer already holds the right bytes, so we
  // can skip rebuilding the merged CPU arrays entirely. On the fast path we
  // still emit one DrawPrimitive per visible primitive (re-reading overlay /
  // water-mask state from TileGPUResources and recomputing per-tile MVP for
  // camera motion), but the heavy per-vertex memcpys are gone.
  struct CachedDraw {
    const TileGPUResources* res;
    glm::dvec3              rtcCenter;
    uint32_t                indexByteOffset;
    uint32_t                indexCount;
    bool                    hasUVs;
    glm::vec4               wmTileBounds;
  };
  std::vector<CachedDraw> cachedDraws_;
  uint64_t                lastDrawSignature_    = 0;
  // Consecutive frames where geometrySignature has been the same non-zero
  // value. Once this reaches kMaxFramesInFlight every persistent buffer slot
  // is guaranteed to contain the right bytes — fast path becomes legal.
  int                     stableSigFrames_      = 0;

  // ── terrain-floor scan cache ──────────────────────────────────────
  // The minAltitudeAboveTerrain clamp scans every vertex of every tile whose
  // bounding region contains the camera lon/lat and picks the altitude of
  // the vertex with the smallest 3D distance to the camera. That's an O(NV)
  // scan over the visible primitives — dominant CPU cost when the camera
  // sits over a dense LOD chain. Result depends only on the camera lon/lat
  // and the set of visible primitives, so we cache it keyed on
  // (camLon, camLat, geometrySignature). Threshold of ~1e-6 degrees (~10 cm)
  // is far below the inter-vertex spacing of the densest tiles served by
  // Cesium Ion world terrain, so the cached nearest vertex stays correct.
  bool     floorCacheValid_      = false;
  double   floorCacheCamLonDeg_  = 0.0;
  double   floorCacheCamLatDeg_  = 0.0;
  uint64_t floorCacheSignature_  = 0;
  float    floorCacheValue_      = 0.0f;
};

} // namespace reactnativecesium
