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
/// Same as `updateCameraLatitude:...` plus camera-space view correction (w,x,y,z). Does not replace HPR; applied after HPR in the engine.
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

// Tileset tuning (triggers rebuild when changed)
- (void)setMaximumScreenSpaceError:(double)v;
- (void)setMaximumSimultaneousTileLoads:(int)v;
- (void)setLoadingDescendantLimit:(int)v;

- (void)setMsaaSampleCount:(int)samples;

/// Throttled metrics consumed by the Swift hybrid view.
@property (nonatomic, readonly) double metricsFps;
@property (nonatomic, readonly) NSInteger metricsTilesRendered;
@property (nonatomic, readonly) NSInteger metricsTilesLoading;
@property (nonatomic, readonly) NSInteger metricsTilesVisited;
@property (nonatomic, readonly) BOOL metricsIonTokenConfigured;
@property (nonatomic, readonly) BOOL metricsTilesetReady;
@property (nonatomic, readonly) NSString *metricsCreditsPlainText;
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
