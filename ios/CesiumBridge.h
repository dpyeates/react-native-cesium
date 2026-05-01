#import <Foundation/Foundation.h>
#import <MetalKit/MetalKit.h>

NS_ASSUME_NONNULL_BEGIN

// Thin Obj-C++ shell for the iOS side. All cross-platform logic (camera
// demand state + smoothing, FPS / metrics aggregation, credit-HTML stripping)
// lives in shared C++ next to CesiumEngine.
@interface CesiumBridge : NSObject

- (instancetype)initWithMetalLayer:(CAMetalLayer *)layer
                             width:(int)width
                            height:(int)height
                         cacheDir:(NSString *)cacheDir;

- (void)updateIonAccessToken:(NSString *)token assetId:(int64_t)assetId;
- (void)updateImageryAssetId:(int64_t)assetId;
- (void)updateCameraLatitude:(double)lat
                   longitude:(double)lon
                    altitude:(double)alt
                     heading:(double)heading
                       pitch:(double)pitch
                        roll:(double)roll;
/// Same as `updateCameraLatitude:...` plus a camera-space view correction (w,x,y,z) applied after HPR.
- (void)updateCameraQuaternionLatitude:(double)lat
                             longitude:(double)lon
                              altitude:(double)alt
                               heading:(double)heading
                                 pitch:(double)pitch
                                  roll:(double)roll
                       viewCorrectionW:(double)qw
                                     x:(double)qx
                                     y:(double)qy
                                     z:(double)qz;
- (void)resize:(int)width height:(int)height;
- (BOOL)shouldRenderNextFrame;
- (void)markNeedsRender;
- (void)renderFrameWithDt:(double)dt;
- (void)shutdown;

// Camera / globe
- (void)setVerticalFovDeg:(double)degrees;

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

/// Throttled metrics consumed by the Swift hybrid view.
@property (nonatomic, readonly) double metricsFps;
@property (nonatomic, readonly) NSInteger metricsTilesRendered;
@property (nonatomic, readonly) NSInteger metricsTilesLoading;
@property (nonatomic, readonly) NSInteger metricsTilesVisited;
@property (nonatomic, readonly) BOOL metricsIonTokenConfigured;
@property (nonatomic, readonly) BOOL metricsTilesetReady;
@property (nonatomic, readonly) BOOL metricsTlsConfigured;
@property (nonatomic, readonly, copy) NSString *metricsCreditsPlainText;

- (double)readCameraLatitude;
- (double)readCameraLongitude;
- (double)readCameraAltitude;
- (double)readCameraHeading;
- (double)readCameraPitch;
- (double)readCameraRoll;
- (double)readVerticalFovDeg;
- (double)readViewCorrectionW;
- (double)readViewCorrectionX;
- (double)readViewCorrectionY;
- (double)readViewCorrectionZ;

@end

NS_ASSUME_NONNULL_END
