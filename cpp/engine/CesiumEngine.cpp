#ifndef NDEBUG
# define NDEBUG
# define CESIUM_ENGINE_UNDEF_NDEBUG
#endif

#include "CesiumEngine.hpp"

#ifdef __ANDROID__
#include <android/log.h>
#define ENGINE_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "CesiumEngine", __VA_ARGS__)
#else
#define ENGINE_LOGI(...)
#endif

#include <Cesium3DTilesContent/registerAllTileContentTypes.h>
#include <Cesium3DTilesSelection/BoundingVolume.h>
#include <Cesium3DTilesSelection/Tile.h>
#include <Cesium3DTilesSelection/Tileset.h>
#include <Cesium3DTilesSelection/TilesetExternals.h>
#include <Cesium3DTilesSelection/TilesetOptions.h>
#include <CesiumGeospatial/BoundingRegion.h>
#include <CesiumGeospatial/GlobeRectangle.h>
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
#include <thread>

namespace reactnativecesium {

int32_t CesiumEngine::resolveTaskThreadCount(int32_t requested) {
  if (requested > 0) return requested;
  unsigned int hw = std::thread::hardware_concurrency();
  if (hw == 0) return 4;
  // Reserve one core for the UI / display thread; clamp into a sensible band.
  int32_t target = static_cast<int32_t>(hw) - 1;
  if (target < 2) target = 2;
  if (target > 8) target = 8;
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
}

void CesiumEngine::destroyTileset() {
  currentOverlay_ = nullptr;
  currentImageryAssetId_ = 1;
  tileset_.reset();
  if (taskProcessor_) {
    taskProcessor_->waitUntilIdle();
  }
}

void CesiumEngine::setImageryAssetId(int64_t assetId) {
  if (config_.ionAccessToken.empty()) return;
  config_.ionImageryAssetId = (assetId <= 0) ? 1 : assetId;
  applyImageryOverlay(config_.ionImageryAssetId);
}

void CesiumEngine::updateFrame(double w, double h, FrameResult& result) {
  result.localPositions.clear();
  result.altitudes.clear();
  result.uvs.clear();
  result.indices.clear();
  result.draws.clear();
  result.creditHtmlLines.clear();

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
    lifecycle_.advanceFrame();
    return;
  }

  result.localPositions.reserve(tunables::kFrameResultPositionFloatReserve);
  result.altitudes.reserve(tunables::kFrameResultPositionFloatReserve / 3);
  result.uvs.reserve(tunables::kFrameResultPositionFloatReserve * 2);
  result.indices.reserve(tunables::kFrameResultIndexUint32Reserve);

  asyncSystem_.dispatchMainThreadTasks();

  const auto viewState     = camera_.computeViewState(w, h);
  const auto& updateResult =
      tileset_->updateViewGroup(tileset_->getDefaultViewGroup(), {viewState});
  tileset_->loadTiles();

  // Only emit the fallback ellipsoid when no real terrain is being rendered
  // this frame. Otherwise it would be over-drawn anyway (~8k vertices and
  // ~24k indices of throwaway transform + memcpy per frame).
  const bool noVisibleTerrain = updateResult.tilesToRenderThisFrame.empty();
  if (noVisibleTerrain) {
    appendEllipsoidDraws(result);
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

  // ── Terrain floor tracking (for minAltitudeAboveTerrain clamp) ───────────
  // Computed once before the tile loop and updated per-tile inside it.
  // Only active when the feature is enabled to avoid any overhead otherwise.
  const bool trackTerrainFloor = (config_.minAltitudeAboveTerrain > 0.0f);
  const CameraParams camParams  = trackTerrainFloor ? camera_.getParams() : CameraParams{};
  const double camLatRad = glm::radians(camParams.latitude);
  const double camLonRad = glm::radians(camParams.longitude);
  float  bestTerrainFloor = terrainFloorEllipsoidM_; // carry previous value if no tile matches
  double bestTerrainDist  = std::numeric_limits<double>::max();

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

      // ── Terrain floor: find nearest vertex to camera nadir ────────────
      if (trackTerrainFloor &&
          rect.contains(CesiumGeospatial::Cartographic(camLonRad, camLatRad, 0.0))) {
        for (const auto& prim : res->primitives) {
          if (prim.altitudes.empty() || prim.localPositions.empty()) continue;
          const size_t vertexCount = prim.localPositions.size();
          for (size_t vi = 0; vi < vertexCount; ++vi) {
            const glm::dvec3 vECEF =
                glm::dvec3(prim.localPositions[vi]) + prim.rtcCenter;
            const double dist = glm::length(vECEF - cameraPos);
            if (dist < bestTerrainDist) {
              bestTerrainDist  = dist;
              bestTerrainFloor = prim.altitudes[vi];
            }
          }
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

      // ── RTC: copy tile-local positions & altitudes ────────────────────────
      result.localPositions.resize(result.localPositions.size() + vertexCount * 3);
      result.altitudes.resize(result.altitudes.size() + vertexCount);
      result.uvs.resize(result.uvs.size() + vertexCount * 2);

      float* posOut = result.localPositions.data() + baseVertex * 3;
      float* altOut = result.altitudes.data()      + baseVertex;
      float* uvOut  = result.uvs.data()            + baseVertex * 2;

      for (size_t vi = 0; vi < vertexCount; ++vi) {
        const glm::vec3& lp = prim.localPositions[vi];
        *posOut++ = lp.x;
        *posOut++ = lp.y;
        *posOut++ = lp.z;
        *altOut++ = prim.altitudes.empty() ? 0.0f : prim.altitudes[vi];
        if (hasPrimUVs) {
          *uvOut++ = prim.uvs[vi].x;
          *uvOut++ = prim.uvs[vi].y;
        } else {
          *uvOut++ = 0.5f;
          *uvOut++ = 0.5f;
        }
      }

      for (uint32_t idx : prim.indices) {
        result.indices.push_back(static_cast<uint32_t>(baseVertex) + idx);
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
      result.draws.push_back(draw);
    }
  }

  result.tilesRendered = static_cast<int>(updateResult.tilesToRenderThisFrame.size());
  result.tilesLoading  = updateResult.workerThreadTileLoadQueueLength +
                         updateResult.mainThreadTileLoadQueueLength;
  result.tilesVisited  = static_cast<int>(updateResult.tilesVisited);
  const auto params    = camera_.getParams();
  result.cameraLat     = params.latitude;
  result.cameraLon     = params.longitude;
  result.cameraAlt     = params.altitude;

  if (trackTerrainFloor) {
    terrainFloorEllipsoidM_ = bestTerrainFloor;
  }

  lifecycle_.advanceFrame();
}

} // namespace reactnativecesium
