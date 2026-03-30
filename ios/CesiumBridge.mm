#import "CesiumBridge.h"

#include "engine/CesiumEngine.hpp"
#include "gestures/GestureHandler.hpp"
#include "metal/MetalBackend.h"

#include <atomic>
#include <memory>
#include <string>

@implementation CesiumBridge {
  std::unique_ptr<reactnativecesium::MetalBackend>   _metalBackend;
  std::unique_ptr<reactnativecesium::CesiumEngine>   _engine;
  std::unique_ptr<reactnativecesium::GestureHandler> _gestureHandler;
  // Persistent frame result — cleared each frame without freeing vector
  // capacity, so after the first warm-up frame no heap allocation occurs.
  reactnativecesium::FrameResult                     _frameResult;
  int   _viewportWidth;
  int   _viewportHeight;
  BOOL  _initialized;

  // Stored init params (used by buildEngine to reconstruct the engine).
  NSString* _cacheDir;
  NSString* _ionAccessToken;
  int64_t   _ionTilesetAssetId;

  // Aircraft joystick rates (degrees/second), written from JS thread,
  // read from CADisplayLink thread. Use atomic for safety.
  std::atomic<double> _pitchRate;
  std::atomic<double> _rollRate;
}

- (instancetype)initWithMetalLayer:(CAMetalLayer *)layer
                             width:(int)width
                            height:(int)height
                         cacheDir:(NSString *)cacheDir {
  self = [super init];
  if (self) {
    _viewportWidth      = width;
    _viewportHeight     = height;
    _initialized        = NO;
    _cacheDir           = [cacheDir copy];
    _ionAccessToken     = nil;
    _ionTilesetAssetId  = 1;
    _pitchRate.store(0.0);
    _rollRate.store(0.0);

    _metalBackend = std::make_unique<reactnativecesium::MetalBackend>();
    _metalBackend->initialize((__bridge void*)layer, width, height);

    _gestureHandler = std::make_unique<reactnativecesium::GestureHandler>();
    _gestureHandler->setViewportSize(width, height);

    [self buildEngine];
    _initialized = YES;
  }
  return self;
}

// Tear down and rebuild _engine using the currently stored config.
// The destructor calls shutdown() → destroyTileset() → waitUntilIdle().
// Call on the main thread only.
- (void)buildEngine {
  _engine.reset();
  _engine = std::make_unique<reactnativecesium::CesiumEngine>();

  reactnativecesium::EngineConfig config;
  if (_ionAccessToken) {
    config.ionAccessToken = std::string([_ionAccessToken UTF8String]);
  }
  config.ionAssetId        = _ionTilesetAssetId;
  config.cacheDatabasePath = _cacheDir
      ? std::string([_cacheDir UTF8String]) + "/cesium_cache.db"
      : "";

  _engine->initialize(*_metalBackend, config);

  auto* backendPtr = _metalBackend.get();
  _engine->getResourcePreparer()->setMetalTextureCreator(
      [backendPtr](const uint8_t* pixels, int32_t w, int32_t h) -> void* {
        return backendPtr->createRasterTexture(pixels, w, h);
      });
}

- (void)updateIonAccessToken:(NSString *)token assetId:(int64_t)assetId {
  if (!_initialized) return;
  _ionAccessToken    = [token copy];
  _ionTilesetAssetId = assetId;
  reactnativecesium::EngineConfig config;
  config.ionAccessToken = token ? std::string([token UTF8String]) : "";
  config.ionAssetId     = assetId;
  _engine->updateConfig(config);
}

- (void)updateImageryAssetId:(int64_t)assetId {
  if (!_initialized) return;
  _engine->setImageryAssetId(assetId);
}

- (void)updateCameraLatitude:(double)lat
                   longitude:(double)lon
                    altitude:(double)alt
                     heading:(double)heading
                       pitch:(double)pitch
                        roll:(double)roll {
  if (!_initialized) return;
  reactnativecesium::CameraParams params;
  params.latitude  = lat;
  params.longitude = lon;
  params.altitude  = alt;
  params.heading   = heading;
  params.pitch     = pitch;
  params.roll      = roll;
  _engine->camera().setParams(params);
}

- (void)setDebugOverlay:(BOOL)__unused enabled { }

- (void)resize:(int)width height:(int)height {
  _viewportWidth  = width;
  _viewportHeight = height;
  if (_metalBackend) _metalBackend->resize(width, height);
  if (_gestureHandler) _gestureHandler->setViewportSize(width, height);
}

- (void)setJoystickPitchRate:(double)pitchRate rollRate:(double)rollRate {
  _pitchRate.store(pitchRate);
  _rollRate.store(rollRate);
}

- (void)renderFrameWithDt:(double)dt {
  if (!_initialized) return;

  @autoreleasepool {
    // Apply gesture-based camera changes accumulated since last frame.
    _gestureHandler->applyToCamera(_engine->camera());
    _gestureHandler->resetDeltas();

    // Apply aircraft joystick rates using real frame duration so movement
    // stays consistent at both 30 Hz and 60 Hz (and ProMotion 120 Hz).
    const double pr = _pitchRate.load();
    const double rr = _rollRate.load();
    if (pr != 0.0 || rr != 0.0) {
      auto p = _engine->camera().getParams();
      p.pitch += pr * dt;
      p.roll  += rr * dt;
      while (p.pitch >  180.0) p.pitch -= 360.0;
      while (p.pitch < -180.0) p.pitch += 360.0;
      while (p.roll  >  180.0) p.roll  -= 360.0;
      while (p.roll  < -180.0) p.roll  += 360.0;
      _engine->camera().setParams(p);
    }

    // Fill the persistent _frameResult (vectors cleared internally; capacity
    // is preserved across frames to avoid per-frame heap allocation).
    _engine->updateFrame(_viewportWidth, _viewportHeight, _frameResult);

    static int logFrame = 0;
    if (++logFrame % 120 == 0) {
      NSLog(@"[CesiumBridge] draws=%zu verts=%zu indices=%zu loading=%d lat=%.4f lon=%.4f alt=%.0f",
            _frameResult.draws.size(),
            _frameResult.eyeRelPositions.size() / 3,
            _frameResult.indices.size(),
            _frameResult.tilesLoading,
            _frameResult.cameraLat, _frameResult.cameraLon, _frameResult.cameraAlt);
    }

    reactnativecesium::FrameParams params;
    _metalBackend->beginFrame(params);
    _metalBackend->drawScene(_frameResult);
    _metalBackend->endFrame();
  }
}

- (void)shutdown {
  if (_engine) _engine->shutdown();
  if (_metalBackend) _metalBackend->shutdown();
  _initialized = NO;
}

- (void)onTouchDown:(int)pointerId x:(float)x y:(float)y {
  if (_gestureHandler) _gestureHandler->onTouchDown(pointerId, x, y);
}

- (void)onTouchMove:(int)pointerId x:(float)x y:(float)y {
  if (_gestureHandler) _gestureHandler->onTouchMove(pointerId, x, y);
}

- (void)onTouchUp:(int)pointerId {
  if (_gestureHandler) _gestureHandler->onTouchUp(pointerId);
}

- (void)setViewportSize:(float)width height:(float)height {
  if (_gestureHandler) _gestureHandler->setViewportSize(width, height);
}

@end
