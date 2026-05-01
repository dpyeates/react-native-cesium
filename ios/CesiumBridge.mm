#import "CesiumBridge.h"
#import <CoreFoundation/CoreFoundation.h>

#include "engine/CameraSmoother.hpp"
#include "engine/CameraTargetState.hpp"
#include "engine/CesiumEngine.hpp"
#include "engine/EngineTunables.hpp"
#include "engine/MetricsAggregator.hpp"
#include "metal/MetalBackend.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <string>

@implementation CesiumBridge {
  std::unique_ptr<reactnativecesium::MetalBackend>      _metalBackend;
  std::unique_ptr<reactnativecesium::CesiumEngine>      _engine;
  reactnativecesium::FrameResult                        _frameResult;

  reactnativecesium::EngineConfig         _config;
  reactnativecesium::CameraTargetState    _target;
  reactnativecesium::MetricsAggregator    _metrics;

  int   _viewportWidth;
  int   _viewportHeight;
  BOOL  _initialized;
  BOOL  _suspended;

  NSString* _cacheDir;
}

- (instancetype)initWithMetalLayer:(CAMetalLayer *)layer
                             width:(int)width
                            height:(int)height
                         cacheDir:(NSString *)cacheDir {
  self = [super init];
  if (self) {
    _viewportWidth     = width;
    _viewportHeight    = height;
    _initialized       = NO;
    _suspended         = NO;
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

    _metalBackend = std::make_unique<reactnativecesium::MetalBackend>();
    _metalBackend->initialize((__bridge void*)layer, width, height);

    [self buildEngine];
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
  _target.requestForceRender();
}

- (void)buildEngine {
  _engine.reset();
  _engine = std::make_unique<reactnativecesium::CesiumEngine>();
  _engine->initialize(_config);

  // Seed the demand target from the engine's initial camera so the first
  // frame is not a "snap from default" jump.
  _target.setAll(_engine->camera().getParams());

  auto* backendPtr = _metalBackend.get();
  _engine->getResourcePreparer()->setGPUTextureCreator(
      [backendPtr](const uint8_t* pixels, int32_t w, int32_t h) -> void* {
        return backendPtr->createRasterTexture(pixels, w, h);
      });
  _engine->getResourcePreparer()->setGPUTextureDeleter(
      [](void* tex) {
        if (tex) CFRelease(tex);
      });
  // Water mask textures reuse the same MTLTexture creator; the binding slot is
  // set at draw time on Metal so no separate factory is needed.
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
  _target.requestForceRender();
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
  _target.requestForceRender();
}

- (void)updateCameraLatitude:(double)lat
                   longitude:(double)lon
                    altitude:(double)alt
                     heading:(double)heading
                       pitch:(double)pitch
                        roll:(double)roll {
  if (!_initialized) return;
  _target.setHpr(lat, lon, alt, heading, pitch, roll);
}

- (void)updateCameraQuaternionLatitude:(double)lat
                             longitude:(double)lon
                              altitude:(double)alt
                               heading:(double)heading
                                 pitch:(double)pitch
                                  roll:(double)roll
                       viewCorrectionW:(double)qw
                                     x:(double)qx
                                     y:(double)qy
                                     z:(double)qz {
  if (!_initialized) return;
  _target.setHpr(lat, lon, alt, heading, pitch, roll);
  const double ql2 = qw * qw + qx * qx + qy * qy + qz * qz;
  glm::dquat q;
  if (ql2 < 1e-20) {
    q = glm::dquat(1.0, 0.0, 0.0, 0.0);
  } else {
    const double inv = 1.0 / std::sqrt(ql2);
    q = glm::dquat(qw * inv, qx * inv, qy * inv, qz * inv);
  }
  _target.setViewCorrection(q);
}

- (void)resize:(int)width height:(int)height {
  _viewportWidth  = width;
  _viewportHeight = height;
  if (_metalBackend) _metalBackend->resize(width, height);
  _target.requestForceRender();
}

- (void)setVerticalFovDeg:(double)degrees {
  if (!_initialized) return;
  _engine->camera().setVerticalFovDegrees(degrees);
  _target.requestForceRender();
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

- (void)setMsaaSampleCount:(int)samples {
  if (_metalBackend) _metalBackend->setMsaaSampleCount(samples);
  _target.requestForceRender();
}

- (void)markNeedsRender { _target.requestForceRender(); }

- (BOOL)shouldRenderNextFrame {
  if (!_initialized || _suspended || !_engine) return NO;
  if (_target.consumeForceRender()) {
    _target.requestForceRender(); // sticky for one frame so we make it through
    return YES;
  }
  if (_frameResult.tilesLoading > 0 || !_frameResult.tilesetActive) return YES;
  if (_target.isDirty()) {
    const auto cur = _engine->camera().getParams();
    const auto tgt = _target.snapshot();
    if (reactnativecesium::CameraTargetState::deltaExceedsEpsilon(cur, tgt)) {
      return YES;
    }
    _target.clearDirty();
  }
  return NO;
}

- (void)renderFrameWithDt:(double)dt {
  if (!_initialized || _suspended) return;

  @autoreleasepool {
    (void)_target.consumeForceRender();

    const auto cur    = _engine->camera().getParams();
    const auto tgt    = _target.snapshot();
    const auto smooth = reactnativecesium::CameraSmoother::step(cur, tgt, dt);
    _engine->camera().setParams(smooth);

    if (!reactnativecesium::CameraTargetState::deltaExceedsEpsilon(smooth, tgt)) {
      _target.clearDirty();
    }

    _engine->updateFrame(_viewportWidth, _viewportHeight, _frameResult);
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

- (double)readCameraLatitude  { return _engine ? _engine->camera().getParams().latitude  : 0.0; }
- (double)readCameraLongitude { return _engine ? _engine->camera().getParams().longitude : 0.0; }
- (double)readCameraAltitude  { return _engine ? _engine->camera().getParams().altitude  : 0.0; }
- (double)readCameraHeading   { return _engine ? _engine->camera().getParams().heading   : 0.0; }
- (double)readCameraPitch     { return _engine ? _engine->camera().getParams().pitch     : 0.0; }
- (double)readCameraRoll      { return _engine ? _engine->camera().getParams().roll      : 0.0; }
- (double)readVerticalFovDeg  { return _engine ? _engine->camera().getVerticalFovDegrees() : 60.0; }

- (double)readViewCorrectionW {
  if (!_engine) return 1.0;
  return _engine->camera().getParams().viewCorrection.w;
}
- (double)readViewCorrectionX {
  return _engine ? _engine->camera().getParams().viewCorrection.x : 0.0;
}
- (double)readViewCorrectionY {
  return _engine ? _engine->camera().getParams().viewCorrection.y : 0.0;
}
- (double)readViewCorrectionZ {
  return _engine ? _engine->camera().getParams().viewCorrection.z : 0.0;
}

- (void)shutdown {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  if (_engine) _engine->shutdown();
  if (_metalBackend) _metalBackend->shutdown();
  _initialized = NO;
}

@end
