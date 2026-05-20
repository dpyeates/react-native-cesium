#import "CesiumBridge.h"
#import <CoreFoundation/CoreFoundation.h>

#include "engine/CameraIntegrator.hpp"
#include "engine/CesiumEngine.hpp"
#include "engine/EngineTunables.hpp"
#include "engine/GeoidConverter.hpp"
#include "engine/MetricsAggregator.hpp"
#include "metal/MetalBackend.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>

@implementation CesiumBridge {
  std::unique_ptr<reactnativecesium::MetalBackend>      _metalBackend;
  std::unique_ptr<reactnativecesium::CesiumEngine>      _engine;
  reactnativecesium::FrameResult                        _frameResult;

  reactnativecesium::EngineConfig                       _config;
  std::unique_ptr<reactnativecesium::CameraIntegrator>  _integrator;
  reactnativecesium::MetricsAggregator                  _metrics;

  // Force-render flag — set on every demand change so the render loop knows
  // to dispatch at least one frame to advance the integrator. Cleared once
  // the integrator reports isActive() == false.
  std::atomic<bool>                                     _forceRender;

  int   _viewportWidth;
  int   _viewportHeight;
  BOOL  _initialized;
  BOOL  _suspended;

  // Last wall-clock tick used to compute the metrics-only frame dt EMA. Kept
  // per-instance (not a function-local static) so a hot reload that destroys
  // and recreates the bridge does not pollute the new bridge's first frame.
  double _lastTickSec;

  // Last time tilesLoading was > 0. Used to keep the render loop at full speed
  // for a grace period after loading finishes so LOD transitions can complete
  // quickly and updateViewGroup can immediately discover the next refinement
  // level — preventing the cascade of 5-second idle gaps between load phases.
  // Sentinel (epoch) means "no loading seen yet".
  std::chrono::steady_clock::time_point _lastLoadingTime;

  NSString* _cacheDir;
}

- (instancetype)initWithMetalLayer:(CAMetalLayer *)layer
                              width:(int)width
                             height:(int)height
                           cacheDir:(NSString *)cacheDir
                    ionAccessToken:(NSString *)token
                         ionAssetId:(int64_t)assetId
                 sqliteCacheMaxRows:(int32_t)sqliteRows
               taskProcessorThreads:(int32_t)threads {
  self = [super init];
  if (self) {
    _viewportWidth     = width;
    _viewportHeight    = height;
    _initialized       = NO;
    _suspended         = NO;
    _lastTickSec       = 0.0;
    _forceRender.store(true, std::memory_order_release);
    _cacheDir          = [cacheDir copy];

    _config.cacheDatabasePath =
        cacheDir ? std::string([cacheDir UTF8String]) + "/cesium_cache.db" : "";

    NSString* caPem = [[NSBundle mainBundle] pathForResource:@"cacert" ofType:@"pem"];
    if (caPem.length > 0) {
      _config.tlsCaBundlePath = std::string([caPem fileSystemRepresentation]);
    }
    static std::atomic<int> caWarned{0};
    if (_config.tlsCaBundlePath.empty() && caWarned.fetch_add(1) == 0) {
      NSLog(@"[ReactNativeCesium] cacert.pem not in main bundle — libcurl may "
            @"fail TLS to api.cesium.com on device. Ensure the pod includes "
            @"ios/cacert.pem (run pod install).");
    }

    // Seed the ion credentials before buildEngine so createTileset() can
    // immediately start the async tileset.json HTTP round-trip. This request
    // (~1.7 s network) will then run concurrently with Metal PSO compilation
    // below rather than serially after it.
    if (token.length > 0) {
      _config.ionAccessToken = std::string([token UTF8String]);
      _config.ionAssetId     = assetId;
    }

    // These two params are consumed once inside CesiumEngine::initialize()
    // (SqliteCache constructor and TaskProcessor thread-pool sizing). Setting
    // them after buildEngine() has no effect, so they must be seeded here.
    if (sqliteRows > 0) {
      _config.sqliteCacheMaxRows = std::max<int32_t>(64, sqliteRows);
    }
    if (threads >= 0) {
      _config.taskProcessorThreads = threads;
    }

    _metalBackend = std::make_unique<reactnativecesium::MetalBackend>();

    // Start the async tileset.json fetch before Metal PSO compilation so both
    // operations run concurrently. The network round-trip (~1.7 s) overlaps
    // with pipeline compilation rather than waiting behind it.
    [self buildEngine];
    _metalBackend->initialize((__bridge void*)layer, width, height);

    _initialized = YES;

    [[NSNotificationCenter defaultCenter]
        addObserver:self selector:@selector(appWillResignActive:)
               name:UIApplicationWillResignActiveNotification object:nil];
    [[NSNotificationCenter defaultCenter]
        addObserver:self selector:@selector(appDidBecomeActive:)
               name:UIApplicationDidBecomeActiveNotification object:nil];
  }
  return self;
}

- (void)appWillResignActive:(NSNotification *)note { _suspended = YES; }
- (void)appDidBecomeActive:(NSNotification *)note  {
  _suspended = NO;
  _forceRender.store(true, std::memory_order_release);
}

- (void)buildEngine {
  _engine.reset();
  _engine = std::make_unique<reactnativecesium::CesiumEngine>();
  _engine->initialize(_config);

  // Seed the integrator from the engine's initial camera (which itself was
  // populated by GlobeCamera's defaults). This puts the demand and actual
  // states in sync from the very first frame; no "snap from default" jump.
  _integrator = std::make_unique<reactnativecesium::CameraIntegrator>(
      _engine->camera().getParams());

  auto* backendPtr = _metalBackend.get();
  _engine->getResourcePreparer()->setGPUTextureCreator(
      [backendPtr](const uint8_t* pixels, int32_t w, int32_t h) -> void* {
        return backendPtr->createRasterTexture(pixels, w, h);
      });
  _engine->getResourcePreparer()->setGPUTextureDeleter(
      [](void* tex) {
        if (tex) CFRelease(tex);
      });
  _engine->getResourcePreparer()->setWaterMaskTextureCreator(
      [backendPtr](const uint8_t* pixels, int32_t w, int32_t h) -> void* {
        return backendPtr->createRasterTexture(pixels, w, h);
      });
  _engine->getResourcePreparer()->setWaterMaskTextureDeleter(
      [](void* tex) {
        if (tex) CFRelease(tex);
      });
}

- (void)applyEngineConfig {
  if (!_engine) return;
  _engine->updateConfig(_config);
  _forceRender.store(true, std::memory_order_release);
}

- (void)updateIonAccessToken:(NSString *)token assetId:(int64_t)assetId {
  if (!_initialized) return;
  _config.ionAccessToken = token ? std::string([token UTF8String]) : std::string();
  _config.ionAssetId     = assetId;
  [self applyEngineConfig];
}

- (void)updateImageryAssetId:(int64_t)assetId {
  if (!_initialized) return;
  _config.ionImageryAssetId = (assetId <= 0) ? 1 : assetId;
  _engine->setImageryAssetId(_config.ionImageryAssetId);
  _forceRender.store(true, std::memory_order_release);
}

// ── Per-DoF setters ──────────────────────────────────────────────────────

- (void)setPositionLatitude:(double)lat longitude:(double)lon {
  if (!_initialized) return;
  _integrator->setPosition(lat, lon);
  _forceRender.store(true, std::memory_order_release);
}

- (void)setAltitude:(double)alt {
  if (!_initialized) return;
  _integrator->setAltitude(alt);
  _forceRender.store(true, std::memory_order_release);
}

- (void)setHeadingDeg:(double)deg {
  if (!_initialized) return;
  _integrator->setHeading(deg);
  _forceRender.store(true, std::memory_order_release);
}

- (void)setAttitudePitch:(double)pitch roll:(double)roll {
  if (!_initialized) return;
  _integrator->setAttitude(pitch, roll);
  _forceRender.store(true, std::memory_order_release);
}

- (void)setViewCorrectionW:(double)qw x:(double)qx y:(double)qy z:(double)qz {
  if (!_initialized) return;
  _integrator->setViewCorrection(glm::dquat(qw, qx, qy, qz));
  _forceRender.store(true, std::memory_order_release);
}

- (void)teleportLatitude:(double)lat
                longitude:(double)lon
                 altitude:(double)alt
                  heading:(double)heading
                    pitch:(double)pitch
                     roll:(double)roll
           verticalFovDeg:(double)vfov {
  if (!_initialized) return;
  reactnativecesium::CameraParams p;
  p.latitude    = lat;
  p.longitude   = lon;
  p.altitude    = alt;
  p.heading     = heading;
  p.pitch       = pitch;
  p.roll        = roll;
  p.verticalFov = vfov;
  // Keep the latest viewCorrection target — teleport is a position/orientation
  // jump, not a boresight reset. Use setViewCorrection(identity) to clear it.
  p.viewCorrection = _integrator->getActual().viewCorrection;
  _integrator->teleport(p);
  // Apply directly to the engine so the next render uses the teleported value
  // without one frame of "almost there" interpolation.
  _engine->camera().setParams(p);
  _forceRender.store(true, std::memory_order_release);
}

- (void)resize:(int)width height:(int)height {
  _viewportWidth  = width;
  _viewportHeight = height;
  if (_metalBackend) _metalBackend->resize(width, height);
  _forceRender.store(true, std::memory_order_release);
}

- (void)setVerticalFovDeg:(double)degrees {
  if (!_initialized) return;
  _integrator->setVerticalFov(degrees);
  _forceRender.store(true, std::memory_order_release);
}

- (void)setRateCapsYaw:(double)yawDegSec
                  pitch:(double)pitchDegSec
                   roll:(double)rollDegSec
                  climb:(double)climbMps
            groundSpeed:(double)groundMps {
  if (!_initialized) return;
  reactnativecesium::CameraRateCaps caps;
  caps.maxYawRateDegSec   = yawDegSec;
  caps.maxPitchRateDegSec = pitchDegSec;
  caps.maxRollRateDegSec  = rollDegSec;
  caps.maxClimbRateMps    = climbMps;
  caps.maxGroundSpeedMps  = groundMps;
  _integrator->setRateCaps(caps);
}

- (void)setMaximumScreenSpaceError:(double)v {
  _config.maximumScreenSpaceError = v;
  if (_initialized) [self applyEngineConfig];
}
- (void)setMaximumSimultaneousTileLoads:(int32_t)v {
  _config.maximumSimultaneousTileLoads = v;
  if (_initialized) [self applyEngineConfig];
}
- (void)setLoadingDescendantLimit:(int32_t)v {
  _config.loadingDescendantLimit = v;
  if (_initialized) [self applyEngineConfig];
}
- (void)setMaximumCachedMiB:(int32_t)v {
  _config.maximumCachedBytes =
      static_cast<int64_t>(std::max<int32_t>(16, v)) * 1024LL * 1024LL;
  if (_initialized) [self applyEngineConfig];
}
- (void)setPreloadAncestors:(BOOL)v {
  _config.preloadAncestors = (v == YES);
  if (_initialized) [self applyEngineConfig];
}
- (void)setPreloadSiblings:(BOOL)v {
  _config.preloadSiblings = (v == YES);
  if (_initialized) [self applyEngineConfig];
}
- (void)setForbidHoles:(BOOL)v {
  _config.forbidHoles = (v == YES);
  if (_initialized) [self applyEngineConfig];
}
- (void)setEnableWaterMask:(BOOL)v {
  _config.enableWaterMask = (v == YES);
  if (_initialized) [self applyEngineConfig];
}
- (void)setEnableFogCulling:(BOOL)v {
  _config.enableFogCulling = (v == YES);
  if (_initialized) [self applyEngineConfig];
}
- (void)setEnforceCulledScreenSpaceError:(BOOL)v {
  _config.enforceCulledScreenSpaceError = (v == YES);
  if (_initialized) [self applyEngineConfig];
}
- (void)setCulledScreenSpaceError:(double)v {
  _config.culledScreenSpaceError = v;
  if (_initialized) [self applyEngineConfig];
}
- (void)setEnableLodTransitionPeriod:(BOOL)v {
  _config.enableLodTransitionPeriod = (v == YES);
  if (_initialized) [self applyEngineConfig];
}
- (void)setLodTransitionLength:(double)v {
  _config.lodTransitionLength = v;
  if (_initialized) [self applyEngineConfig];
}
- (void)setSqliteCacheMaxRows:(int32_t)v {
  // Cache rows are taken into account on the next initialize().
  _config.sqliteCacheMaxRows = std::max<int32_t>(64, v);
}
- (void)setTaskProcessorThreads:(int32_t)v {
  _config.taskProcessorThreads = std::max<int32_t>(0, v);
}
- (void)setMinAltitudeAboveTerrain:(float)v {
  _config.minAltitudeAboveTerrain = std::max(0.0f, v);
  if (_engine) _engine->updateConfig(_config);
  _forceRender.store(true, std::memory_order_release);
}

- (void)setMsaaSampleCount:(int)samples {
  if (_metalBackend) _metalBackend->setMsaaSampleCount(samples);
  _forceRender.store(true, std::memory_order_release);
}

- (void)markNeedsRender { _forceRender.store(true, std::memory_order_release); }

- (BOOL)shouldRenderNextFrame {
  if (!_initialized || _suspended || !_engine) return NO;
  if (_forceRender.load(std::memory_order_acquire))            return YES;
  if (!_frameResult.tilesetActive)                             return YES;
  if (_frameResult.tilesLoading > 0)                          return YES;
  if (_integrator && _integrator->isActive())                  return YES;
  // ── Post-loading grace period ─────────────────────────────────────────────
  // Keep the render loop at full speed for 2 seconds after the last non-zero
  // loading frame. This fixes a cascade of multi-second idle gaps that
  // otherwise appear between load phases:
  //
  //  1. Loading queue drains → shouldRenderNextFrame returns NO
  //  2. Loop drops to 4 fps (0.25 s idle probe)
  //  3. deltaTime is clamped to 100 ms → a 0.5 s LOD fade takes 5 idle
  //     frames × 0.25 s = 1.25 s of wall-clock time instead of 0.5 s
  //  4. kickDescendantsWhileFadingIn=true means descendants can't be
  //     *requested* until the parent finishes fading
  //  5. loadTiles() is only called inside renderFrame, so tile requests are
  //     throttled to 4 per second during idle
  //  The net effect: each of the ~5 refinement levels adds a 5-second gap.
  //
  // With a 2-second grace period:
  //  • LOD fades complete at full speed (0.5 s real, not 1.25 s)
  //  • updateViewGroup discovers the next level immediately after the fade
  //  • Workers start downloading before we ever slow down
  //  → The 5-second inter-phase gaps collapse to ~0.5 s
  if (_lastLoadingTime.time_since_epoch().count() > 0) {
    const double secSinceLoad = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - _lastLoadingTime).count();
    if (secSinceLoad < 2.0) return YES;
  }
  return NO;
}

- (void)renderFrameAt:(double)nowSeconds {
  if (!_initialized || _suspended) return;
  if (!_engine || !_integrator || !_metalBackend) return;

  @autoreleasepool {
    // Consume the force-render flag (we're rendering this frame either way).
    _forceRender.store(false, std::memory_order_release);

    // The integrator owns its own clock now — see CameraIntegrator::step().
    // `nowSeconds` is still used below for the FPS-EMA dt only.
    auto actual = _integrator->step();

    // ── Terrain floor clamp ───────────────────────────────────────────────
    // Uses terrain floor from the previous frame (one-frame lag is
    // imperceptible at 60 fps). Skipped when minAltitudeAboveTerrain == 0.
    const float minAbove = _engine->getConfig().minAltitudeAboveTerrain;
    if (minAbove > 0.0f && _engine->terrainFloorKnown()) {
      const double geoidN =
          reactnativecesium::mslToEllipsoidMeters(actual.latitude, actual.longitude, 0.0);
      const double floorMsl =
          static_cast<double>(_engine->terrainFloorEllipsoidMeters()) - geoidN;
      const double minMsl = floorMsl + static_cast<double>(minAbove);
      if (actual.altitude < minMsl) {
        _integrator->clampActualAltitude(minMsl);
        actual.altitude = minMsl;
      }
    }

    _engine->camera().setParams(actual);

    _engine->updateFrame(_viewportWidth, _viewportHeight, _frameResult);

    // Track the last time tiles were actively loading so shouldRenderNextFrame
    // can suppress the idle transition for a grace period (see that method).
    if (_frameResult.tilesLoading > 0) {
      _lastLoadingTime = std::chrono::steady_clock::now();
    }

    // For metrics purposes only — keeps the FPS EMA in something close to the
    // wall-clock cadence. Not used by the integrator.
    double dt = (_lastTickSec > 0.0) ? (nowSeconds - _lastTickSec) : (1.0 / 60.0);
    _lastTickSec = nowSeconds;
    if (dt > 0.5 || dt <= 0.0) dt = 1.0 / 60.0;
    _metrics.tick(dt, _frameResult, !_config.tlsCaBundlePath.empty());

    reactnativecesium::FrameParams params;
    _metalBackend->beginFrame(params);
    _metalBackend->drawScene(_frameResult);
    _metalBackend->endFrame();
  }
}

- (double)metricsFps              { return _metrics.latest().fps; }
- (NSInteger)metricsTilesRendered { return _metrics.latest().tilesRendered; }
- (NSInteger)metricsTilesLoading  { return _metrics.latest().tilesLoading; }
- (NSInteger)metricsTilesVisited  { return _metrics.latest().tilesVisited; }
- (BOOL)metricsIonTokenConfigured { return _metrics.latest().ionTokenConfigured; }
- (BOOL)metricsTilesetReady       { return _metrics.latest().tilesetReady; }
- (BOOL)metricsTlsConfigured      { return _metrics.latest().tlsConfigured; }
- (NSString*)metricsCreditsPlainText {
  return [NSString stringWithUTF8String:_metrics.latest().creditsPlainText.c_str()];
}

// ── Actual camera readback (post-integration; what was rendered) ─────────
- (double)readActualLatitude       { return _integrator ? _integrator->getActual().latitude       : 0.0; }
- (double)readActualLongitude      { return _integrator ? _integrator->getActual().longitude      : 0.0; }
- (double)readActualAltitude       { return _integrator ? _integrator->getActual().altitude       : 0.0; }
- (double)readActualHeading        { return _integrator ? _integrator->getActual().heading        : 0.0; }
- (double)readActualPitch          { return _integrator ? _integrator->getActual().pitch          : 0.0; }
- (double)readActualRoll           { return _integrator ? _integrator->getActual().roll           : 0.0; }
- (double)readActualVerticalFovDeg { return _integrator ? _integrator->getActual().verticalFov    : 60.0; }

// ── Demand camera readback (what the consumer last asked for) ────────────
- (double)readDemandLatitude       { return _integrator ? _integrator->getDemand().latitude       : 0.0; }
- (double)readDemandLongitude      { return _integrator ? _integrator->getDemand().longitude      : 0.0; }
- (double)readDemandAltitude       { return _integrator ? _integrator->getDemand().altitude       : 0.0; }
- (double)readDemandHeading        { return _integrator ? _integrator->getDemand().heading        : 0.0; }
- (double)readDemandPitch          { return _integrator ? _integrator->getDemand().pitch          : 0.0; }
- (double)readDemandRoll           { return _integrator ? _integrator->getDemand().roll           : 0.0; }
- (double)readDemandVerticalFovDeg { return _integrator ? _integrator->getDemand().verticalFov    : 60.0; }

- (double)readViewCorrectionW {
  return _integrator ? _integrator->getActual().viewCorrection.w : 1.0;
}
- (double)readViewCorrectionX {
  return _integrator ? _integrator->getActual().viewCorrection.x : 0.0;
}
- (double)readViewCorrectionY {
  return _integrator ? _integrator->getActual().viewCorrection.y : 0.0;
}
- (double)readViewCorrectionZ {
  return _integrator ? _integrator->getActual().viewCorrection.z : 0.0;
}

- (void)shutdown {
  [[NSNotificationCenter defaultCenter] removeObserver:self];

  // Mark uninitialised up-front so any late prop setters that race the
  // teardown become no-ops instead of touching half-destroyed state.
  _initialized = NO;

  if (_engine) _engine->shutdown();
  if (_metalBackend) _metalBackend->shutdown();

  // Release the engine (and therefore the SqliteCache it owns) here rather
  // than waiting for ARC to dealloc this CesiumBridge. During fast refresh
  // the next CesiumView can mount and open the same cesium_cache.db file
  // before our owning Swift view's deinit completes; freeing the unique_ptr
  // now closes the SQLite handle synchronously and avoids "database is
  // locked" warnings from the new engine's SqliteCache.
  _integrator.reset();
  _engine.reset();
  _metalBackend.reset();
}

- (void)dealloc {
  // Defensive: shutdown should already have run from the owning Swift view's
  // deinit. If something exotic happened (e.g. a partially-built bridge that
  // never had shutdown invoked, or an Objective-C exception path) we still
  // need to detach from NSNotificationCenter and free the C++ owners in a
  // deterministic order before ARC walks the ivar block.
  if (_initialized) {
    [self shutdown];
  } else {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    _integrator.reset();
    _engine.reset();
    _metalBackend.reset();
  }
}

@end
