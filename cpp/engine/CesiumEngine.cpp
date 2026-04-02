#ifndef NDEBUG
# define NDEBUG
# define CESIUM_ENGINE_UNDEF_NDEBUG
#endif

#include "CesiumEngine.hpp"

#include <Cesium3DTilesContent/registerAllTileContentTypes.h>
#include <Cesium3DTilesSelection/Tile.h>
#include <Cesium3DTilesSelection/Tileset.h>
#include <Cesium3DTilesSelection/TilesetExternals.h>
#include <Cesium3DTilesSelection/TilesetOptions.h>
#include <CesiumAsync/CachingAssetAccessor.h>
#include <CesiumAsync/SqliteCache.h>
#include <CesiumCurl/CurlAssetAccessor.h>
#include <CesiumGeospatial/Ellipsoid.h>
#include <CesiumRasterOverlays/IonRasterOverlay.h>
#include <CesiumUtility/CreditSystem.h>
#include <CesiumUtility/IntrusivePointer.h>

#ifdef CESIUM_ENGINE_UNDEF_NDEBUG
# undef NDEBUG
# undef CESIUM_ENGINE_UNDEF_NDEBUG
#endif

#include <spdlog/spdlog.h>
#include <glm/gtc/matrix_inverse.hpp>

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
  // 20 m is enough separation at Earth scale; larger insets only push the
  // fallback mesh further “under” the real globe without much benefit.
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
    const double phi = M_PI * (static_cast<double>(lat) / nLat - 0.5); // -π/2 … +π/2
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
      // CCW winding when viewed from outside Earth.
      ellipsoidIndices_.push_back(v00); ellipsoidIndices_.push_back(v10); ellipsoidIndices_.push_back(v11);
      ellipsoidIndices_.push_back(v00); ellipsoidIndices_.push_back(v11); ellipsoidIndices_.push_back(v01);
    }
  }
}

void CesiumEngine::appendEllipsoidDraws(FrameResult& result) const {
  if (ellipsoidPositions_.empty() || ellipsoidIndices_.empty()) return;

  const glm::dvec3 cameraPos = camera_.getECEFPosition();
  const size_t   baseVertex   = result.eyeRelPositions.size() / 3;
  const uint32_t indexByteOff = static_cast<uint32_t>(result.indices.size() * sizeof(uint32_t));

  for (const auto& posEcef : ellipsoidPositions_) {
    const glm::dvec3 eyeRel = posEcef - cameraPos;
    result.eyeRelPositions.push_back(static_cast<float>(eyeRel.x));
    result.eyeRelPositions.push_back(static_cast<float>(eyeRel.y));
    result.eyeRelPositions.push_back(static_cast<float>(eyeRel.z));

    // Vertices sit on an ellipsoid inset below WGS84 for depth sorting; raw
    // geodetic height would be slightly negative, which the terrain shader
    // blue. For the no-tiles fallback we want nominal land (green) at sea
    // level, not water — use 0 m for shading only.
    result.altitudes.push_back(0.0f);

    // No UVs — shader applies hypsometric-style tint from altitude.
    result.uvs.push_back(0.5f);
    result.uvs.push_back(0.5f);
  }

  for (uint32_t idx : ellipsoidIndices_) {
    result.indices.push_back(static_cast<uint32_t>(baseVertex) + idx);
  }

  DrawPrimitive draw;
  draw.indexByteOffset     = indexByteOff;
  draw.indexCount          = static_cast<uint32_t>(ellipsoidIndices_.size());
  draw.hasUVs              = false;
  draw.isEllipsoidFallback = true;
  draw.overlayTexture      = nullptr;
  result.draws.push_back(draw);
}

void CesiumEngine::initialize(IGPUBackend& /*gpu*/, const EngineConfig& config) {
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
  ++frameCount_;

  result.eyeRelPositions.clear();
  result.uvs.clear();
  result.altitudes.clear();
  result.indices.clear();
  result.draws.clear();
  result.creditHtmlLines.clear();

  result.ionTokenConfigured = !config_.ionAccessToken.empty();
  result.tilesetActive      = (tileset_ != nullptr);
  result.verticalFovDeg     = camera_.getVerticalFovDegrees();

  if (!tileset_) {
    // No Ion tileset configured yet — still render the ellipsoid fallback so
    // the globe is always visible.
    result.vpMatrix   = camera_.computeVPMatrix(w, h);
    result.invVP      = glm::inverse(result.vpMatrix);
    result.cameraEcef = glm::vec3(camera_.getECEFPosition());
    appendEllipsoidDraws(result);
    lifecycle_.advanceFrame();
    return;
  }

  result.eyeRelPositions.reserve(512 * 1024);
  result.uvs.reserve(512 * 1024 * 2);
  result.altitudes.reserve(512 * 1024 / 3);
  result.indices.reserve(3 * 1024 * 1024);

  const auto& ellipsoid = CesiumGeospatial::Ellipsoid::WGS84;

  asyncSystem_.dispatchMainThreadTasks();

  const glm::dvec3 cameraPos = camera_.getECEFPosition();
  result.cameraEcef = glm::vec3(cameraPos);

  result.vpMatrix = camera_.computeVPMatrix(w, h);
  result.invVP    = glm::inverse(result.vpMatrix);

  // Draw ellipsoid first so tile draws (appended below) overdraw it wherever
  // terrain data is available (reversed-Z depth test: closer = higher value).
  appendEllipsoidDraws(result);

  const auto viewState     = camera_.computeViewState(w, h);
  const auto& updateResult = tileset_->updateView({viewState});

  // Include every active credit, not only shouldBeShownOnScreen==true. Cesium
  // marks many provider strings (Bing, Google, etc.) for off-screen / popup
  // attribution; we have no separate popup, so fold them into the footer.
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

    for (const auto& prim : res->primitives) {
      if (prim.indices.empty() || prim.positionsEcef.empty()) continue;

      const size_t   currentVertex   = result.eyeRelPositions.size() / 3;
      const uint32_t indexByteOffset =
          static_cast<uint32_t>(result.indices.size() * sizeof(uint32_t));
      const bool hasPrimUVs =
          !prim.uvs.empty() && prim.uvs.size() == prim.positionsEcef.size();

      for (size_t vi = 0; vi < prim.positionsEcef.size(); ++vi) {
        const glm::dvec3& posEcef = prim.positionsEcef[vi];
        const glm::dvec3 eyeRel = posEcef - cameraPos;
        result.eyeRelPositions.push_back(static_cast<float>(eyeRel.x));
        result.eyeRelPositions.push_back(static_cast<float>(eyeRel.y));
        result.eyeRelPositions.push_back(static_cast<float>(eyeRel.z));

        auto carto = ellipsoid.cartesianToCartographic(posEcef);
        result.altitudes.push_back(
            carto ? static_cast<float>(carto->height) : 0.0f);

        if (hasPrimUVs) {
          result.uvs.push_back(prim.uvs[vi].x);
          result.uvs.push_back(prim.uvs[vi].y);
        } else {
          result.uvs.push_back(0.5f);
          result.uvs.push_back(0.5f);
        }
      }

      for (uint32_t idx : prim.indices) {
        result.indices.push_back(static_cast<uint32_t>(currentVertex) + idx);
      }

      DrawPrimitive draw;
      draw.indexByteOffset      = indexByteOffset;
      draw.indexCount           = static_cast<uint32_t>(prim.indices.size());
      draw.hasUVs               = hasPrimUVs;
      draw.overlayTexture       = res->overlayTexture;
      draw.overlayTranslation   = res->overlayTranslation;
      draw.overlayScale         = res->overlayScale;
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
