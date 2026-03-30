#import "CesiumBridge.h"

#include "engine/CesiumEngine.hpp"
#include "gestures/GestureHandler.hpp"
#include "metal/MetalBackend.h"

#include <atomic>
#include <cmath>
#include <memory>
#include <string>

namespace {

double smoothstep01(double t) {
  if (t <= 0.0) return 0.0;
  if (t >= 1.0) return 1.0;
  return t * t * (3.0 - 2.0 * t);
}

double lerp(double a, double b, double t) { return a + (b - a) * t; }

double lerpAngleDeg(double a, double b, double t) {
  double diff = std::fmod(b - a + 540.0, 360.0) - 180.0;
  return a + diff * t;
}

NSString* stripHtmlToPlain(NSString* html) {
  if (html.length == 0) return @"";
  NSError* err = nil;
  NSRegularExpression* rx = [NSRegularExpression regularExpressionWithPattern:@"<[^>]+>"
                                                                        options:0
                                                                          error:&err];
  NSString* plain =
      [rx stringByReplacingMatchesInString:html
                                   options:0
                                     range:NSMakeRange(0, html.length)
                              withTemplate:@" "];
  NSRegularExpression* wsRx =
      [NSRegularExpression regularExpressionWithPattern:@"\\s+"
                                                options:0
                                                  error:&err];
  plain = [wsRx stringByReplacingMatchesInString:plain
                                         options:0
                                           range:NSMakeRange(0, plain.length)
                                    withTemplate:@" "];
  NSCharacterSet* ws = [NSCharacterSet whitespaceAndNewlineCharacterSet];
  return [plain stringByTrimmingCharactersInSet:ws];
}

struct CamAnim {
  reactnativecesium::CameraParams start{};
  reactnativecesium::CameraParams end{};
  double                   duration = 0.0;
  double                   elapsed  = 0.0;
  bool                     active   = false;
  enum { FlyTo, LookAt } mode = FlyTo;
};

} // namespace

@implementation CesiumBridge {
  std::unique_ptr<reactnativecesium::MetalBackend>   _metalBackend;
  std::unique_ptr<reactnativecesium::CesiumEngine>   _engine;
  std::unique_ptr<reactnativecesium::GestureHandler> _gestureHandler;
  reactnativecesium::FrameResult                     _frameResult;
  int   _viewportWidth;
  int   _viewportHeight;
  BOOL  _initialized;

  NSString* _cacheDir;
  NSString* _ionAccessToken;
  int64_t   _ionTilesetAssetId;

  double _maxSSE;
  double _maxSimLoads;
  double _loadDescLim;

  std::atomic<double> _pitchRate;
  std::atomic<double> _rollRate;

  BOOL _debugOverlay;

  CamAnim _camAnim;

  double _fpsEma;
  int    _metricsTick;

  double _metricsFps;
  NSInteger _metricsTilesRendered;
  NSInteger _metricsTilesLoading;
  NSInteger _metricsTilesVisited;
  BOOL _metricsIonTokenConfigured;
  BOOL _metricsTilesetReady;
  NSString* _metricsCreditsPlainText;
}

- (reactnativecesium::EngineConfig)makeEngineConfig {
  reactnativecesium::EngineConfig config;
  if (_ionAccessToken) {
    config.ionAccessToken = std::string([_ionAccessToken UTF8String]);
  }
  config.ionAssetId = _ionTilesetAssetId;
  config.cacheDatabasePath =
      _cacheDir ? std::string([_cacheDir UTF8String]) + "/cesium_cache.db" : "";
  config.maximumScreenSpaceError = std::max(1.0, _maxSSE);
  config.maximumSimultaneousTileLoads =
      static_cast<int32_t>(std::max(1.0, std::round(_maxSimLoads)));
  config.loadingDescendantLimit =
      static_cast<int32_t>(std::max(1.0, std::round(_loadDescLim)));
  return config;
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
    _cacheDir          = [cacheDir copy];
    _ionAccessToken    = nil;
    _ionTilesetAssetId = 1;
    _maxSSE            = 32.0;
    _maxSimLoads       = 12.0;
    _loadDescLim       = 20.0;
    _pitchRate.store(0.0);
    _rollRate.store(0.0);
    _debugOverlay      = NO;
    _fpsEma            = 0.0;
    _metricsTick       = 0;
    _metricsFps        = 0.0;
    _metricsTilesRendered = 0;
    _metricsTilesLoading  = 0;
    _metricsTilesVisited  = 0;
    _metricsIonTokenConfigured = NO;
    _metricsTilesetReady       = NO;
    _metricsCreditsPlainText   = @"";

    _metalBackend = std::make_unique<reactnativecesium::MetalBackend>();
    _metalBackend->initialize((__bridge void*)layer, width, height);

    _gestureHandler = std::make_unique<reactnativecesium::GestureHandler>();
    _gestureHandler->setViewportSize(static_cast<float>(width),
                                     static_cast<float>(height));

    [self buildEngine];
    _initialized = YES;
  }
  return self;
}

- (void)buildEngine {
  _engine.reset();
  _engine = std::make_unique<reactnativecesium::CesiumEngine>();
  _engine->initialize(*_metalBackend, [self makeEngineConfig]);

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
  _camAnim.active    = false;
  _engine->updateConfig([self makeEngineConfig]);
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
  if (!_initialized || _camAnim.active) return;
  reactnativecesium::CameraParams params;
  params.latitude  = lat;
  params.longitude = lon;
  params.altitude  = alt;
  params.heading   = heading;
  params.pitch     = pitch;
  params.roll      = roll;
  _engine->camera().setParams(params);
}

- (void)setDebugOverlay:(BOOL)enabled {
  _debugOverlay = enabled;
  if (!enabled) self.debugOverlayText = nil;
}

- (void)resize:(int)width height:(int)height {
  _viewportWidth  = width;
  _viewportHeight = height;
  if (_metalBackend) _metalBackend->resize(width, height);
  if (_gestureHandler) {
    _gestureHandler->setViewportSize(static_cast<float>(width),
                                     static_cast<float>(height));
  }
}

- (void)setJoystickPitchRate:(double)pitchRate rollRate:(double)rollRate {
  _pitchRate.store(pitchRate);
  _rollRate.store(rollRate);
}

- (void)setVerticalFovDeg:(double)degrees {
  if (!_initialized) return;
  _engine->camera().setVerticalFovDegrees(degrees);
}

- (void)setGesturePanEnabled:(BOOL)enabled {
  if (_gestureHandler) _gestureHandler->setPanEnabled(enabled);
}
- (void)setGesturePinchZoomEnabled:(BOOL)enabled {
  if (_gestureHandler) _gestureHandler->setPinchZoomEnabled(enabled);
}
- (void)setGesturePinchRotateEnabled:(BOOL)enabled {
  if (_gestureHandler) _gestureHandler->setPinchRotateEnabled(enabled);
}
- (void)setGesturePanSensitivity:(double)s {
  if (_gestureHandler) _gestureHandler->setPanSensitivity(s);
}
- (void)setGesturePinchSensitivity:(double)s {
  if (_gestureHandler) _gestureHandler->setPinchSensitivity(s);
}

- (void)setMaximumScreenSpaceError:(double)v {
  _maxSSE = v;
  if (!_initialized) return;
  _engine->updateConfig([self makeEngineConfig]);
}
- (void)setMaximumSimultaneousTileLoads:(int)v {
  _maxSimLoads = static_cast<double>(v);
  if (!_initialized) return;
  _engine->updateConfig([self makeEngineConfig]);
}
- (void)setLoadingDescendantLimit:(int)v {
  _loadDescLim = static_cast<double>(v);
  if (!_initialized) return;
  _engine->updateConfig([self makeEngineConfig]);
}

- (void)setMsaaSampleCount:(int)samples {
  if (_metalBackend) _metalBackend->setMsaaSampleCount(samples);
}

- (void)stepCameraAnimation:(double)dt {
  if (!_camAnim.active || !_engine) return;
  _camAnim.elapsed += dt;
  double u = smoothstep01(_camAnim.duration > 1e-6 ? _camAnim.elapsed / _camAnim.duration : 1.0);
  reactnativecesium::CameraParams p;
  if (_camAnim.mode == CamAnim::FlyTo) {
    p.latitude  = lerp(_camAnim.start.latitude, _camAnim.end.latitude, u);
    p.longitude = lerp(_camAnim.start.longitude, _camAnim.end.longitude, u);
    p.altitude  = lerp(_camAnim.start.altitude, _camAnim.end.altitude, u);
    p.heading   = lerpAngleDeg(_camAnim.start.heading, _camAnim.end.heading, u);
    p.pitch     = lerpAngleDeg(_camAnim.start.pitch, _camAnim.end.pitch, u);
    p.roll      = lerpAngleDeg(_camAnim.start.roll, _camAnim.end.roll, u);
  } else {
    p           = _camAnim.start;
    p.heading   = lerpAngleDeg(_camAnim.start.heading, _camAnim.end.heading, u);
    p.pitch     = lerpAngleDeg(_camAnim.start.pitch, _camAnim.end.pitch, u);
  }
  _engine->camera().setParams(p);
  if (_camAnim.elapsed >= _camAnim.duration) {
    _camAnim.active = false;
  }
}

- (void)flyToLatitude:(double)lat
            longitude:(double)lon
             altitude:(double)alt
              heading:(double)heading
                pitch:(double)pitch
                 roll:(double)roll
     durationSeconds:(double)duration {
  if (!_initialized) return;
  _camAnim.start = _engine->camera().getParams();
  _camAnim.end.latitude  = lat;
  _camAnim.end.longitude = lon;
  _camAnim.end.altitude  = alt;
  _camAnim.end.heading   = heading;
  _camAnim.end.pitch     = pitch;
  _camAnim.end.roll      = roll;
  _camAnim.duration      = std::max(0.01, duration);
  _camAnim.elapsed       = 0.0;
  _camAnim.mode          = CamAnim::FlyTo;
  _camAnim.active        = true;
}

- (void)lookAtTargetLatitude:(double)lat
                   longitude:(double)lon
                    altitude:(double)alt
            durationSeconds:(double)duration {
  if (!_initialized) return;
  _camAnim.start = _engine->camera().getParams();
  double eh = 0, ep = 0;
  _engine->camera().computeHeadingPitchToward(lat, lon, alt, eh, ep);
  _camAnim.end          = _camAnim.start;
  _camAnim.end.heading  = eh;
  _camAnim.end.pitch    = ep;
  _camAnim.duration     = std::max(0.01, duration);
  _camAnim.elapsed      = 0.0;
  _camAnim.mode         = CamAnim::LookAt;
  _camAnim.active       = true;
}

- (void)cancelCameraAnimation {
  _camAnim.active = false;
}

- (void)renderFrameWithDt:(double)dt {
  if (!_initialized) return;

  @autoreleasepool {
    if (_camAnim.active) {
      [self stepCameraAnimation:dt];
    } else {
      _gestureHandler->applyToCamera(_engine->camera());
      const double pr = _pitchRate.load();
      const double rr = _rollRate.load();
      if (pr != 0.0 || rr != 0.0) {
        auto p = _engine->camera().getParams();
        p.pitch += pr * dt;
        p.roll += rr * dt;
        while (p.pitch > 180.0) p.pitch -= 360.0;
        while (p.pitch < -180.0) p.pitch += 360.0;
        while (p.roll > 180.0) p.roll -= 360.0;
        while (p.roll < -180.0) p.roll += 360.0;
        _engine->camera().setParams(p);
      }
    }
    _gestureHandler->resetDeltas();

    _engine->updateFrame(_viewportWidth, _viewportHeight, _frameResult);

    if (_debugOverlay) {
      self.debugOverlayText = [NSString
          stringWithFormat:
              @"tiles=%d loadQ=%d visited=%d\nion=%@ ts=%@ fov=%.1f°\n"
              @"lat=%.5f lon=%.5f alt=%.0f\nhdg=%.1f pitch=%.1f roll=%.1f",
              _frameResult.tilesRendered, _frameResult.tilesLoading,
              _frameResult.tilesVisited,
              _frameResult.ionTokenConfigured ? @"yes" : @"no",
              _frameResult.tilesetActive ? @"yes" : @"no", _frameResult.verticalFovDeg,
              _frameResult.cameraLat, _frameResult.cameraLon, _frameResult.cameraAlt,
              _engine->camera().getParams().heading, _engine->camera().getParams().pitch,
              _engine->camera().getParams().roll];
    }

    NSMutableString* credits = [NSMutableString string];
    for (const auto& html : _frameResult.creditHtmlLines) {
      NSString* chunk = stripHtmlToPlain(
          [NSString stringWithUTF8String:html.c_str()]);
      if (chunk.length == 0) continue;
      if (credits.length > 0) [credits appendString:@" · "];
      [credits appendString:chunk];
    }
    self.creditsPlainText = credits.length > 0 ? [credits copy] : @"";

    const double instFps = (dt > 1e-6) ? (1.0 / dt) : 0.0;
    _fpsEma = (_fpsEma <= 1e-6) ? instFps : (_fpsEma * 0.85 + instFps * 0.15);

    if (++_metricsTick >= 20) {
      _metricsTick               = 0;
      _metricsFps                = _fpsEma;
      _metricsTilesRendered      = _frameResult.tilesRendered;
      _metricsTilesLoading       = _frameResult.tilesLoading;
      _metricsTilesVisited       = _frameResult.tilesVisited;
      _metricsIonTokenConfigured = _frameResult.ionTokenConfigured;
      _metricsTilesetReady       = _frameResult.tilesetActive;
      _metricsCreditsPlainText   = self.creditsPlainText ?: @"";
    }

    reactnativecesium::FrameParams params;
    _metalBackend->beginFrame(params);
    _metalBackend->drawScene(_frameResult);
    _metalBackend->endFrame();
  }
}

- (double)metricsFps {
  return _metricsFps;
}
- (NSInteger)metricsTilesRendered {
  return _metricsTilesRendered;
}
- (NSInteger)metricsTilesLoading {
  return _metricsTilesLoading;
}
- (NSInteger)metricsTilesVisited {
  return _metricsTilesVisited;
}
- (BOOL)metricsIonTokenConfigured {
  return _metricsIonTokenConfigured;
}
- (BOOL)metricsTilesetReady {
  return _metricsTilesetReady;
}
- (NSString*)metricsCreditsPlainText {
  return _metricsCreditsPlainText ?: @"";
}

- (double)readCameraLatitude {
  return _engine ? _engine->camera().getParams().latitude : 0.0;
}
- (double)readCameraLongitude {
  return _engine ? _engine->camera().getParams().longitude : 0.0;
}
- (double)readCameraAltitude {
  return _engine ? _engine->camera().getParams().altitude : 0.0;
}
- (double)readCameraHeading {
  return _engine ? _engine->camera().getParams().heading : 0.0;
}
- (double)readCameraPitch {
  return _engine ? _engine->camera().getParams().pitch : 0.0;
}
- (double)readCameraRoll {
  return _engine ? _engine->camera().getParams().roll : 0.0;
}
- (double)readVerticalFovDeg {
  return _engine ? _engine->camera().getVerticalFovDegrees() : 60.0;
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
