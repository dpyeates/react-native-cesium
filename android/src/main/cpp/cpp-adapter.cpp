#include <jni.h>
#include <fbjni/fbjni.h>
#include <vulkan/vulkan.h>

#include "ReactNativeCesiumOnLoad.hpp"
#include "cesium-native-smoke.hpp"

// Touch Vulkan ABI so the NDK Vulkan link is exercised at build time.
static const uint32_t kCesiumVulkanApiVersion = VK_MAKE_VERSION(1, 1, 0);

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
  (void)kCesiumVulkanApiVersion;
  reactnativecesium::cesiumNativeSmokeTouch();
  return facebook::jni::initialize(vm, []() {
    margelo::nitro::reactnativecesium::registerAllNatives();
  });
}