#import <Foundation/Foundation.h>

#include "cesium-native-smoke.hpp"

@interface CesiumNativeBridge : NSObject
@end

@implementation CesiumNativeBridge

+ (void)load {
  reactnativecesium::cesiumNativeSmokeTouch();
}

@end
