#import "CesiumBridge.h"
#import <CoreFoundation/CoreFoundation.h>
#import <Network/Network.h>

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

struct CamAnim {
  reactnativecesium::CameraParams start{};
  reactnativecesium::CameraParams end{};
  double                   duration = 0.0;
  double                   elapsed  = 0.0;
  bool                     active   = false;
  enum { FlyTo, LookAt } mode = FlyTo;
};


} // namespace

// ── Per-DOF smoothing. ────────────────────────────────────────────────────────
static const double kSmoothAlt = 25.0;
static const double kSmoothHdg = 30.0;
static const double kSmoothRoll = 50.0;
static const double kSmoothPitch = 50.0;

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

  BOOL _debugOverlay;
  BOOL _showCredits;

  CamAnim _camAnim;

  // Demand target — incoming props/calls write here; render loop smooths toward it.
  reactnativecesium::CameraParams _camTarget;

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
  double    _metricsTerrainHeight;

  // Network reachability
  nw_path_monitor_t _pathMonitor;
  BOOL              _networkReachable;

  // Estimated terrain altitude (m above WGS84) below the camera, derived from
  // rendered tile vertex altitudes and updated every frame.
  double _terrainHeightBelowCamera;
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
    _debugOverlay      = NO;
    _showCredits       = YES;
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

    [self startNetworkMonitor];
  }
  return self;
}

- (void)appWillResignActive:(NSNotification *)note { _suspended = YES; }
- (void)appDidBecomeActive:(NSNotification *)note  { _suspended = NO;  }

- (void)startNetworkMonitor {
  _networkReachable = YES;
  __weak CesiumBridge *weakSelf = self;
  _pathMonitor = nw_path_monitor_create();
  dispatch_queue_t q = dispatch_queue_create("com.reactnativecesium.netmonitor",
                                              DISPATCH_QUEUE_SERIAL);
  nw_path_monitor_set_queue(_pathMonitor, q);
  nw_path_monitor_set_update_handler(_pathMonitor, ^(nw_path_t path) {
    const BOOL reachable = nw_path_get_status(path) == nw_path_status_satisfied;
    dispatch_async(dispatch_get_main_queue(), ^{
      CesiumBridge *strong = weakSelf;
      if (strong) strong->_networkReachable = reachable;
    });
  });
  nw_path_monitor_start(_pathMonitor);
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
  _engine->initialize(*_metalBackend, cfg);

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
  _camTarget.latitude  = lat;
  _camTarget.longitude = lon;
  _camTarget.altitude  = alt;
  _camTarget.heading   = heading;
  _camTarget.pitch     = pitch;
  _camTarget.roll      = roll;
}

- (void)setDebugOverlay:(BOOL)enabled {
  _debugOverlay = enabled;
  if (!enabled) self.debugOverlayText = nil;
}

- (void)setShowCredits:(BOOL)enabled {
  _showCredits = enabled;
  if (!enabled) _metricsCreditsPlainText = @"";
}

- (void)resize:(int)width height:(int)height {
  _viewportWidth  = width;
  _viewportHeight = height;
  if (_metalBackend) _metalBackend->resize(width, height);
}

- (void)setVerticalFovDeg:(double)degrees {
  if (!_initialized) return;
  _engine->camera().setVerticalFovDegrees(degrees);
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
    p.latitude  = lerp(_camAnim.start.latitude,  _camAnim.end.latitude,  u);
    p.longitude = lerp(_camAnim.start.longitude, _camAnim.end.longitude, u);
    p.altitude  = lerp(_camAnim.start.altitude,  _camAnim.end.altitude,  u);
    p.heading   = lerpAngleDeg(_camAnim.start.heading, _camAnim.end.heading, u);
    p.pitch     = lerpAngleDeg(_camAnim.start.pitch,   _camAnim.end.pitch,   u);
    p.roll      = lerpAngleDeg(_camAnim.start.roll,    _camAnim.end.roll,    u);
  } else {
    p         = _camAnim.start;
    p.heading = lerpAngleDeg(_camAnim.start.heading, _camAnim.end.heading, u);
    p.pitch   = lerpAngleDeg(_camAnim.start.pitch,   _camAnim.end.pitch,   u);
  }
  _engine->camera().setParams(p);
  if (_camAnim.elapsed >= _camAnim.duration) {
    _camAnim.active = false;
    _camTarget = _engine->camera().getParams();
  }
}

- (void)flyToLatitude:(double)lat longitude:(double)lon altitude:(double)alt
              heading:(double)heading pitch:(double)pitch roll:(double)roll
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

- (void)lookAtTargetLatitude:(double)lat longitude:(double)lon altitude:(double)alt
            durationSeconds:(double)duration {
  if (!_initialized) return;
  _camAnim.start = _engine->camera().getParams();
  double eh = 0, ep = 0;
  _engine->camera().computeHeadingPitchToward(lat, lon, alt, eh, ep);
  _camAnim.end         = _camAnim.start;
  _camAnim.end.heading = eh;
  _camAnim.end.pitch   = ep;
  _camAnim.duration    = std::max(0.01, duration);
  _camAnim.elapsed     = 0.0;
  _camAnim.mode        = CamAnim::LookAt;
  _camAnim.active      = true;
}

- (void)cancelCameraAnimation {
  _camAnim.active = false;
  if (_engine) _camTarget = _engine->camera().getParams();
}

- (void)renderFrameWithDt:(double)dt {
  if (!_initialized || _suspended) return;

  @autoreleasepool {
    if (_camAnim.active) {
      [self stepCameraAnimation:dt];
    } else {
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
      cur.roll  = 0;//lerpAngleDeg(cur.roll, _camTarget.roll, aRoll);
      _engine->camera().setParams(cur);
    }

    _engine->updateFrame(_viewportWidth, _viewportHeight, _frameResult);

    {
      // Compute terrain height at the camera's nadir (lat/lon directly below).
      // Approach: project each tile vertex onto the camera's nadir axis (unit
      // vector from Earth centre through the camera).  Vertices whose horizontal
      // distance from that axis is < camAlt/5 are "directly below" the camera;
      // we take the maximum altitude among those.  This is the terrain height
      // at the camera's lat/lon, not an average over the whole scene.
      //
      // We use isEllipsoidFallback (not hasUVs) to skip the synthetic flat
      // globe, so terrain-only tiles (no imagery UVs) are still sampled.
      {
        const float camAlt = static_cast<float>(std::max(10.0, _frameResult.cameraAlt));

        // Nadir unit vector (away from Earth centre, parallel to camera's "up").
        const float ex  = _frameResult.cameraEcef.x;
        const float ey  = _frameResult.cameraEcef.y;
        const float ez  = _frameResult.cameraEcef.z;
        const float elen = sqrtf(ex*ex + ey*ey + ez*ez);
        const float nx  = (elen > 0) ? ex/elen : 0.f;
        const float ny  = (elen > 0) ? ey/elen : 0.f;
        const float nz  = (elen > 0) ? ez/elen : 0.f;

        // Accept vertices within camAlt/5 horizontal metres of the nadir ray.
        const float horizThresh  = camAlt / 5.0f;
        const float horizThreshSq = horizThresh * horizThresh;

        float maxNadirAlt = -1e9f;
        float closestAlt  =  0.0f;
        float minDistSq   =  FLT_MAX;
        bool  anyNadir    = false;

        for (const auto& draw : _frameResult.draws) {
          if (draw.isEllipsoidFallback) continue;
          const uint32_t first = draw.indexByteOffset / sizeof(uint32_t);
          const uint32_t step  = std::max(1u, draw.indexCount / 30u);
          for (uint32_t j = 0; j < draw.indexCount; j += step) {
            const uint32_t idx = first + j;
            if (idx >= _frameResult.indices.size()) break;
            const uint32_t vIdx = _frameResult.indices[idx];
            if (vIdx * 3 + 2 >= _frameResult.eyeRelPositions.size()) continue;
            if (vIdx           >= _frameResult.altitudes.size())      continue;
            const float x = _frameResult.eyeRelPositions[vIdx * 3 + 0];
            const float y = _frameResult.eyeRelPositions[vIdx * 3 + 1];
            const float z = _frameResult.eyeRelPositions[vIdx * 3 + 2];
            const float alt = _frameResult.altitudes[vIdx];

            // Horizontal component (perpendicular to nadir axis).
            const float dot = x*nx + y*ny + z*nz;
            const float hx  = x - dot*nx;
            const float hy  = y - dot*ny;
            const float hz  = z - dot*nz;
            const float horizSq = hx*hx + hy*hy + hz*hz;

            if (horizSq < horizThreshSq) {
              if (alt > maxNadirAlt) maxNadirAlt = alt;
              anyNadir = true;
            }

            // Always track the closest vertex as a fallback.
            const float distSq = x*x + y*y + z*z;
            if (distSq < minDistSq) {
              minDistSq = distSq;
              closestAlt = alt;
            }
          }
        }

        const double newEst = anyNadir ? static_cast<double>(maxNadirAlt)
                                       : static_cast<double>(closestAlt);

        // Snap up immediately (new terrain is higher = potential collision),
        // decay down slowly (avoid releasing clamp as tiles scroll away).
        if (newEst > _terrainHeightBelowCamera) {
          _terrainHeightBelowCamera = newEst;
        } else {
          _terrainHeightBelowCamera = 0.95 * _terrainHeightBelowCamera + 0.05 * newEst;
        }

      }
    }

    if (_debugOverlay) {
      const auto cp = _engine->camera().getParams();
      self.debugOverlayText = [NSString
          stringWithFormat:
              @"tiles=%d loadQ=%d visited=%d\nion=%@ ts=%@ fov=%.1f°\n"
              @"lat=%.5f lon=%.5f alt=%.0f\nhdg=%.1f\n"
              @"pitch  cur=%.2f  dem=%.2f  Δ=%.2f\n"
              @"roll   cur=%.2f  dem=%.2f  Δ=%.2f",
              _frameResult.tilesRendered, _frameResult.tilesLoading,
              _frameResult.tilesVisited,
              _frameResult.ionTokenConfigured ? @"yes" : @"no",
              _frameResult.tilesetActive ? @"yes" : @"no", _frameResult.verticalFovDeg,
              _frameResult.cameraLat, _frameResult.cameraLon, _frameResult.cameraAlt,
              cp.heading,
              cp.pitch, _camTarget.pitch, _camTarget.pitch - cp.pitch,
              cp.roll,  _camTarget.roll,  _camTarget.roll  - cp.roll];
    }

    const double instFps = (dt > 1e-6) ? (1.0 / dt) : 0.0;
    _fpsEma = (_fpsEma <= 1e-6) ? instFps : (_fpsEma * 0.85 + instFps * 0.15);

    if (++_metricsTick >= 30) {
      _metricsTick               = 0;
      _metricsFps                = _fpsEma;
      _metricsTilesRendered      = _frameResult.tilesRendered;
      _metricsTilesLoading       = _frameResult.tilesLoading;
      _metricsTilesVisited       = _frameResult.tilesVisited;
      _metricsIonTokenConfigured = _frameResult.ionTokenConfigured;
      _metricsTilesetReady       = _frameResult.tilesetActive;
      _metricsTerrainHeight      = _terrainHeightBelowCamera;
      if (_showCredits && !_frameResult.creditHtmlLines.empty()) {
        NSMutableString* credits = [NSMutableString string];
        for (const auto& html : _frameResult.creditHtmlLines) {
          NSString* chunk = stripHtmlToPlain(
              [NSString stringWithUTF8String:html.c_str()]);
          if (chunk.length == 0) continue;
          if (credits.length > 0) [credits appendString:@" · "];
          [credits appendString:chunk];
        }
        _metricsCreditsPlainText = [credits copy];
      } else {
        _metricsCreditsPlainText = @"";
      }
    }

    reactnativecesium::FrameParams params;
    _metalBackend->beginFrame(params);
    _metalBackend->drawScene(_frameResult);
    _metalBackend->endFrame();
  }
}

- (double)metricsFps              { return _metricsFps; }
- (NSInteger)metricsTilesRendered { return _metricsTilesRendered; }
- (NSInteger)metricsTilesLoading  { return _metricsTilesLoading; }
- (NSInteger)metricsTilesVisited  { return _metricsTilesVisited; }
- (BOOL)metricsIonTokenConfigured { return _metricsIonTokenConfigured; }
- (BOOL)metricsTilesetReady       { return _metricsTilesetReady; }
- (NSString*)metricsCreditsPlainText { return _metricsCreditsPlainText ?: @""; }
- (double)metricsTerrainHeight    { return _metricsTerrainHeight; }
- (BOOL)metricsNetworkReachable   { return _networkReachable; }

- (double)readCameraLatitude  { return _engine ? _engine->camera().getParams().latitude  : 0.0; }
- (double)readCameraLongitude { return _engine ? _engine->camera().getParams().longitude : 0.0; }
- (double)readCameraAltitude  { return _engine ? _engine->camera().getParams().altitude  : 0.0; }
- (double)readCameraHeading   { return _engine ? _engine->camera().getParams().heading   : 0.0; }
- (double)readCameraPitch     { return _engine ? _engine->camera().getParams().pitch     : 0.0; }
- (double)readCameraRoll      { return _engine ? _engine->camera().getParams().roll      : 0.0; }
- (double)readVerticalFovDeg  { return _engine ? _engine->camera().getVerticalFovDegrees() : 60.0; }

- (void)shutdown {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  if (_pathMonitor) {
    nw_path_monitor_cancel(_pathMonitor);
    _pathMonitor = nil;
  }
  if (_engine) _engine->shutdown();
  if (_metalBackend) _metalBackend->shutdown();
  _initialized = NO;
}

@end
