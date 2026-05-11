#import <Foundation/Foundation.h>
#import <MetalKit/MetalKit.h>

NS_ASSUME_NONNULL_BEGIN

// Thin Obj-C++ shell for the iOS side. All cross-platform logic (per-DoF
// camera integrator, FPS / metrics aggregation, credit-HTML stripping) lives
// in shared C++ next to CesiumEngine.
@interface CesiumBridge : NSObject

- (instancetype)initWithMetalLayer:(CAMetalLayer *)layer
                             width:(int)width
                            height:(int)height
                         cacheDir:(NSString *)cacheDir;

- (void)updateIonAccessToken:(NSString *)token assetId:(int64_t)assetId;
- (void)updateImageryAssetId:(int64_t)assetId;

// ── Per-DoF camera demand setters ────────────────────────────────────────
// Each call records the measurement with the current native steady_clock
// time and performs one α-β update against the predicted state at that
// instant. Pitch / roll are bundled because they typically come from a
// single IMU sample.
- (void)setPositionLatitude:(double)lat longitude:(double)lon;
- (void)setAltitude:(double)alt;
- (void)setHeadingDeg:(double)deg;
- (void)setAttitudePitch:(double)pitch roll:(double)roll;
- (void)setViewCorrectionW:(double)qw x:(double)qx y:(double)qy z:(double)qz;

/// Hard scene jump — resets every DoF (value + velocity + demand) atomically.
- (void)teleportLatitude:(double)lat
                longitude:(double)lon
                 altitude:(double)alt
                  heading:(double)heading
                    pitch:(double)pitch
                     roll:(double)roll
           verticalFovDeg:(double)vfov;

- (void)resize:(int)width height:(int)height;
- (BOOL)shouldRenderNextFrame;
- (void)markNeedsRender;
/// Render one frame. `nowSeconds` is wall-clock seconds (steady_clock) used
/// by the integrator to extrapolate every DoF forward.
- (void)renderFrameAt:(double)nowSeconds;
- (void)shutdown;

// Camera / globe
- (void)setVerticalFovDeg:(double)degrees;

/// Runtime rate caps (0 = uncapped). See EngineTunables for compile-time
/// defaults.
- (void)setRateCapsYaw:(double)yawDegSec
                  pitch:(double)pitchDegSec
                   roll:(double)rollDegSec
                  climb:(double)climbMps
            groundSpeed:(double)groundMps;

// Tileset tuning (runtime-mutable; no full rebuild)
- (void)setMaximumScreenSpaceError:(double)v;
- (void)setMaximumSimultaneousTileLoads:(int32_t)v;
- (void)setLoadingDescendantLimit:(int32_t)v;

- (void)setMsaaSampleCount:(int)samples;

// Optional perf / quality knobs (defaults match EngineConfig).
- (void)setMaximumCachedMiB:(int32_t)v;
- (void)setPreloadAncestors:(BOOL)v;
- (void)setPreloadSiblings:(BOOL)v;
- (void)setForbidHoles:(BOOL)v;
- (void)setEnableWaterMask:(BOOL)v;
- (void)setEnableFogCulling:(BOOL)v;
- (void)setEnforceCulledScreenSpaceError:(BOOL)v;
- (void)setCulledScreenSpaceError:(double)v;
- (void)setEnableLodTransitionPeriod:(BOOL)v;
- (void)setLodTransitionLength:(double)v;
- (void)setSqliteCacheMaxRows:(int32_t)v;
- (void)setTaskProcessorThreads:(int32_t)v;
- (void)setMinAltitudeAboveTerrain:(float)v;

/// Throttled metrics consumed by the Swift hybrid view.
@property (nonatomic, readonly) double metricsFps;
@property (nonatomic, readonly) NSInteger metricsTilesRendered;
@property (nonatomic, readonly) NSInteger metricsTilesLoading;
@property (nonatomic, readonly) NSInteger metricsTilesVisited;
@property (nonatomic, readonly) BOOL metricsIonTokenConfigured;
@property (nonatomic, readonly) BOOL metricsTilesetReady;
@property (nonatomic, readonly) BOOL metricsTlsConfigured;
@property (nonatomic, readonly, copy) NSString *metricsCreditsPlainText;

// ── Actual camera readback (what was rendered most recently) ────────────
- (double)readActualLatitude;
- (double)readActualLongitude;
- (double)readActualAltitude;
- (double)readActualHeading;
- (double)readActualPitch;
- (double)readActualRoll;
- (double)readActualVerticalFovDeg;

// ── Demand camera readback (what the consumer last asked for) ───────────
- (double)readDemandLatitude;
- (double)readDemandLongitude;
- (double)readDemandAltitude;
- (double)readDemandHeading;
- (double)readDemandPitch;
- (double)readDemandRoll;
- (double)readDemandVerticalFovDeg;

- (double)readViewCorrectionW;
- (double)readViewCorrectionX;
- (double)readViewCorrectionY;
- (double)readViewCorrectionZ;

@end

NS_ASSUME_NONNULL_END
