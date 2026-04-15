#import "CesiumBridge.h"
#import <CoreFoundation/CoreFoundation.h>

#include "engine/CesiumEngine.hpp"
#include "metal/MetalBackend.h"

#include <atomic>
#include <cmath>
#include <memory>
#include <string>

namespace {

// Strips HTML tags and collapses whitespace to produce plain-text attribution.
static NSString* stripHtmlToPlain(NSString* html) {
  if (html.length == 0) return @"";
  NSError* err = nil;
  NSRegularExpression* tagRx =
      [NSRegularExpression regularExpressionWithPattern:@"<[^>]+>" options:0 error:&err];
  NSString* plain = [tagRx stringByReplacingMatchesInString:html
                                                    options:0
                                                      range:NSMakeRange(0, html.length)
                                               withTemplate:@" "];
  NSRegularExpression* wsRx =
      [NSRegularExpression regularExpressionWithPattern:@"\\s+" options:0 error:&err];
  plain = [wsRx stringByReplacingMatchesInString:plain
                                         options:0
                                           range:NSMakeRange(0, plain.length)
                                    withTemplate:@" "];
  return [plain stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
}

double lerpAngleDeg(double a, double b, double t) {
  double diff = std::fmod(b - a + 540.0, 360.0) - 180.0;
  return a + diff * t;
}

double angleDeltaAbsDeg(double a, double b) {
  return std::abs(std::fmod(b - a + 540.0, 360.0) - 180.0);
}

} // namespace

// ── Per-DOF smoothing. ────────────────────────────────────────────────────────
static const double kSmoothAlt = 25.0;
static const double kSmoothHdg = 30.0;
static const double kSmoothRoll = 50.0;
static const double kSmoothPitch = 50.0;
static const double kEpsLatLon = 1e-7;
static const double kEpsAlt = 0.1;
static const double kEpsAngleDeg = 0.05;

@implementation CesiumBridge {
  std::unique_ptr<reactnativecesium::MetalBackend> _metalBackend;
  std::unique_ptr<reactnativecesium::CesiumEngine> _engine;
  reactnativecesium::FrameResult                   _frameResult;
  int   _viewportWidth;
  int   _viewportHeight;
  BOOL  _initialized;

  NSString* _cacheDir;
  NSString* _ionAccessToken;
  int64_t   _ionTilesetAssetId;

  double _maxSSE;
  double _maxSimLoads;
  double _loadDescLim;

  // Demand target — incoming props/calls write here; render loop smooths toward it.
  reactnativecesium::CameraParams _camTarget;
  BOOL _forceRenderNextFrame;

  BOOL _suspended;

  double _fpsEma;
  int    _metricsTick;

  double    _metricsFps;
  NSInteger _metricsTilesRendered;
  NSInteger _metricsTilesLoading;
  NSInteger _metricsTilesVisited;
  BOOL      _metricsIonTokenConfigured;
  BOOL      _metricsTilesetReady;
  NSString* _metricsCreditsPlainText;

  NSString* _lastCreditHtmlJoined;
}

- (reactnativecesium::EngineConfig)makeEngineConfig {
  reactnativecesium::EngineConfig config;
  if (_ionAccessToken) {
    config.ionAccessToken = std::string([_ionAccessToken UTF8String]);
  }
  config.ionAssetId = _ionTilesetAssetId;
  config.cacheDatabasePath =
      _cacheDir ? std::string([_cacheDir UTF8String]) + "/cesium_cache.db" : "";
  NSString* caPem = [[NSBundle mainBundle] pathForResource:@"cacert" ofType:@"pem"];
  if (caPem.length > 0) {
    config.tlsCaBundlePath = std::string([caPem fileSystemRepresentation]);
  }
  config.maximumScreenSpaceError = std::max(1.0, _maxSSE);
  config.maximumSimultaneousTileLoads =
      static_cast<int32_t>(std::max(0.0, std::round(_maxSimLoads)));
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
    _forceRenderNextFrame = YES;
    _suspended         = NO;
    _fpsEma            = 0.0;
    _metricsTick       = 0;
    _metricsFps        = 0.0;
    _metricsTilesRendered = 0;
    _metricsTilesLoading  = 0;
    _metricsTilesVisited  = 0;
    _metricsIonTokenConfigured = NO;
    _metricsTilesetReady       = NO;
    _metricsCreditsPlainText   = @"";
    _lastCreditHtmlJoined       = @"";

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
  _forceRenderNextFrame = YES;
}

- (void)buildEngine {
  reactnativecesium::EngineConfig cfg = [self makeEngineConfig];
  static std::atomic<int> caWarned{0};
  if (cfg.tlsCaBundlePath.empty() && caWarned.fetch_add(1) == 0) {
    NSLog(@"[ReactNativeCesium] cacert.pem not in main bundle — libcurl may fail TLS to "
          @"api.cesium.com on device. Ensure the pod includes ios/cacert.pem (run pod install).");
  }
  _engine.reset();
  _engine = std::make_unique<reactnativecesium::CesiumEngine>();
  _engine->initialize(cfg);

  // Sync demand target with engine's initial camera defaults.
  _camTarget = _engine->camera().getParams();

  auto* backendPtr = _metalBackend.get();
  _engine->getResourcePreparer()->setGPUTextureCreator(
      [backendPtr](const uint8_t* pixels, int32_t w, int32_t h) -> void* {
        return backendPtr->createRasterTexture(pixels, w, h);
      });
  _engine->getResourcePreparer()->setGPUTextureDeleter(
      [](void* tex) {
        if (tex) CFRelease(tex);
      });
  // Water mask textures are identical MTLTexture objects — createRasterTexture
  // is reused.  On Metal, the binding slot (texture index 1) is set at draw
  // time so no separate factory is needed.
  _engine->getResourcePreparer()->setWaterMaskTextureCreator(
      [backendPtr](const uint8_t* pixels, int32_t w, int32_t h) -> void* {
        return backendPtr->createRasterTexture(pixels, w, h);
      });
  _engine->getResourcePreparer()->setWaterMaskTextureDeleter(
      [](void* tex) {
        if (tex) CFRelease(tex);
      });
}

- (void)updateIonAccessToken:(NSString *)token assetId:(int64_t)assetId {
  if (!_initialized) return;
  _ionAccessToken    = [token copy];
  _ionTilesetAssetId = assetId;
  _engine->updateConfig([self makeEngineConfig]);
  _forceRenderNextFrame = YES;
}

- (void)updateImageryAssetId:(int64_t)assetId {
  if (!_initialized) return;
  _engine->setImageryAssetId(assetId);
  _forceRenderNextFrame = YES;
}

- (void)updateCameraLatitude:(double)lat
                   longitude:(double)lon
                    altitude:(double)alt
                     heading:(double)heading
                       pitch:(double)pitch
                        roll:(double)roll {
  if (!_initialized) return;
  _camTarget.latitude  = lat;
  _camTarget.longitude = lon;
  _camTarget.altitude  = alt;
  _camTarget.heading   = heading;
  _camTarget.pitch     = pitch;
  _camTarget.roll      = roll;
  _forceRenderNextFrame = YES;
}

- (void)resize:(int)width height:(int)height {
  _viewportWidth  = width;
  _viewportHeight = height;
  if (_metalBackend) _metalBackend->resize(width, height);
  _forceRenderNextFrame = YES;
}

- (void)setVerticalFovDeg:(double)degrees {
  if (!_initialized) return;
  _engine->camera().setVerticalFovDegrees(degrees);
  _forceRenderNextFrame = YES;
}

- (void)setMaximumScreenSpaceError:(double)v {
  _maxSSE = v;
  if (!_initialized) return;
  _engine->updateConfig([self makeEngineConfig]);
  _forceRenderNextFrame = YES;
}
- (void)setMaximumSimultaneousTileLoads:(int)v {
  _maxSimLoads = static_cast<double>(v);
  if (!_initialized) return;
  _engine->updateConfig([self makeEngineConfig]);
  _forceRenderNextFrame = YES;
}
- (void)setLoadingDescendantLimit:(int)v {
  _loadDescLim = static_cast<double>(v);
  if (!_initialized) return;
  _engine->updateConfig([self makeEngineConfig]);
  _forceRenderNextFrame = YES;
}

- (void)setMsaaSampleCount:(int)samples {
  if (_metalBackend) _metalBackend->setMsaaSampleCount(samples);
  _forceRenderNextFrame = YES;
}

- (void)markNeedsRender {
  _forceRenderNextFrame = YES;
}

- (BOOL)shouldRenderNextFrame {
  if (!_initialized || _suspended || !_engine) {
    return NO;
  }

  const auto cur = _engine->camera().getParams();
  const bool cameraDirty =
      std::abs(_camTarget.latitude - cur.latitude) > kEpsLatLon ||
      std::abs(_camTarget.longitude - cur.longitude) > kEpsLatLon ||
      std::abs(_camTarget.altitude - cur.altitude) > kEpsAlt ||
      angleDeltaAbsDeg(cur.heading, _camTarget.heading) > kEpsAngleDeg ||
      angleDeltaAbsDeg(cur.pitch, _camTarget.pitch) > kEpsAngleDeg ||
      angleDeltaAbsDeg(cur.roll, _camTarget.roll) > kEpsAngleDeg;

  return _forceRenderNextFrame ||
         _frameResult.tilesLoading > 0 ||
         !_frameResult.tilesetActive ||
         cameraDirty;
}

- (void)renderFrameWithDt:(double)dt {
  if (!_initialized || _suspended) return;

  @autoreleasepool {
    auto cur = _engine->camera().getParams();

    // ── Smooth follower: track _camTarget per DOF ────────────────────────────
    // lat/lon: direct copy — pan gestures must feel instant.
    cur.latitude  = _camTarget.latitude;
    cur.longitude = _camTarget.longitude;
    // alt/hdg/roll/pitch: exponential decay toward target.
    const double aAlt = 1.0 - std::exp(-kSmoothAlt * dt);
    const double aHdg = 1.0 - std::exp(-kSmoothHdg * dt);
    const double aPitch = 1.0 - std::exp(-kSmoothPitch * dt);
    const double aRoll = 1.0 - std::exp(-kSmoothRoll * dt);
    cur.altitude = cur.altitude + aAlt * (_camTarget.altitude - cur.altitude);
    cur.heading  = lerpAngleDeg(cur.heading, _camTarget.heading, aHdg);
    cur.pitch  = lerpAngleDeg(cur.pitch, _camTarget.pitch, aPitch);
    cur.roll  = lerpAngleDeg(cur.roll, _camTarget.roll, aRoll);

    if (std::abs(_camTarget.altitude - cur.altitude) <= kEpsAlt) {
      cur.altitude = _camTarget.altitude;
    }
    if (angleDeltaAbsDeg(cur.heading, _camTarget.heading) <= kEpsAngleDeg) {
      cur.heading = _camTarget.heading;
    }
    if (angleDeltaAbsDeg(cur.pitch, _camTarget.pitch) <= kEpsAngleDeg) {
      cur.pitch = _camTarget.pitch;
    }
    if (angleDeltaAbsDeg(cur.roll, _camTarget.roll) <= kEpsAngleDeg) {
      cur.roll = _camTarget.roll;
    }
    _engine->camera().setParams(cur);

    _engine->updateFrame(_viewportWidth, _viewportHeight, _frameResult);

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
      if (!_frameResult.creditHtmlLines.empty()) {
        NSMutableString* joined = [NSMutableString string];
        for (const auto& html : _frameResult.creditHtmlLines) {
          if (joined.length > 0) [joined appendString:@"|"];
          [joined appendString:[NSString stringWithUTF8String:html.c_str()]];
        }
        if (![_lastCreditHtmlJoined isEqualToString:joined]) {
          _lastCreditHtmlJoined = [joined copy];
          NSMutableString* credits = [NSMutableString string];
          for (const auto& html : _frameResult.creditHtmlLines) {
            NSString* chunk = stripHtmlToPlain(
                [NSString stringWithUTF8String:html.c_str()]);
            if (chunk.length == 0) continue;
            if (credits.length > 0) [credits appendString:@" · "];
            [credits appendString:chunk];
          }
          _metricsCreditsPlainText = [credits copy];
        }
      } else {
        _lastCreditHtmlJoined = @"";
        _metricsCreditsPlainText = @"";
      }
    }

    reactnativecesium::FrameParams params;
    _metalBackend->beginFrame(params);
    _metalBackend->drawScene(_frameResult);
    _metalBackend->endFrame();
    _forceRenderNextFrame = NO;
  }
}

- (double)metricsFps              { return _metricsFps; }
- (NSInteger)metricsTilesRendered { return _metricsTilesRendered; }
- (NSInteger)metricsTilesLoading  { return _metricsTilesLoading; }
- (NSInteger)metricsTilesVisited  { return _metricsTilesVisited; }
- (BOOL)metricsIonTokenConfigured { return _metricsIonTokenConfigured; }
- (BOOL)metricsTilesetReady       { return _metricsTilesetReady; }
- (NSString*)metricsCreditsPlainText { return _metricsCreditsPlainText ?: @""; }

- (double)readCameraLatitude  { return _engine ? _engine->camera().getParams().latitude  : 0.0; }
- (double)readCameraLongitude { return _engine ? _engine->camera().getParams().longitude : 0.0; }
- (double)readCameraAltitude  { return _engine ? _engine->camera().getParams().altitude  : 0.0; }
- (double)readCameraHeading   { return _engine ? _engine->camera().getParams().heading   : 0.0; }
- (double)readCameraPitch     { return _engine ? _engine->camera().getParams().pitch     : 0.0; }
- (double)readCameraRoll      { return _engine ? _engine->camera().getParams().roll      : 0.0; }
- (double)readVerticalFovDeg  { return _engine ? _engine->camera().getVerticalFovDegrees() : 60.0; }

- (void)shutdown {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  if (_engine) _engine->shutdown();
  if (_metalBackend) _metalBackend->shutdown();
  _initialized = NO;
}

@end
