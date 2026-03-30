// ---------------------------------------------------------------------------
// The Cesium Native xcframework is compiled with -DNDEBUG (Release mode).
// ReferenceCounted<T,false> in debug mode inherits from ThreadIdHolder<false>,
// which adds a std::thread::id _threadID field — absent in the release binary.
// This shifts _referenceCount from offset 8 to offset 16, creating an ABI
// mismatch: addReference() reads garbage memory and the thread-ID assertion
// fires even on the correct thread.  Defining NDEBUG before any Cesium headers
// forces the same (release) layout, eliminating the spurious crash.
// ---------------------------------------------------------------------------
#ifndef NDEBUG
# define NDEBUG
# define CESIUM_ENGINE_UNDEF_NDEBUG  // undefine after Cesium includes
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

// Restore NDEBUG state after Cesium headers so the rest of the TU can use
// assert() / our own debug guards normally.
#ifdef CESIUM_ENGINE_UNDEF_NDEBUG
# undef NDEBUG
# undef CESIUM_ENGINE_UNDEF_NDEBUG
#endif

#include <spdlog/spdlog.h>

#include <glm/gtc/matrix_inverse.hpp>

#include <pthread.h>   // pthread_self / pthread_main_np
#include <functional>  // std::hash
#include <signal.h>    // signal / SIGABRT

// ---------------------------------------------------------------------------
// Verbose instrumentation — kept active while diagnosing the overlay crash.
// Guarded by CESIUM_ENGINE_VERBOSE instead of NDEBUG so it remains independent
// of the ABI workaround above.
// ---------------------------------------------------------------------------
#define CESIUM_ENGINE_VERBOSE 1

#ifdef CESIUM_ENGINE_VERBOSE

static void cesiumSigAbrtHandler(int) {
  auto tid = std::this_thread::get_id();
  uint64_t tidHash = static_cast<uint64_t>(std::hash<std::thread::id>{}(tid));
  bool isMain = (pthread_main_np() != 0);
  spdlog::critical(
      "[CESIUM-CRASH] SIGABRT in CesiumEngine thread "
      "hash=0x{:016x} isMain={} pthread=0x{:016x}",
      tidHash, isMain, reinterpret_cast<uint64_t>(pthread_self()));
  signal(SIGABRT, SIG_DFL);
  abort();
}

static void installCesiumCrashHandler() {
  static bool installed = false;
  if (!installed) {
    signal(SIGABRT, cesiumSigAbrtHandler);
    installed = true;
  }
}

#define CESIUM_LOG_THREAD(tag)                                              \
  do {                                                                      \
    auto _tid = std::this_thread::get_id();                                 \
    uint64_t _hash = static_cast<uint64_t>(                                 \
        std::hash<std::thread::id>{}(_tid));                                \
    bool _main = (pthread_main_np() != 0);                                  \
    spdlog::warn("[CESIUM-DBG] {} thread hash=0x{:016x} isMain={} "        \
                 "pthread=0x{:016x}",                                       \
                 (tag), _hash, _main,                                       \
                 reinterpret_cast<uint64_t>(pthread_self()));               \
  } while (false)

#else
#define CESIUM_LOG_THREAD(tag) (void)0
static void installCesiumCrashHandler() {}
#endif // CESIUM_ENGINE_VERBOSE

namespace reactnativecesium {

CesiumEngine::CesiumEngine()
    : taskProcessor_(std::make_shared<TaskProcessor>(4)),
      asyncSystem_(taskProcessor_),
      constructionThreadId_(std::this_thread::get_id()) {
  Cesium3DTilesContent::registerAllTileContentTypes();
  installCesiumCrashHandler();
  CESIUM_LOG_THREAD("CesiumEngine::CesiumEngine (constructor)");
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

  // Add the imagery overlay immediately, before updateView() is ever called
  // and before any async tile-loading work has been dispatched.
  if (imageryAssetId != 1) {
    CESIUM_LOG_THREAD("createTileset: before overlay add");

#ifndef NDEBUG
    {
      auto currentId = std::this_thread::get_id();
      bool sameAsConstruction = (currentId == constructionThreadId_);
      bool isMain = (pthread_main_np() != 0);
      uint64_t currentHash = static_cast<uint64_t>(
          std::hash<std::thread::id>{}(currentId));
      uint64_t constructHash = static_cast<uint64_t>(
          std::hash<std::thread::id>{}(constructionThreadId_));
      spdlog::warn(
          "[CESIUM-DBG] overlay add: imageryAssetId={} "
          "currentThread=0x{:016x} constructionThread=0x{:016x} "
          "sameAsConstruction={} isMain={} activeTasks={}",
          imageryAssetId, currentHash, constructHash,
          sameAsConstruction, isMain,
          taskProcessor_->getActiveTaskCount());
    }
#endif

    auto* rawOverlay = new CesiumRasterOverlays::IonRasterOverlay(
        "imagery", imageryAssetId, token);

    CESIUM_LOG_THREAD("createTileset: IonRasterOverlay allocated");
    spdlog::warn("[CESIUM-DBG] rawOverlay ptr=0x{:016x} refCount={}",
                 reinterpret_cast<uint64_t>(rawOverlay),
                 rawOverlay->getReferenceCount());

    CesiumUtility::IntrusivePointer<CesiumRasterOverlays::RasterOverlay> ov =
        rawOverlay;

    CESIUM_LOG_THREAD("createTileset: IntrusivePointer constructed");
    tileset_->getOverlays().add(ov);
    CESIUM_LOG_THREAD("createTileset: overlay added OK");
  }
}

void CesiumEngine::destroyTileset() {
  CESIUM_LOG_THREAD("destroyTileset: enter");
  spdlog::warn("[CESIUM-DBG] destroyTileset: queued={} active={} before reset",
               taskProcessor_->getQueuedTaskCount(),
               taskProcessor_->getActiveTaskCount());

  tileset_.reset();

  spdlog::warn("[CESIUM-DBG] destroyTileset: queued={} active={} after reset, waiting...",
               taskProcessor_->getQueuedTaskCount(),
               taskProcessor_->getActiveTaskCount());

  taskProcessor_->waitUntilIdle();

  spdlog::warn("[CESIUM-DBG] destroyTileset: idle — queued={} active={}",
               taskProcessor_->getQueuedTaskCount(),
               taskProcessor_->getActiveTaskCount());
  CESIUM_LOG_THREAD("destroyTileset: exit");
}

void CesiumEngine::queueImageryOverlay(int64_t assetId) {
  // With the inline-add approach (overlay added inside createTileset), this
  // method is only called after a complete engine restart via buildEngine().
  // The engine has a fresh tileset that was created with the correct overlay
  // already inline, so there is nothing to queue here.  We only update the
  // tracking variable so updateConfig() can re-apply it if needed.
  currentImageryAssetId_ = assetId;
}

void CesiumEngine::setImageryAssetId(int64_t assetId) {
  if (config_.ionAccessToken.empty()) return;
  currentImageryAssetId_ = assetId;
  destroyTileset();
  createTileset(config_.ionAccessToken, config_.ionAssetId, assetId);
}

FrameResult CesiumEngine::updateFrame(double w, double h) {
  FrameResult result;
  ++frameCount_;

  if (!tileset_) return result;

  // Flush async work (tile loads, overlay resolution) that worker threads have
  // posted back to the main thread.
  asyncSystem_.dispatchMainThreadTasks();

  // Camera position in ECEF (double precision for eye-relative subtraction).
  const glm::dvec3 cameraPos = camera_.getECEFPosition();
  result.cameraEcef = glm::vec3(cameraPos);

  // VP matrix for the GPU (rotation-only view + reversed-Z projection).
  result.vpMatrix = camera_.computeVPMatrix(w, h);
  result.invVP    = glm::inverse(result.vpMatrix);

  // Ask Cesium which tiles to render.
  const auto viewState     = camera_.computeViewState(w, h);
  const auto& updateResult = tileset_->updateView({viewState});

  // Merge visible tile geometry into single eye-relative float buffers.
  result.eyeRelPositions.reserve(512 * 1024);
  result.uvs.reserve(512 * 1024 * 2); // 2 floats per vertex
  result.indices.reserve(3 * 1024 * 1024);

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

      const size_t currentVertex =
          result.eyeRelPositions.size() / 3;

      const uint32_t indexByteOffset =
          static_cast<uint32_t>(result.indices.size() * sizeof(uint32_t));

      const bool hasPrimUVs =
          !prim.uvs.empty() && prim.uvs.size() == prim.positionsEcef.size();

      // Convert each vertex from absolute ECEF (double) to eye-relative (float).
      // Simultaneously populate the UV buffer (real UVs or (0.5,0.5) placeholder).
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

      // Offset indices by cumulative vertex count.
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

  // Debug info.
  result.tilesRendered = static_cast<int>(updateResult.tilesToRenderThisFrame.size());
  result.tilesLoading  = updateResult.workerThreadTileLoadQueueLength +
                         updateResult.mainThreadTileLoadQueueLength;
  result.tilesVisited  = static_cast<int>(updateResult.tilesVisited);
  const auto params    = camera_.getParams();
  result.cameraLat     = params.latitude;
  result.cameraLon     = params.longitude;
  result.cameraAlt     = params.altitude;

  // Advance lifecycle frame counter.
  lifecycle_.advanceFrame();

  return result;
}

} // namespace reactnativecesium
