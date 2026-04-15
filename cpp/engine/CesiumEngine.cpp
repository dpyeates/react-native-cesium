#ifndef NDEBUG
# define NDEBUG
# define CESIUM_ENGINE_UNDEF_NDEBUG
#endif

#include "CesiumEngine.hpp"

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
#include <CesiumUtility/CreditSystem.h>
#include <CesiumUtility/IntrusivePointer.h>

#ifdef CESIUM_ENGINE_UNDEF_NDEBUG
# undef NDEBUG
# undef CESIUM_ENGINE_UNDEF_NDEBUG
#endif

#include <spdlog/spdlog.h>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace reactnativecesium {

CesiumEngine::CesiumEngine()
    : taskProcessor_(std::make_shared<TaskProcessor>(4)),
      asyncSystem_(taskProcessor_) {
  Cesium3DTilesContent::registerAllTileContentTypes();
}

CesiumEngine::~CesiumEngine() { shutdown(); }

bool CesiumEngine::tilesetOptionsMatch(const EngineConfig& a,
                                       const EngineConfig& b) const {
  return a.maximumScreenSpaceError == b.maximumScreenSpaceError &&
         a.maximumSimultaneousTileLoads == b.maximumSimultaneousTileLoads &&
         a.loadingDescendantLimit == b.loadingDescendantLimit;
}

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

// Append the fallback ellipsoid as a draw.
// RTC convention for the fallback: the tile's "RTC centre" is the camera
// position itself, so local positions == old camera-relative positions.  The
// per-draw MVP is therefore the same rotation-only VP used before RTC was
// introduced, keeping the fallback on the same code path.
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
    // Local position = ECEF - camera (same as old eye-relative, since camera is the RTC centre).
    const glm::dvec3 local = posEcef - cameraPos;
    *posOut++ = static_cast<float>(local.x);
    *posOut++ = static_cast<float>(local.y);
    *posOut++ = static_cast<float>(local.z);
    // Fallback geoid should represent land by default when terrain tiles are not
    // loaded yet, so keep altitude at sea level for hypsometric land colouring.
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
  // Ellipsoid RTC centre = camera → local pos = eye-relative → use the
  // rotation-only VP (identical to the pre-RTC vertex transform).
  draw.mvpMatrix           = result.vpMatrix;
  draw.rtcCenterEcef       = result.cameraEcef;
  result.draws.push_back(draw);
}

void CesiumEngine::initialize(const EngineConfig& config) {
  config_ = config;

  auto logger = spdlog::default_logger();

  CesiumCurl::CurlAssetAccessorOptions curlOpts;
  if (!config.tlsCaBundlePath.empty()) {
    curlOpts.certificateFile = config.tlsCaBundlePath;
  }
  auto curlAccessor = std::make_shared<CesiumCurl::CurlAssetAccessor>(curlOpts);

  if (!config.cacheDatabasePath.empty()) {
    auto cache = std::make_shared<CesiumAsync::SqliteCache>(
        logger, config.cacheDatabasePath, 4096);
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
  createTileset(config.ionAccessToken, config.ionAssetId);
}

void CesiumEngine::shutdown() {
  destroyTileset();
  creditSystem_.reset();
}

void CesiumEngine::updateConfig(const EngineConfig& config) {
  const bool needRebuild =
      config.ionAccessToken != config_.ionAccessToken ||
      config.ionAssetId != config_.ionAssetId ||
      !tilesetOptionsMatch(config, config_);

  config_ = config;

  if (!needRebuild) return;

  destroyTileset();
  if (!config_.ionAccessToken.empty()) {
    createTileset(config_.ionAccessToken, config_.ionAssetId, currentImageryAssetId_);
  }
}

void CesiumEngine::createTileset(const std::string& token,
                                 int64_t            assetId,
                                 int64_t            imageryAssetId) {
  if (token.empty()) return;

  if (!creditSystem_) {
    creditSystem_ = std::make_shared<CesiumUtility::CreditSystem>();
  }

  auto logger = spdlog::default_logger();

  Cesium3DTilesSelection::TilesetExternals externals{
      assetAccessor_, resourcePreparer_, asyncSystem_, creditSystem_,
      logger, nullptr};

  Cesium3DTilesSelection::TilesetOptions opts;
  opts.maximumCachedBytes = 256 * 1024 * 1024;
  opts.maximumSimultaneousTileLoads =
      static_cast<uint32_t>(std::max(0, config_.maximumSimultaneousTileLoads));
  opts.loadingDescendantLimit =
      static_cast<uint32_t>(std::max(1, config_.loadingDescendantLimit));
  opts.maximumScreenSpaceError =
      std::max(1.0, config_.maximumScreenSpaceError);
  opts.preloadAncestors = true;
  opts.preloadSiblings  = true;
  opts.forbidHoles      = true;
  opts.contentOptions.enableWaterMask = true;

  tileset_ = std::make_unique<Cesium3DTilesSelection::Tileset>(
      externals, assetId, token, opts);

  if (imageryAssetId != 1) {
    CesiumUtility::IntrusivePointer<CesiumRasterOverlays::RasterOverlay> ov =
        new CesiumRasterOverlays::IonRasterOverlay("imagery", imageryAssetId, token);
    tileset_->getOverlays().add(ov);
  }
}

void CesiumEngine::destroyTileset() {
  tileset_.reset();
  taskProcessor_->waitUntilIdle();
}

void CesiumEngine::setImageryAssetId(int64_t assetId) {
  if (config_.ionAccessToken.empty()) return;
  currentImageryAssetId_ = assetId;
  destroyTileset();
  createTileset(config_.ionAccessToken, config_.ionAssetId, assetId);
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

  result.localPositions.reserve(512 * 1024);
  result.altitudes.reserve(512 * 1024 / 3);
  result.uvs.reserve(512 * 1024 * 2);
  result.indices.reserve(3 * 1024 * 1024);

  asyncSystem_.dispatchMainThreadTasks();

  // Ellipsoid fallback is appended first so real terrain tiles (drawn later)
  // overdraw it via the reversed-Z depth test.
  appendEllipsoidDraws(result);

  const auto viewState     = camera_.computeViewState(w, h);
  const auto& updateResult = tileset_->updateView({viewState});

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
      // Positions are already relative to prim.rtcCenter (small float values).
      // Altitudes are computed in double in GltfToMesh — sub-millimetre precision.
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
      // Compute the camera-relative offset of the tile's RTC centre in double
      // and bake it into the MVP matrix before casting to float32.  This means
      // the large camera↔tile translation is resolved with double precision;
      // the vertex shader only ever adds small tile-local offsets on top of it.
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

  lifecycle_.advanceFrame();
}

} // namespace reactnativecesium
