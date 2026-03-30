// ---------------------------------------------------------------------------
// The Cesium Native xcframework is compiled with -DNDEBUG (Release mode).
// ReferenceCounted<T,false> in debug mode inherits from ThreadIdHolder<false>,
// which adds a std::thread::id _threadID field absent in the release binary.
// This shifts _referenceCount from offset 8 to offset 16, creating an ABI
// mismatch: addReference() reads garbage memory and the thread-ID assertion
// fires even on the correct thread.  Defining NDEBUG before Cesium headers
// forces the same (release) layout, eliminating the spurious crash.
// ---------------------------------------------------------------------------
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
#include <CesiumRasterOverlays/IonRasterOverlay.h>
#include <CesiumUtility/CreditSystem.h>
#include <CesiumUtility/IntrusivePointer.h>

#ifdef CESIUM_ENGINE_UNDEF_NDEBUG
# undef NDEBUG
# undef CESIUM_ENGINE_UNDEF_NDEBUG
#endif

#include <spdlog/spdlog.h>
#include <glm/gtc/matrix_inverse.hpp>

namespace reactnativecesium {

CesiumEngine::CesiumEngine()
    : taskProcessor_(std::make_shared<TaskProcessor>(4)),
      asyncSystem_(taskProcessor_) {
  Cesium3DTilesContent::registerAllTileContentTypes();
}

CesiumEngine::~CesiumEngine() { shutdown(); }

void CesiumEngine::initialize(IGPUBackend& /*gpu*/, const EngineConfig& config) {
  config_ = config;

  auto logger       = spdlog::default_logger();
  auto curlAccessor = std::make_shared<CesiumCurl::CurlAssetAccessor>();

  if (!config.cacheDatabasePath.empty()) {
    auto cache = std::make_shared<CesiumAsync::SqliteCache>(
        logger, config.cacheDatabasePath, 4096);
    assetAccessor_ = std::make_shared<CesiumAsync::CachingAssetAccessor>(
        logger, curlAccessor, cache);
  } else {
    assetAccessor_ = curlAccessor;
  }

  resourcePreparer_ = std::make_shared<ResourcePreparer>(lifecycle_);
  createTileset(config.ionAccessToken, config.ionAssetId);
}

void CesiumEngine::shutdown() {
  destroyTileset();
}

void CesiumEngine::updateConfig(const EngineConfig& config) {
  bool newTileset = config.ionAccessToken != config_.ionAccessToken ||
                    config.ionAssetId     != config_.ionAssetId;
  config_ = config;
  if (newTileset) {
    destroyTileset();
    createTileset(config.ionAccessToken, config.ionAssetId, currentImageryAssetId_);
  }
}

void CesiumEngine::createTileset(const std::string& token,
                                  int64_t            assetId,
                                  int64_t            imageryAssetId) {
  if (token.empty()) return;

  auto logger       = spdlog::default_logger();
  auto creditSystem = std::make_shared<CesiumUtility::CreditSystem>();

  Cesium3DTilesSelection::TilesetExternals externals{
      assetAccessor_, resourcePreparer_, asyncSystem_, creditSystem,
      logger, nullptr};

  Cesium3DTilesSelection::TilesetOptions opts;
  opts.maximumCachedBytes           = 256 * 1024 * 1024;
  opts.maximumSimultaneousTileLoads = 12;
  opts.loadingDescendantLimit       = 20;
  opts.maximumScreenSpaceError      = 32.0;
  opts.preloadAncestors             = true;
  opts.preloadSiblings              = true;
  opts.forbidHoles                  = true;

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

  // Clear without releasing capacity so the vectors never reallocate after the
  // first warm-up frame (caller holds a persistent FrameResult across frames).
  result.eyeRelPositions.clear();
  result.uvs.clear();
  result.indices.clear();
  result.draws.clear();

  if (!tileset_) return;

  // One-time capacity hints — no-ops on subsequent frames once the vectors
  // have grown to their working size.
  result.eyeRelPositions.reserve(512 * 1024);
  result.uvs.reserve(512 * 1024 * 2);
  result.indices.reserve(3 * 1024 * 1024);

  // Flush async work (tile loads, overlay resolution) posted to the main thread.
  asyncSystem_.dispatchMainThreadTasks();

  // Camera position in ECEF (double precision for eye-relative subtraction).
  const glm::dvec3 cameraPos = camera_.getECEFPosition();
  result.cameraEcef = glm::vec3(cameraPos);

  result.vpMatrix = camera_.computeVPMatrix(w, h);
  result.invVP    = glm::inverse(result.vpMatrix);

  const auto viewState     = camera_.computeViewState(w, h);
  const auto& updateResult = tileset_->updateView({viewState});

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
        const glm::dvec3 eyeRel = prim.positionsEcef[vi] - cameraPos;
        result.eyeRelPositions.push_back(static_cast<float>(eyeRel.x));
        result.eyeRelPositions.push_back(static_cast<float>(eyeRel.y));
        result.eyeRelPositions.push_back(static_cast<float>(eyeRel.z));

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
      draw.indexByteOffset = indexByteOffset;
      draw.indexCount      = static_cast<uint32_t>(prim.indices.size());
      draw.hasUVs          = hasPrimUVs;
      draw.overlayTexture  = res->overlayTexture;
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
