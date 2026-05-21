#ifndef NDEBUG
# define NDEBUG
# define CESIUM_ENGINE_UNDEF_NDEBUG
#endif

#include "CesiumEngine.hpp"
#include "GeoidConverter.hpp"

#include <iostream>

#include <Cesium3DTilesContent/registerAllTileContentTypes.h>
#include <Cesium3DTilesSelection/BoundingVolume.h>
#include <Cesium3DTilesSelection/Tile.h>
#include <Cesium3DTilesSelection/Tileset.h>
#include <Cesium3DTilesSelection/TilesetExternals.h>
#include <Cesium3DTilesSelection/TilesetOptions.h>
#include <CesiumGeospatial/BoundingRegion.h>
#include <CesiumGeospatial/Ellipsoid.h>
#include <CesiumGeospatial/GlobeRectangle.h>
#include <CesiumGeospatial/GlobeTransforms.h>
#include <CesiumAsync/CachingAssetAccessor.h>
#include <CesiumAsync/SqliteCache.h>
#include <CesiumCurl/CurlAssetAccessor.h>
#include <CesiumRasterOverlays/IonRasterOverlay.h>
#include <CesiumRasterOverlays/RasterOverlay.h>
#include <CesiumUtility/CreditSystem.h>

#ifdef CESIUM_ENGINE_UNDEF_NDEBUG
# undef NDEBUG
# undef CESIUM_ENGINE_UNDEF_NDEBUG
#endif

#include <spdlog/spdlog.h>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>


namespace reactnativecesium {

int32_t CesiumEngine::resolveTaskThreadCount(int32_t requested) {
  if (requested > 0) return requested;
  unsigned int hw = std::thread::hardware_concurrency();
  if (hw == 0) return 4;
  // Reserve one core for the UI / display thread; clamp into a sensible band.
  // The upper bound was 8 historically but modern phones / tablets routinely
  // ship 8-10 cores (iPhone 15 Pro, iPad M2/M4, Pixel 8 Pro, Galaxy S24
  // Ultra) and tile decoding (Draco, KTX2, glTF parse) is embarrassingly
  // parallel — capping at 8 left half the CPU idle during cold-start tile
  // burst.  16 covers everything up to a Mac M4 Pro and is harmless on
  // smaller chips because we simply create empty workers.
  int32_t target = static_cast<int32_t>(hw) - 1;
  if (target < 2)  target = 2;
  if (target > 16) target = 16;
  return target;
}

CesiumEngine::CesiumEngine()
    : taskProcessor_(std::make_shared<TaskProcessor>(
          static_cast<uint32_t>(resolveTaskThreadCount(0)))),
      asyncSystem_(taskProcessor_) {
  Cesium3DTilesContent::registerAllTileContentTypes();
}

CesiumEngine::~CesiumEngine() { shutdown(); }

void CesiumEngine::buildEllipsoidMesh() {
  // Tessellate a WGS84 ellipsoid slightly inset so terrain tiles (at true
  // surface height) reliably overdraw it via the reversed-Z depth test.
  constexpr int nLon = 128;
  constexpr int nLat = 64;

  constexpr double a_full = 6378137.0;
  constexpr double b_full = 6356752.31424518;
  constexpr double inset  = 20.0;
  const double a = a_full - inset;
  const double b = b_full - inset;

  ellipsoidPositions_.clear();
  ellipsoidIndices_.clear();
  ellipsoidPositions_.reserve(static_cast<size_t>((nLat + 1) * nLon));
  ellipsoidIndices_.reserve(static_cast<size_t>(nLat * nLon * 6));

  for (int lat = 0; lat <= nLat; ++lat) {
    const double phi = M_PI * (static_cast<double>(lat) / nLat - 0.5);
    const double cosPhi = std::cos(phi);
    const double sinPhi = std::sin(phi);
    for (int lon = 0; lon < nLon; ++lon) {
      const double theta = 2.0 * M_PI * static_cast<double>(lon) / nLon;
      ellipsoidPositions_.push_back({
          a * cosPhi * std::cos(theta),
          a * cosPhi * std::sin(theta),
          b * sinPhi
      });
    }
  }

  for (int lat = 0; lat < nLat; ++lat) {
    for (int lon = 0; lon < nLon; ++lon) {
      const uint32_t v00 = static_cast<uint32_t>(lat       * nLon + lon);
      const uint32_t v10 = static_cast<uint32_t>(lat       * nLon + (lon + 1) % nLon);
      const uint32_t v01 = static_cast<uint32_t>((lat + 1) * nLon + lon);
      const uint32_t v11 = static_cast<uint32_t>((lat + 1) * nLon + (lon + 1) % nLon);
      ellipsoidIndices_.push_back(v00); ellipsoidIndices_.push_back(v10); ellipsoidIndices_.push_back(v11);
      ellipsoidIndices_.push_back(v00); ellipsoidIndices_.push_back(v11); ellipsoidIndices_.push_back(v01);
    }
  }
}

void CesiumEngine::appendEllipsoidDraws(FrameResult& result) const {
  if (ellipsoidPositions_.empty() || ellipsoidIndices_.empty()) return;

  const glm::dvec3 cameraPos  = camera_.getECEFPosition();
  const size_t     baseVertex  = result.localPositions.size() / 3;
  const uint32_t   indexByteOff =
      static_cast<uint32_t>(result.indices.size() * sizeof(uint32_t));
  const size_t vertexCount = ellipsoidPositions_.size();

  result.localPositions.resize(result.localPositions.size() + vertexCount * 3);
  result.altitudes.resize(result.altitudes.size() + vertexCount);
  result.uvs.resize(result.uvs.size() + vertexCount * 2);

  float* posOut = result.localPositions.data() + baseVertex * 3;
  float* altOut = result.altitudes.data()      + baseVertex;
  float* uvOut  = result.uvs.data()            + baseVertex * 2;

  for (const auto& posEcef : ellipsoidPositions_) {
    const glm::dvec3 local = posEcef - cameraPos;
    *posOut++ = static_cast<float>(local.x);
    *posOut++ = static_cast<float>(local.y);
    *posOut++ = static_cast<float>(local.z);
    *altOut++ = 0.0f;
    *uvOut++  = 0.5f;
    *uvOut++  = 0.5f;
  }

  const size_t indexOffset = result.indices.size();
  result.indices.resize(indexOffset + ellipsoidIndices_.size());
  uint32_t* indexOut = result.indices.data() + indexOffset;
  for (uint32_t idx : ellipsoidIndices_) {
    *indexOut++ = static_cast<uint32_t>(baseVertex) + idx;
  }

  DrawPrimitive draw;
  draw.indexByteOffset     = indexByteOff;
  draw.indexCount          = static_cast<uint32_t>(ellipsoidIndices_.size());
  draw.hasUVs              = false;
  draw.isEllipsoidFallback = true;
  draw.overlayTexture      = nullptr;
  draw.mvpMatrix           = result.vpMatrix;
  draw.rtcCenterEcef       = result.cameraEcef;
  result.draws.push_back(draw);
}

void CesiumEngine::initialize(const EngineConfig& config) {
  config_ = config;
  // Ensure ionImageryAssetId default is honoured if the caller passed 0/<0.
  if (config_.ionImageryAssetId <= 0) {
    config_.ionImageryAssetId = 1;
  }

  // Re-create the worker pool if the requested thread count differs from
  // the autodetected default we used in the constructor.
  const int32_t desiredThreads = resolveTaskThreadCount(config_.taskProcessorThreads);
  if (taskProcessor_) {
    // We can only resize the pool by replacing it, which would break
    // asyncSystem_. Skip when it's the same to preserve in-flight tasks.
    // Note: TaskProcessor stores its size privately, so we conservatively
    // recreate only when an explicit override is provided.
    if (config_.taskProcessorThreads > 0) {
      taskProcessor_->waitUntilIdle();
      taskProcessor_ = std::make_shared<TaskProcessor>(
          static_cast<uint32_t>(desiredThreads));
      asyncSystem_ = CesiumAsync::AsyncSystem(taskProcessor_);
    }
  }

  auto logger = spdlog::default_logger();

  CesiumCurl::CurlAssetAccessorOptions curlOpts;
  if (!config_.tlsCaBundlePath.empty()) {
    curlOpts.certificateFile = config_.tlsCaBundlePath;
  }
  auto curlAccessor = std::make_shared<CesiumCurl::CurlAssetAccessor>(curlOpts);

  if (!config_.cacheDatabasePath.empty()) {
    auto cache = std::make_shared<CesiumAsync::SqliteCache>(
        logger, config_.cacheDatabasePath,
        static_cast<uint64_t>(std::max<int32_t>(64, config_.sqliteCacheMaxRows)));
    assetAccessor_ = std::make_shared<CesiumAsync::CachingAssetAccessor>(
        logger, curlAccessor, cache);
  } else {
    assetAccessor_ = curlAccessor;
  }

  resourcePreparer_ = std::make_shared<ResourcePreparer>(lifecycle_);

  if (!creditSystem_) {
    creditSystem_ = std::make_shared<CesiumUtility::CreditSystem>();
  }

  buildEllipsoidMesh();
  createTileset();
}

void CesiumEngine::shutdown() {
  destroyTileset();
  creditSystem_.reset();
}

void CesiumEngine::updateConfig(const EngineConfig& config) {
  // Only token / asset id changes force a full tileset rebuild. Everything
  // else is mutated in place via tileset_->getOptions() to avoid the very
  // expensive teardown + re-load from the root.
  const bool needRebuild =
      config.ionAccessToken != config_.ionAccessToken ||
      config.ionAssetId     != config_.ionAssetId;

  // Imagery overlay is its own change set.
  const bool overlayChanged =
      config.ionImageryAssetId != config_.ionImageryAssetId;

  config_ = config;
  if (config_.ionImageryAssetId <= 0) {
    config_.ionImageryAssetId = 1;
  }

  if (needRebuild) {
    destroyTileset();
    if (!config_.ionAccessToken.empty()) {
      createTileset();
    }
    return;
  }

  if (tileset_) {
    applyRuntimeConfig(config_);
  }

  if (overlayChanged) {
    applyImageryOverlay(config_.ionImageryAssetId);
  }
}

void CesiumEngine::createTileset() {
  if (config_.ionAccessToken.empty()) {
    spdlog::warn("createTileset: ion access token is empty, skipping tileset creation");
    return;
  }

  if (!creditSystem_) {
    creditSystem_ = std::make_shared<CesiumUtility::CreditSystem>();
  }

  spdlog::info("createTileset: creating tileset with assetId={}", config_.ionAssetId);

  auto logger = spdlog::default_logger();

  Cesium3DTilesSelection::TilesetExternals externals{
      assetAccessor_, resourcePreparer_, asyncSystem_, creditSystem_,
      logger, nullptr};

  Cesium3DTilesSelection::TilesetOptions opts;
  opts.maximumCachedBytes = static_cast<int64_t>(
      std::max<int64_t>(16LL * 1024LL * 1024LL, config_.maximumCachedBytes));
  opts.maximumSimultaneousTileLoads =
      static_cast<uint32_t>(std::max(0, config_.maximumSimultaneousTileLoads));
  opts.loadingDescendantLimit =
      static_cast<uint32_t>(std::max(1, config_.loadingDescendantLimit));
  opts.maximumScreenSpaceError =
      std::max(1.0, config_.maximumScreenSpaceError);
  opts.preloadAncestors = config_.preloadAncestors;
  opts.preloadSiblings  = config_.preloadSiblings;
  opts.forbidHoles      = config_.forbidHoles;
  opts.contentOptions.enableWaterMask = config_.enableWaterMask;
  opts.enableFogCulling = config_.enableFogCulling;
  opts.enforceCulledScreenSpaceError = config_.enforceCulledScreenSpaceError;
  opts.culledScreenSpaceError = std::max(1.0, config_.culledScreenSpaceError);
  opts.enableLodTransitionPeriod = config_.enableLodTransitionPeriod;
  opts.lodTransitionLength = std::max(0.0, config_.lodTransitionLength);
  // Rate-limit per-frame main-thread tile finalisation so the render thread
  // stays responsive during heavy startup bursts (see EngineConfig docs).
  opts.mainThreadLoadingTimeLimit =
      std::max(0.0, config_.mainThreadLoadingTimeLimitMs);

  tileset_ = std::make_unique<Cesium3DTilesSelection::Tileset>(
      externals, config_.ionAssetId, config_.ionAccessToken, opts);

  spdlog::info("createTileset: tileset created successfully");

  currentOverlay_ = nullptr;
  currentImageryAssetId_ = 1;
  if (config_.ionImageryAssetId > 0 && config_.ionImageryAssetId != 1) {
    applyImageryOverlay(config_.ionImageryAssetId);
  }
}

bool CesiumEngine::applyRuntimeConfig(const EngineConfig& cfg) {
  if (!tileset_) return false;
  auto& opts = tileset_->getOptions();
  bool changed = false;

  const double newSse =
      std::max(1.0, cfg.maximumScreenSpaceError);
  if (opts.maximumScreenSpaceError != newSse) {
    opts.maximumScreenSpaceError = newSse;
    changed = true;
  }
  const uint32_t newSim =
      static_cast<uint32_t>(std::max(0, cfg.maximumSimultaneousTileLoads));
  if (opts.maximumSimultaneousTileLoads != newSim) {
    opts.maximumSimultaneousTileLoads = newSim;
    changed = true;
  }
  const uint32_t newDesc =
      static_cast<uint32_t>(std::max(1, cfg.loadingDescendantLimit));
  if (opts.loadingDescendantLimit != newDesc) {
    opts.loadingDescendantLimit = newDesc;
    changed = true;
  }
  const int64_t newCacheBytes = std::max<int64_t>(
      16LL * 1024LL * 1024LL, cfg.maximumCachedBytes);
  if (opts.maximumCachedBytes != newCacheBytes) {
    opts.maximumCachedBytes = newCacheBytes;
    changed = true;
  }
  if (opts.preloadAncestors != cfg.preloadAncestors) {
    opts.preloadAncestors = cfg.preloadAncestors;
    changed = true;
  }
  if (opts.preloadSiblings != cfg.preloadSiblings) {
    opts.preloadSiblings = cfg.preloadSiblings;
    changed = true;
  }
  if (opts.forbidHoles != cfg.forbidHoles) {
    opts.forbidHoles = cfg.forbidHoles;
    changed = true;
  }
  // Note: contentOptions.enableWaterMask only takes effect on tiles loaded
  // after the change. Existing in-memory tiles keep their water mask state.
  if (opts.contentOptions.enableWaterMask != cfg.enableWaterMask) {
    opts.contentOptions.enableWaterMask = cfg.enableWaterMask;
    changed = true;
  }
  if (opts.enableFogCulling != cfg.enableFogCulling) {
    opts.enableFogCulling = cfg.enableFogCulling;
    changed = true;
  }
  if (opts.enforceCulledScreenSpaceError != cfg.enforceCulledScreenSpaceError) {
    opts.enforceCulledScreenSpaceError = cfg.enforceCulledScreenSpaceError;
    changed = true;
  }
  const double newCulled =
      std::max(1.0, cfg.culledScreenSpaceError);
  if (opts.culledScreenSpaceError != newCulled) {
    opts.culledScreenSpaceError = newCulled;
    changed = true;
  }
  if (opts.enableLodTransitionPeriod != cfg.enableLodTransitionPeriod) {
    opts.enableLodTransitionPeriod = cfg.enableLodTransitionPeriod;
    changed = true;
  }
  const double newLodLen = std::max(0.0, cfg.lodTransitionLength);
  if (opts.lodTransitionLength != newLodLen) {
    opts.lodTransitionLength = newLodLen;
    changed = true;
  }
  const double newMainLimit = std::max(0.0, cfg.mainThreadLoadingTimeLimitMs);
  if (opts.mainThreadLoadingTimeLimit != newMainLimit) {
    opts.mainThreadLoadingTimeLimit = newMainLimit;
    changed = true;
  }
  return changed;
}

void CesiumEngine::applyImageryOverlay(int64_t targetAssetId) {
  if (!tileset_) return;
  if (targetAssetId == currentImageryAssetId_) return;

  if (currentOverlay_) {
    tileset_->getOverlays().remove(currentOverlay_);
    currentOverlay_ = nullptr;
  }
  currentImageryAssetId_ = (targetAssetId <= 0) ? 1 : targetAssetId;

  if (currentImageryAssetId_ != 1 && !config_.ionAccessToken.empty()) {
    CesiumUtility::IntrusivePointer<CesiumRasterOverlays::RasterOverlay> ov =
        new CesiumRasterOverlays::IonRasterOverlay(
            "imagery", currentImageryAssetId_, config_.ionAccessToken);
    tileset_->getOverlays().add(ov);
    currentOverlay_ = ov;
  }

  // an overlay swap mutates res->overlayTexture pointers on already-loaded
  // tiles. The fast path re-reads the texture from `res` each frame so output
  // pixels stay correct, but burn one stale frame to be safe: drop the cache
  // and force the engine to take the slow path again until N stable frames
  // confirm the new state.
  lastDrawSignature_   = 0;
  stableSigFrames_     = 0;
  cachedDraws_.clear();
  // floor cache is keyed on signature — drop it too so the next frame
  // re-scans before establishing a new entry.
  floorCacheValid_     = false;
}

void CesiumEngine::destroyTileset() {
  currentOverlay_ = nullptr;
  currentImageryAssetId_ = 1;
  tileset_.reset();
  if (taskProcessor_) {
    taskProcessor_->waitUntilIdle();
  }
  // every TileGPUResources* baked into cachedDraws_ has just been freed.
  // Clearing the cache guarantees no dangling pointer survives into the next
  // updateFrame and forces a clean slow-path rebuild.
  lastDrawSignature_   = 0;
  stableSigFrames_     = 0;
  cachedDraws_.clear();
  // P5: the cached value was keyed on TileGPUResources we just freed.
  floorCacheValid_     = false;
}

void CesiumEngine::setImageryAssetId(int64_t assetId) {
  if (config_.ionAccessToken.empty()) return;
  config_.ionImageryAssetId = (assetId <= 0) ? 1 : assetId;
  applyImageryOverlay(config_.ionImageryAssetId);
}

namespace {
// FNV-1a 64-bit hash mix. Used to compute the geometry signature from the
// ordered list of (TileGPUResources*, primitive*, primitive.indices.size())
// triples for tilesToRenderThisFrame. Two frames whose visible primitives
// hash to the same value are guaranteed (modulo astronomically unlikely
// 64-bit collision) to produce identical merged vertex / index / UV buffers.
inline uint64_t fnv1a64Mix(uint64_t h, uint64_t v) {
  h ^= v;
  h *= 0x100000001b3ULL;
  return h;
}
} // namespace

void CesiumEngine::updateFrame(double w, double h, FrameResult& result) {
  result.localPositions.clear();
  result.altitudes.clear();
  result.uvs.clear();
  result.indices.clear();
  result.draws.clear();
  result.creditHtmlLines.clear();
  result.geometrySignature = 0;

  result.ionTokenConfigured = !config_.ionAccessToken.empty();
  result.tilesetActive      = (tileset_ != nullptr);
  result.verticalFovDeg     = camera_.getVerticalFovDegrees();

  const glm::dvec3 cameraPos = camera_.getECEFPosition();
  result.cameraEcef = glm::vec3(cameraPos);

  // Rotation-only VP (float) — used by the sky shader and the ellipsoid fallback.
  result.vpMatrix = camera_.computeVPMatrix(w, h);
  result.invVP    = glm::inverse(result.vpMatrix);

  // Double-precision rotation-only VP — used to build per-tile MVP matrices
  // so the camera↔tile-centre translation is resolved in double before the
  // final cast to float32.
  const glm::dmat4 vpDouble = camera_.computeVPMatrixDouble(w, h);

  if (!tileset_) {
    appendEllipsoidDraws(result);
    lastDrawSignature_ = 0;
    stableSigFrames_   = 0;
    cachedDraws_.clear();
    lifecycle_.advanceFrame();
    return;
  }

  result.localPositions.reserve(tunables::kFrameResultPositionFloatReserve);
  result.altitudes.reserve(tunables::kFrameResultPositionFloatReserve / 3);
  result.uvs.reserve(tunables::kFrameResultPositionFloatReserve * 2);
  result.indices.reserve(tunables::kFrameResultIndexUint32Reserve);

  asyncSystem_.dispatchMainThreadTasks();

  // Wall-clock deltaTime since previous frame. Cesium Native requires this to
  // advance LOD transition fade percentages (without it, fade stays at 0 and
  // newly-selected tiles never become visible). Clamped to a max of 100ms so a
  // long pause / app foreground transition doesn't instantly complete fades.
  const auto now = std::chrono::steady_clock::now();
  float deltaTime = 0.0f;
  if (hasLastFrameTime_) {
    const auto diff = std::chrono::duration<float>(now - lastFrameTime_).count();
    deltaTime = std::min(diff, 0.1f);
  }
  lastFrameTime_    = now;
  hasLastFrameTime_ = true;

  const auto viewState     = camera_.computeViewState(w, h);
  const auto& updateResult =
      tileset_->updateViewGroup(tileset_->getDefaultViewGroup(), {viewState}, deltaTime);
  tileset_->loadTiles();

  // Only emit the fallback ellipsoid when no real terrain is being rendered
  // this frame. Otherwise it would be over-drawn anyway (~8k vertices and
  // ~24k indices of throwaway transform + memcpy per frame).
  const bool noVisibleTerrain = updateResult.tilesToRenderThisFrame.empty();
  if (noVisibleTerrain) {
    appendEllipsoidDraws(result);
    // Ellipsoid fallback emits its own geometry; any cached terrain state is
    // stale until real tiles arrive. Reset so fast path can't fire on the
    // very next frame after a tile re-appears.
    lastDrawSignature_ = 0;
    stableSigFrames_   = 0;
    cachedDraws_.clear();
  }

  if (creditSystem_) {
    const CesiumUtility::CreditsSnapshot& snap =
        creditSystem_->getSnapshot(CesiumUtility::CreditFilteringMode::UniqueHtml);
    for (const auto& c : snap.currentCredits) {
      const std::string& html = creditSystem_->getHtml(c);
      if (html.empty()) continue;
      if (html.find("Error: Invalid Credit") != std::string::npos) continue;
      result.creditHtmlLines.push_back(html);
    }
  }

  // ── geometry signature ────────────────────────────────────────────
  // Fingerprint the ordered visible primitive list. When the same fingerprint
  // shows up for kMaxFramesInFlight consecutive frames *and* there are no
  // LOD-fade draws, every backend slot's persistent buffer already holds the
  // right bytes — the fast path can then skip the entire merge loop and the
  // memcpys it produces.
  uint64_t signature = 0xcbf29ce484222325ULL; // FNV-1a 64-bit offset basis
  if (!noVisibleTerrain) {
    for (const auto& tile : updateResult.tilesToRenderThisFrame) {
      if (!tile) continue;
      const auto* rc = tile->getContent().getRenderContent();
      if (!rc) continue;
      const auto* res = static_cast<const TileGPUResources*>(rc->getRenderResources());
      if (!res) continue;
      signature = fnv1a64Mix(signature, reinterpret_cast<uint64_t>(res));
      signature = fnv1a64Mix(signature,
                              static_cast<uint64_t>(res->primitives.size()));
      for (const auto& prim : res->primitives) {
        signature = fnv1a64Mix(signature, reinterpret_cast<uint64_t>(&prim));
        signature = fnv1a64Mix(signature,
                                static_cast<uint64_t>(prim.indices.size()));
      }
    }
    // Reserve 0 as the "force upload" sentinel; we should never emit 0 as a
    // valid signature.
    if (signature == 0ULL) signature = 1ULL;
  } else {
    signature = 0ULL;
  }

  // ── scan tilesFadingOut for any in-flight fades ──────────────────────
  // Fast path is illegal whenever a single fading-out draw would be emitted
  // (lodFade < 1 means the per-tile dither cutoff changes every frame and the
  // emitted set of draws may grow/shrink mid-transition).
  bool anyFadeDraws = false;
  for (const auto& tile : updateResult.tilesFadingOut) {
    if (!tile) continue;
    const auto* rc = tile->getContent().getRenderContent();
    if (!rc) continue;
    if (1.0f - rc->getLodTransitionFadePercentage() > 0.0f) {
      anyFadeDraws = true;
      break;
    }
  }

  // ── update stability counter ─────────────────────────────────────────
  // IMPORTANT: reset to 0, NOT 1. Setting it to 1 was an off-by-one that
  // let the fast path fire while one slot still held the previous signature:
  //   fi=A (reset to 1): slow path → geomSlotSig_[A]=sig
  //   fi=B (counter=2) : slow path → geomSlotSig_[B]=sig
  //   fi=C (counter=3≥3): fast path → geomSlotSig_[C] STILL stale → blank!
  // With reset=0 the fast path is deferred until ALL kMaxFramesInFlight
  // slots have been written with the current signature.
  if (!noVisibleTerrain && signature != 0ULL) {
    if (signature == lastDrawSignature_) {
      ++stableSigFrames_;
    } else {
      stableSigFrames_   = 0;
      lastDrawSignature_ = signature;
      cachedDraws_.clear();
    }
  }

  // NOTE: must be STRICTLY GREATER (>) than kMaxFramesInFlight, not >=.
  // With >= kMaxFramesInFlight the fast path fires on the Nth stable frame,
  // which is exactly the frame where the Nth backend slot *would* have been
  // written in the slow path. At that point the slot still holds the previous
  // (or zero) signature and the backend returns a blank frame.
  // With > kMaxFramesInFlight the Nth slow-path frame writes the last slot,
  // and the fast path activates on frame N+1 when all slots are primed.
  const bool canFastPath = !noVisibleTerrain && !anyFadeDraws &&
                           signature != 0ULL &&
                           stableSigFrames_ > tunables::kMaxFramesInFlight &&
                           !cachedDraws_.empty();

  // ── Terrain floor tracking (for minAltitudeAboveTerrain clamp) ───────────
  // Computed once before the tile loop and updated per-tile inside it.
  // Only active when the feature is enabled to avoid any overhead otherwise.
  const bool trackTerrainFloor = (config_.minAltitudeAboveTerrain > 0.0f);
  const CameraParams camParams  = trackTerrainFloor ? camera_.getParams() : CameraParams{};
  const double camLatRad = glm::radians(camParams.latitude);
  const double camLonRad = glm::radians(camParams.longitude);

  // Expected ground ellipsoid height at camera location (from geoid).
  // Used to filter out elevated structures (buildings) during floor sampling.
  const double expectedGroundEllipsoid = trackTerrainFloor
      ? mslToEllipsoidMeters(camParams.latitude, camParams.longitude, 0.0)
      : 0.0;

  float  bestTerrainFloor = terrainFloorEllipsoidM_; // carry previous value if no tile matches
  double bestHorizDistSq  = std::numeric_limits<double>::max();
  bool   floorScanFound   = false;

  // Horizontal search radius for floor sampling.
  // Increased to 200m to ensure robust sampling across terrain LODs and camera speeds.
  constexpr double kFloorSearchRadiusM  = 200.0;
  constexpr double kFloorSearchRadiusSq = kFloorSearchRadiusM * kFloorSearchRadiusM;

  // ── floor-scan cache lookup ─────────────────────────────────────────
  // Skip the per-vertex scan when the camera has moved less than ~10 cm in
  // both lat and lon AND the visible primitive set is unchanged (same
  // geometry signature). The cached value is only trusted for non-zero
  // signatures — sig==0 means a fade or rebuild is in flight, in which case
  // we always re-scan and refresh the cache afterwards.
  bool floorScanFromCache = false;
  if (trackTerrainFloor && floorCacheValid_ && signature != 0ULL &&
      signature == floorCacheSignature_) {
    const double dLon = std::abs(camParams.longitude - floorCacheCamLonDeg_);
    const double dLat = std::abs(camParams.latitude  - floorCacheCamLatDeg_);
    if (dLon < 1e-6 && dLat < 1e-6) {
      bestTerrainFloor   = floorCacheValue_;
      floorScanFromCache = true;
    }
  }
  // When tracking is off, suppress the per-tile scan branches (legacy
  // behaviour already gated on trackTerrainFloor; this just keeps the cache
  // semantics aligned).
  const bool runFloorScan = trackTerrainFloor && !floorScanFromCache;

  // ENU basis at the camera — horizontal distance to each terrain vertex.
  glm::dvec3 floorEast(1.0, 0.0, 0.0);
  glm::dvec3 floorNorth(0.0, 1.0, 0.0);
  glm::dvec3 floorScanPos = cameraPos; // Always scan directly below camera
  glm::dvec3 floorScanAheadPos = cameraPos; // Also scan ahead if moving
  bool useLookAhead = false;
  
  if (runFloorScan) {
    const auto& ellipsoid = CesiumGeospatial::Ellipsoid::WGS84;
    const glm::dmat4 enuToEcef =
        CesiumGeospatial::GlobeTransforms::eastNorthUpToFixedFrame(
            cameraPos, ellipsoid);
    floorEast  = glm::normalize(glm::dvec3(enuToEcef[0]));
    floorNorth = glm::normalize(glm::dvec3(enuToEcef[1]));
    
    // ── Look-ahead terrain sampling ──────────────────────────────────────
    // Sample terrain BOTH below camera AND ahead. Use the maximum height to
    // prevent clipping through terrain between current position and look-ahead.
    if (floorCacheValid_) {
      // Convert previous lat/lon to ECEF to get movement vector
      const double prevLonRad = floorCacheCamLonDeg_ * M_PI / 180.0;
      const double prevLatRad = floorCacheCamLatDeg_ * M_PI / 180.0;
      const glm::dvec3 prevCamPos = ellipsoid.cartographicToCartesian(
          CesiumGeospatial::Cartographic(prevLonRad, prevLatRad, camParams.altitude));
      
      // Calculate horizontal movement since last frame
      const glm::dvec3 movement3D = cameraPos - prevCamPos;
      const double moveEast = glm::dot(movement3D, floorEast);
      const double moveNorth = glm::dot(movement3D, floorNorth);
      const double horizMovement = std::sqrt(moveEast * moveEast + moveNorth * moveNorth);
      
      // Look ahead by 5x recent movement (minimum 10m, max 100m)
      constexpr double kMinLookAhead = 10.0;   // meters
      constexpr double kMaxLookAhead = 100.0;  // meters
      constexpr double kLookAheadMultiplier = 5.0;
      
      double lookAheadDist = std::clamp(
          horizMovement * kLookAheadMultiplier,
          kMinLookAhead,
          kMaxLookAhead);
      
      if (horizMovement > 0.1) { // Only look ahead if actually moving
        // Project sampling point forward in direction of movement
        const glm::dvec3 moveDir = glm::normalize(
            floorEast * moveEast + floorNorth * moveNorth);
        floorScanAheadPos = cameraPos + moveDir * lookAheadDist;
        useLookAhead = true;
      }
    }
  }

  // Convert floor scan positions to lat/lon for tile bounds checking
  // Camera position (always used)
  double floorScanLonRad = camLonRad;
  double floorScanLatRad = camLatRad;
  
  // Look-ahead position (used when moving)
  double floorScanAheadLonRad = camLonRad;
  double floorScanAheadLatRad = camLatRad;
  if (runFloorScan && useLookAhead) {
    const auto& ellipsoid = CesiumGeospatial::Ellipsoid::WGS84;
    const auto scanCartographic = ellipsoid.cartesianToCartographic(floorScanAheadPos);
    if (scanCartographic) {
      floorScanAheadLonRad = scanCartographic->longitude;
      floorScanAheadLatRad = scanCartographic->latitude;
    }
  }

  // ── fast path ────────────────────────────────────────────────────────
  // The visible primitive list has been identical for kMaxFramesInFlight
  // frames in a row, so every backend slot is guaranteed to hold a copy of
  // the merged buffers. Skip the merge entirely: re-emit one DrawPrimitive
  // per cached entry with a fresh per-tile MVP and the latest overlay /
  // water-mask state read straight from TileGPUResources. The leftover
  // result.localPositions / altitudes / uvs / indices stay empty — backends
  // detect this via result.geometrySignature and use their cached slot data.
  if (canFastPath) {
    result.draws.reserve(cachedDraws_.size());
    const TileGPUResources* lastScanRes = nullptr;
    for (const auto& c : cachedDraws_) {
      lifecycle_.stampTileUsed(const_cast<TileGPUResources*>(c.res));

      // Floor scan (run once per res so multi-primitive tiles aren't scanned
      // twice). Skipped when the P5 cache hit above produced a usable value.
      // Sample terrain at BOTH camera position and look-ahead position.
      // Find lowest terrain at each position, then use the MAXIMUM of the two.
      if (runFloorScan && c.res != lastScanRes) {
        lastScanRes = c.res;
        
        // Check if camera position is in tile bounds
        const bool camInside =
            (c.wmTileBounds.x <= static_cast<float>(floorScanLonRad) &&
             static_cast<float>(floorScanLonRad) <= c.wmTileBounds.z) &&
            (c.wmTileBounds.y <= static_cast<float>(floorScanLatRad) &&
             static_cast<float>(floorScanLatRad) <= c.wmTileBounds.w);
        
        // Check if look-ahead position is in tile bounds (when moving)
        const bool aheadInside = useLookAhead && 
            (c.wmTileBounds.x <= static_cast<float>(floorScanAheadLonRad) &&
             static_cast<float>(floorScanAheadLonRad) <= c.wmTileBounds.z) &&
            (c.wmTileBounds.y <= static_cast<float>(floorScanAheadLatRad) &&
             static_cast<float>(floorScanAheadLatRad) <= c.wmTileBounds.w);
        
        if (camInside || aheadInside) {
          // Track lowest terrain at each position separately
          float terrainAtCam = std::numeric_limits<float>::max();
          float terrainAhead = std::numeric_limits<float>::max();
          bool foundAtCam = false;
          bool foundAhead = false;
          
          for (const auto& prim : c.res->primitives) {
            if (prim.altitudes.empty() || prim.localPositions.empty()) continue;
            const size_t vc = prim.localPositions.size();
            
            for (size_t vi = 0; vi < vc; ++vi) {
              const glm::dvec3 vECEF = glm::dvec3(prim.localPositions[vi]) + prim.rtcCenter;
              const float vAlt = prim.altitudes[vi];
              
              // Sample terrain at camera position - find LOWEST
              if (camInside) {
                const glm::dvec3 deltaCam = vECEF - floorScanPos;
                const double deCam = glm::dot(deltaCam, floorEast);
                const double dnCam = glm::dot(deltaCam, floorNorth);
                const double horizSqCam = deCam * deCam + dnCam * dnCam;
                
                if (horizSqCam <= kFloorSearchRadiusSq) {
                  if (!foundAtCam || vAlt < terrainAtCam) {
                    terrainAtCam = vAlt;
                    foundAtCam = true;
                  }
                }
              }
              
              // Sample terrain at look-ahead position - find LOWEST
              if (aheadInside) {
                const glm::dvec3 deltaAhead = vECEF - floorScanAheadPos;
                const double deAhead = glm::dot(deltaAhead, floorEast);
                const double dnAhead = glm::dot(deltaAhead, floorNorth);
                const double horizSqAhead = deAhead * deAhead + dnAhead * dnAhead;
                
                if (horizSqAhead <= kFloorSearchRadiusSq) {
                  if (!foundAhead || vAlt < terrainAhead) {
                    terrainAhead = vAlt;
                    foundAhead = true;
                  }
                }
              }
            }
          }
          
          // Use the MAXIMUM of the two terrain samples (prevents clipping through either)
          if (foundAtCam && foundAhead) {
            bestTerrainFloor = std::max(terrainAtCam, terrainAhead);
            floorScanFound = true;
          } else if (foundAtCam) {
            bestTerrainFloor = terrainAtCam;
            floorScanFound = true;
          } else if (foundAhead) {
            bestTerrainFloor = terrainAhead;
            floorScanFound = true;
          }
        }
      }

      const glm::dvec3 rtcCamRel = c.rtcCenter - glm::dvec3(cameraPos);
      const glm::dmat4 translateD = glm::translate(glm::dmat4(1.0), rtcCamRel);

      DrawPrimitive draw;
      draw.indexByteOffset    = c.indexByteOffset;
      draw.indexCount         = c.indexCount;
      draw.hasUVs             = c.hasUVs;
      draw.overlayTexture     = c.res->overlayTexture;
      draw.overlayTranslation = c.res->overlayTranslation;
      draw.overlayScale       = c.res->overlayScale;
      draw.waterMaskTexture   = c.res->waterMaskTexture;
      draw.isOnlyWater        = c.res->isOnlyWater;
      draw.wmTileBounds       = c.wmTileBounds;
      draw.wmTranslation      = c.res->wmTranslation;
      draw.wmScale            = c.res->wmScale;
      draw.mvpMatrix          = glm::mat4(vpDouble * translateD);
      draw.rtcCenterEcef      = glm::vec3(c.rtcCenter);
      draw.lodFade            = 1.0f;
      result.draws.push_back(draw);
    }

    result.geometrySignature = signature;

    result.tilesRendered = static_cast<int>(updateResult.tilesToRenderThisFrame.size());
    result.tilesLoading  = updateResult.workerThreadTileLoadQueueLength +
                           updateResult.mainThreadTileLoadQueueLength;
    result.tilesVisited  = static_cast<int>(updateResult.tilesVisited);

    const auto paramsFP = camera_.getParams();
    result.cameraLat = paramsFP.latitude;
    result.cameraLon = paramsFP.longitude;
    result.cameraAlt = paramsFP.altitude;

    if (trackTerrainFloor && (floorScanFound || floorScanFromCache)) {
      terrainFloorEllipsoidM_ = bestTerrainFloor;
      terrainFloorValid_      = true;
      // P5: refresh the floor cache on a fresh scan; skip when we hit the
      // cache (the stored value already reflects this signature).
      if (runFloorScan && signature != 0ULL) {
        floorCacheValid_     = true;
        floorCacheSignature_ = signature;
        floorCacheCamLonDeg_ = camParams.longitude;
        floorCacheCamLatDeg_ = camParams.latitude;
        floorCacheValue_     = bestTerrainFloor;
      }
    }

    lifecycle_.advanceFrame();
    return;
  }

  // ── Slow path: rebuild cachedDraws_ inline ───────────────────────────────
  // Wipe so the loop below can append. After the loop, if there were no
  // fade-out draws we keep it for the fast path; otherwise we clear and
  // reset the stability counter (the cache is only valid for stable
  // no-fade frames).
  cachedDraws_.clear();
  if (!noVisibleTerrain) cachedDraws_.reserve(updateResult.tilesToRenderThisFrame.size());

  for (const auto& tile : updateResult.tilesToRenderThisFrame) {
    if (!tile) continue;

    const auto* renderContent = tile->getContent().getRenderContent();
    if (!renderContent) continue;

    const auto* res = static_cast<const TileGPUResources*>(
        renderContent->getRenderResources());
    if (!res) continue;

    lifecycle_.stampTileUsed(const_cast<TileGPUResources*>(res));

    glm::vec4 wmTileBounds(0.0f);
    const auto& bv = tile->getBoundingVolume();
    const CesiumGeospatial::BoundingRegion* bRegion =
        Cesium3DTilesSelection::getBoundingRegionFromBoundingVolume(bv);
    if (bRegion) {
      const auto& rect = bRegion->getRectangle();
      wmTileBounds = glm::vec4(
          static_cast<float>(rect.getWest()),
          static_cast<float>(rect.getSouth()),
          static_cast<float>(rect.getEast()),
          static_cast<float>(rect.getNorth()));

      // ── Terrain floor: sample at both camera and look-ahead positions ────
      // Find lowest terrain at each position, then use the MAXIMUM of the two.
      // Suppressed when the cache returned a usable value.
      const bool camInside = runFloorScan && 
          rect.contains(CesiumGeospatial::Cartographic(floorScanLonRad, floorScanLatRad, 0.0));
      const bool aheadInside = runFloorScan && useLookAhead &&
          rect.contains(CesiumGeospatial::Cartographic(floorScanAheadLonRad, floorScanAheadLatRad, 0.0));
      
      if (camInside || aheadInside) {
        // Track lowest terrain at each position separately
        float terrainAtCam = std::numeric_limits<float>::max();
        float terrainAhead = std::numeric_limits<float>::max();
        bool foundAtCam = false;
        bool foundAhead = false;
        
        for (const auto& prim : res->primitives) {
          if (prim.altitudes.empty() || prim.localPositions.empty()) continue;
          const size_t vertexCount = prim.localPositions.size();
          
          for (size_t vi = 0; vi < vertexCount; ++vi) {
            const glm::dvec3 vECEF = glm::dvec3(prim.localPositions[vi]) + prim.rtcCenter;
            const float vAlt = prim.altitudes[vi];
            
            // Sample terrain at camera position - find LOWEST
            if (camInside) {
              const glm::dvec3 deltaCam = vECEF - floorScanPos;
              const double deCam = glm::dot(deltaCam, floorEast);
              const double dnCam = glm::dot(deltaCam, floorNorth);
              const double horizSqCam = deCam * deCam + dnCam * dnCam;
              
              if (horizSqCam <= kFloorSearchRadiusSq) {
                if (!foundAtCam || vAlt < terrainAtCam) {
                  terrainAtCam = vAlt;
                  foundAtCam = true;
                }
              }
            }
            
            // Sample terrain at look-ahead position - find LOWEST
            if (aheadInside) {
              const glm::dvec3 deltaAhead = vECEF - floorScanAheadPos;
              const double deAhead = glm::dot(deltaAhead, floorEast);
              const double dnAhead = glm::dot(deltaAhead, floorNorth);
              const double horizSqAhead = deAhead * deAhead + dnAhead * dnAhead;
              
              if (horizSqAhead <= kFloorSearchRadiusSq) {
                if (!foundAhead || vAlt < terrainAhead) {
                  terrainAhead = vAlt;
                  foundAhead = true;
                }
              }
            }
          }
        }
        
        // Use the MAXIMUM of the two terrain samples (prevents clipping through either)
        if (foundAtCam && foundAhead) {
          bestTerrainFloor = std::max(terrainAtCam, terrainAhead);
          floorScanFound = true;
        } else if (foundAtCam) {
          bestTerrainFloor = terrainAtCam;
          floorScanFound = true;
        } else if (foundAhead) {
          bestTerrainFloor = terrainAhead;
          floorScanFound = true;
        }
      }
    }

    for (const auto& prim : res->primitives) {
      if (prim.indices.empty() || prim.localPositions.empty()) continue;

      const size_t   baseVertex    = result.localPositions.size() / 3;
      const uint32_t indexByteOff  =
          static_cast<uint32_t>(result.indices.size() * sizeof(uint32_t));
      const size_t   vertexCount   = prim.localPositions.size();
      const bool     hasPrimUVs    =
          !prim.uvs.empty() && prim.uvs.size() == vertexCount;

      // ── RTC: bulk-copy tile-local positions, altitudes, UVs ─────────────────
      // Use insert(end(), ptr, ptr+n) rather than resize()+memcpy: libc++
      // dispatches insert for trivially-copyable float ranges to memmove
      // (NEON-accelerated even at -O0), skipping the per-element zero-init
      // loop that resize() performs before we'd overwrite the same bytes
      // with memcpy. This halves the memory writes and eliminates the
      // un-vectorised zero-init bottleneck in Debug builds.
      static_assert(sizeof(glm::vec3) == 3 * sizeof(float), "glm::vec3 must be packed");
      static_assert(sizeof(glm::vec2) == 2 * sizeof(float), "glm::vec2 must be packed");

      {
        const float* posData = reinterpret_cast<const float*>(prim.localPositions.data());
        result.localPositions.insert(result.localPositions.end(),
                                     posData, posData + vertexCount * 3);
      }

      if (!prim.altitudes.empty()) {
        result.altitudes.insert(result.altitudes.end(),
                                prim.altitudes.data(),
                                prim.altitudes.data() + vertexCount);
      } else {
        result.altitudes.resize(result.altitudes.size() + vertexCount, 0.0f);
      }

      if (hasPrimUVs) {
        const float* uvData = reinterpret_cast<const float*>(prim.uvs.data());
        result.uvs.insert(result.uvs.end(), uvData, uvData + vertexCount * 2);
      } else {
        result.uvs.resize(result.uvs.size() + vertexCount * 2, 0.5f);
      }

      // Indices: add base-vertex offset then insert. Use a temporary vector
      // so the transform writes directly into pre-sized storage.
      {
        const size_t   nIdx    = prim.indices.size();
        const size_t   idxBase = result.indices.size();
        result.indices.resize(idxBase + nIdx);
        const uint32_t bv32    = static_cast<uint32_t>(baseVertex);
        const uint32_t* __restrict srcIdx = prim.indices.data();
              uint32_t* __restrict dstIdx = result.indices.data() + idxBase;
        for (size_t ii = 0; ii < nIdx; ++ii) { dstIdx[ii] = srcIdx[ii] + bv32; }
      }

      // ── RTC: per-tile MVP matrix (double → float) ─────────────────────────
      const glm::dvec3 rtcCamRel =
          prim.rtcCenter - glm::dvec3(cameraPos);
      const glm::dmat4 translateD =
          glm::translate(glm::dmat4(1.0), rtcCamRel);
      const glm::mat4 perTileMVP =
          glm::mat4(vpDouble * translateD);

      DrawPrimitive draw;
      draw.indexByteOffset      = indexByteOff;
      draw.indexCount           = static_cast<uint32_t>(prim.indices.size());
      draw.hasUVs               = hasPrimUVs;
      draw.overlayTexture       = res->overlayTexture;
      draw.overlayTranslation   = res->overlayTranslation;
      draw.overlayScale         = res->overlayScale;
      draw.waterMaskTexture     = res->waterMaskTexture;
      draw.isOnlyWater          = res->isOnlyWater;
      draw.wmTileBounds         = wmTileBounds;
      draw.wmTranslation        = res->wmTranslation;
      draw.wmScale              = res->wmScale;
      draw.mvpMatrix            = perTileMVP;
      draw.rtcCenterEcef        = glm::vec3(prim.rtcCenter);
      // ── LOD fade strategy: incoming tiles are SOLID, outgoing tiles dither ──
      // We deliberately ignore Cesium Native's "fade-in" percentage on the
      // tilesToRenderThisFrame list and always render newly-selected tiles at
      // full opacity.  The reason is visibility correctness: if the incoming
      // child were also dithering, both child (fading in) and parent (fading
      // out, see loop below) would simultaneously have holes — and behind the
      // holes is the sky.  By rendering the incoming tile fully solid we
      // guarantee a complete opaque "floor" for every screen pixel covered by
      // terrain, and the outgoing tile dithers OUT on top of it.  The original
      // ring artefact is still masked because the outgoing tile is the
      // coarser/older surface fading away (its disappearance is what used to
      // be visible as a hard ring).
      draw.lodFade              = 1.0f;
      result.draws.push_back(draw);

      // capture the per-primitive bits the fast path needs to re-emit
      // this draw next frame (camera-dependent MVP is recomputed; everything
      // else is stable as long as the signature is unchanged).
      CachedDraw cd;
      cd.res             = res;
      cd.rtcCenter       = prim.rtcCenter;
      cd.indexByteOffset = indexByteOff;
      cd.indexCount      = static_cast<uint32_t>(prim.indices.size());
      cd.hasUVs          = hasPrimUVs;
      cd.wmTileBounds    = wmTileBounds;
      cachedDraws_.push_back(cd);
    }
  }

  // ── Fading-out tiles (LOD transition) ──────────────────────────────────────
  // The outgoing (de-selected) tiles dither out on top of the already-solid
  // incoming tiles drawn in the loop above.  Because the layer behind every
  // dithered pixel is the new solid tile, the dither holes never reveal the
  // sky — eliminating the bleed-through artefacts visible with the previous
  // "both layers fade simultaneously" approach.
  //
  // Cesium Native fade semantics (Tileset.cpp _updateLodTransitions):
  //   "We always fade tiles from 0.0 --> 1.0. Whether the tile is fading in or
  //    out is determined by whether the tile is in the tilesToRenderThisFrame
  //    or tilesFadingOut list."
  //
  // For tilesFadingOut, raw fade goes 0→1 but means "just stopped being
  // selected → fully gone".  Our shader treats lodFade as visibility
  // (1=visible, 0=hide), so we invert: visibility = 1 - rawFade.
  // Terrain-floor tracking is skipped (these tiles are being replaced).
  for (const auto& tile : updateResult.tilesFadingOut) {
    if (!tile) continue;
    const auto* renderContent = tile->getContent().getRenderContent();
    if (!renderContent) continue;
    const float fade = 1.0f - renderContent->getLodTransitionFadePercentage();
    if (fade <= 0.0f) continue;  // fully faded out — "hide right away" per Cesium Native docs

    const auto* res = static_cast<const TileGPUResources*>(
        renderContent->getRenderResources());
    if (!res) continue;
    lifecycle_.stampTileUsed(const_cast<TileGPUResources*>(res));

    glm::vec4 wmTileBounds(0.0f);
    const auto& bv = tile->getBoundingVolume();
    const CesiumGeospatial::BoundingRegion* bRegion =
        Cesium3DTilesSelection::getBoundingRegionFromBoundingVolume(bv);
    if (bRegion) {
      const auto& rect = bRegion->getRectangle();
      wmTileBounds = glm::vec4(
          static_cast<float>(rect.getWest()),
          static_cast<float>(rect.getSouth()),
          static_cast<float>(rect.getEast()),
          static_cast<float>(rect.getNorth()));
    }

    for (const auto& prim : res->primitives) {
      if (prim.indices.empty() || prim.localPositions.empty()) continue;

      const size_t   baseVertex   = result.localPositions.size() / 3;
      const uint32_t indexByteOff =
          static_cast<uint32_t>(result.indices.size() * sizeof(uint32_t));
      const size_t   vertexCount  = prim.localPositions.size();
      const bool     hasPrimUVs   =
          !prim.uvs.empty() && prim.uvs.size() == vertexCount;

      {
        const float* posData = reinterpret_cast<const float*>(prim.localPositions.data());
        result.localPositions.insert(result.localPositions.end(),
                                     posData, posData + vertexCount * 3);
      }

      if (!prim.altitudes.empty()) {
        result.altitudes.insert(result.altitudes.end(),
                                prim.altitudes.data(),
                                prim.altitudes.data() + vertexCount);
      } else {
        result.altitudes.resize(result.altitudes.size() + vertexCount, 0.0f);
      }

      if (hasPrimUVs) {
        const float* uvData = reinterpret_cast<const float*>(prim.uvs.data());
        result.uvs.insert(result.uvs.end(), uvData, uvData + vertexCount * 2);
      } else {
        result.uvs.resize(result.uvs.size() + vertexCount * 2, 0.5f);
      }

      {
        const size_t   nIdx    = prim.indices.size();
        const size_t   idxBase = result.indices.size();
        result.indices.resize(idxBase + nIdx);
        const uint32_t bv32    = static_cast<uint32_t>(baseVertex);
        const uint32_t* __restrict srcIdx = prim.indices.data();
              uint32_t* __restrict dstIdx = result.indices.data() + idxBase;
        for (size_t ii = 0; ii < nIdx; ++ii) { dstIdx[ii] = srcIdx[ii] + bv32; }
      }

      const glm::dvec3 rtcCamRel =
          prim.rtcCenter - glm::dvec3(cameraPos);
      const glm::dmat4 translateD =
          glm::translate(glm::dmat4(1.0), rtcCamRel);
      const glm::mat4 perTileMVP =
          glm::mat4(vpDouble * translateD);

      DrawPrimitive draw;
      draw.indexByteOffset      = indexByteOff;
      draw.indexCount           = static_cast<uint32_t>(prim.indices.size());
      draw.hasUVs               = hasPrimUVs;
      draw.overlayTexture       = res->overlayTexture;
      draw.overlayTranslation   = res->overlayTranslation;
      draw.overlayScale         = res->overlayScale;
      draw.waterMaskTexture     = res->waterMaskTexture;
      draw.isOnlyWater          = res->isOnlyWater;
      draw.wmTileBounds         = wmTileBounds;
      draw.wmTranslation        = res->wmTranslation;
      draw.wmScale              = res->wmScale;
      draw.mvpMatrix            = perTileMVP;
      draw.rtcCenterEcef        = glm::vec3(prim.rtcCenter);
      draw.lodFade              = fade;
      result.draws.push_back(draw);
    }
  }

  // ── Partition draws so all solid (lodFade==1) tiles render first ────────────
  // Backends use this to bind the solid pipeline first (early-Z enabled on
  // TBDR / Adreno / Mali) and switch to the dither pipeline at most once per
  // frame when the first fading tile is reached. Stable to keep tile order
  // deterministic so identical scenes produce identical command buffers.
  std::stable_partition(result.draws.begin(), result.draws.end(),
      [](const DrawPrimitive& d) { return d.lodFade >= 1.0f; });

  // ── finalise cache state for next frame ──────────────────────────────
  // Backends consume `geometrySignature` to short-circuit the host→GPU
  // memcpy when their per-slot cached signature matches. We emit a non-zero
  // signature only when this frame is byte-for-byte reproducible from the
  // cached state — i.e. terrain is present and no LOD fades are in flight.
  // Any fading-out draw forces a full upload because the dither cutoff
  // changes every frame.
  if (anyFadeDraws || noVisibleTerrain) {
    result.geometrySignature = 0ULL;
    cachedDraws_.clear();
    stableSigFrames_ = 0;
  } else {
    result.geometrySignature = signature;
  }

  result.tilesRendered = static_cast<int>(updateResult.tilesToRenderThisFrame.size());
  result.tilesLoading  = updateResult.workerThreadTileLoadQueueLength +
                         updateResult.mainThreadTileLoadQueueLength;
  result.tilesVisited  = static_cast<int>(updateResult.tilesVisited);

  const auto params    = camera_.getParams();
  result.cameraLat     = params.latitude;
  result.cameraLon     = params.longitude;
  result.cameraAlt     = params.altitude;

  if (trackTerrainFloor && (floorScanFound || floorScanFromCache)) {
    terrainFloorEllipsoidM_ = bestTerrainFloor;
    terrainFloorValid_      = true;
    // refresh the floor cache on a fresh scan (slow path always runs a
    // scan unless the cache short-circuit fired above). Only memoise when
    // the geometry signature is stable enough to key on.
    if (runFloorScan && signature != 0ULL) {
      floorCacheValid_     = true;
      floorCacheSignature_ = signature;
      floorCacheCamLonDeg_ = camParams.longitude;
      floorCacheCamLatDeg_ = camParams.latitude;
      floorCacheValue_     = bestTerrainFloor;
    } else if (!runFloorScan) {
      // Cache hit on the slow path is unusual but possible (e.g. fade
      // started this frame after a stable cache was warm). Nothing to do
      // — terrainFloorEllipsoidM_ is already the cached value.
    } else {
      // Cannot key on signature=0; invalidate so the next stable frame
      // re-populates the cache deterministically.
      floorCacheValid_ = false;
    }
  }

  lifecycle_.advanceFrame();
}

} // namespace reactnativecesium
