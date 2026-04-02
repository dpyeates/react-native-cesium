#import <Foundation/Foundation.h>
#import <MetalKit/MetalKit.h>

NS_ASSUME_NONNULL_BEGIN

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
- (void)setDebugOverlay:(BOOL)enabled;
- (void)setShowCredits:(BOOL)enabled;
- (void)resize:(int)width height:(int)height;
- (void)renderFrameWithDt:(double)dt;
- (void)shutdown;

- (void)setJoystickPitchRate:(double)pitchRate rollRate:(double)rollRate;

// Camera / globe
- (void)setVerticalFovDeg:(double)degrees;

// Tileset tuning (triggers rebuild when changed)
- (void)setMaximumScreenSpaceError:(double)v;
- (void)setMaximumSimultaneousTileLoads:(int)v;
- (void)setLoadingDescendantLimit:(int)v;

- (void)setMsaaSampleCount:(int)samples;

/// Updated each frame when debug overlay is on — read after `renderFrameWithDt`.
@property (nonatomic, copy, nullable) NSString *debugOverlayText;

/// Throttled metrics (updated ~every 20 frames inside `renderFrameWithDt`).
@property (nonatomic, readonly) double metricsFps;
@property (nonatomic, readonly) NSInteger metricsTilesRendered;
@property (nonatomic, readonly) NSInteger metricsTilesLoading;
@property (nonatomic, readonly) NSInteger metricsTilesVisited;
@property (nonatomic, readonly) BOOL metricsIonTokenConfigured;
@property (nonatomic, readonly) BOOL metricsTilesetReady;
@property (nonatomic, readonly) NSString *metricsCreditsPlainText;
/// Estimated terrain altitude (m above WGS84 ellipsoid) below the camera.
@property (nonatomic, readonly) double metricsTerrainHeight;
/// YES when the device has a usable network path; updated by NWPathMonitor.
/// When NO, tile requests are suspended (maximumSimultaneousTileLoads = 0)
/// and the ellipsoid fallback mesh provides flat terrain for uncached areas.
@property (nonatomic, readonly) BOOL metricsNetworkReachable;

- (double)readCameraLatitude;
- (double)readCameraLongitude;
- (double)readCameraAltitude;
- (double)readCameraHeading;
- (double)readCameraPitch;
- (double)readCameraRoll;
- (double)readVerticalFovDeg;

- (void)flyToLatitude:(double)lat
            longitude:(double)lon
             altitude:(double)alt
              heading:(double)heading
                pitch:(double)pitch
                 roll:(double)roll
     durationSeconds:(double)duration;

- (void)lookAtTargetLatitude:(double)lat
                   longitude:(double)lon
                    altitude:(double)alt
            durationSeconds:(double)duration;

- (void)cancelCameraAnimation;

@end

NS_ASSUME_NONNULL_END
