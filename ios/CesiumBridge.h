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
- (void)resize:(int)width height:(int)height;
- (void)renderFrameWithDt:(double)dt;
- (void)shutdown;

- (void)onTouchDown:(int)pointerId x:(float)x y:(float)y;
- (void)onTouchMove:(int)pointerId x:(float)x y:(float)y;
- (void)onTouchUp:(int)pointerId;
- (void)setViewportSize:(float)width height:(float)height;

- (void)setJoystickPitchRate:(double)pitchRate rollRate:(double)rollRate;

// Camera / globe
- (void)setVerticalFovDeg:(double)degrees;

// Gestures
- (void)setGesturePanEnabled:(BOOL)enabled;
- (void)setGesturePinchZoomEnabled:(BOOL)enabled;
- (void)setGesturePinchRotateEnabled:(BOOL)enabled;
- (void)setGesturePanSensitivity:(double)s;
- (void)setGesturePinchSensitivity:(double)s;

// Tileset tuning (triggers rebuild when changed)
- (void)setMaximumScreenSpaceError:(double)v;
- (void)setMaximumSimultaneousTileLoads:(int)v;
- (void)setLoadingDescendantLimit:(int)v;

- (void)setMsaaSampleCount:(int)samples;

/// Updated each frame when debug overlay is on — read after `renderFrameWithDt`.
@property (nonatomic, copy, nullable) NSString *debugOverlayText;
/// Plain-text attribution from last frame (HTML stripped).
@property (nonatomic, copy, nullable) NSString *creditsPlainText;

/// Throttled metrics (updated ~every 20 frames inside `renderFrameWithDt`).
@property (nonatomic, readonly) double metricsFps;
@property (nonatomic, readonly) NSInteger metricsTilesRendered;
@property (nonatomic, readonly) NSInteger metricsTilesLoading;
@property (nonatomic, readonly) NSInteger metricsTilesVisited;
@property (nonatomic, readonly) BOOL metricsIonTokenConfigured;
@property (nonatomic, readonly) BOOL metricsTilesetReady;
@property (nonatomic, readonly, copy) NSString *metricsCreditsPlainText;

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
