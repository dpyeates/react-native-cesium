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
/// Render one frame.  |dt| is the actual frame duration in seconds
/// (from CADisplayLink.targetTimestamp - CADisplayLink.timestamp).
/// Used to integrate joystick rates correctly at any refresh rate.
- (void)renderFrameWithDt:(double)dt;
- (void)shutdown;

- (void)onTouchDown:(int)pointerId x:(float)x y:(float)y;
- (void)onTouchMove:(int)pointerId x:(float)x y:(float)y;
- (void)onTouchUp:(int)pointerId;
- (void)setViewportSize:(float)width height:(float)height;

/// Set aircraft-style joystick rates (degrees/second). Native render loop
/// applies these each frame. Pass (0, 0) to stop.
- (void)setJoystickPitchRate:(double)pitchRate rollRate:(double)rollRate;

@end

NS_ASSUME_NONNULL_END
