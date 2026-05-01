#include <jni.h>
#include <fbjni/fbjni.h>
#include <vulkan/vulkan.h>

#include "ReactNativeCesiumOnLoad.hpp"

// Cesium tile content types are registered exactly once in CesiumEngine's
// constructor; do not duplicate that registration here.

static const uint32_t kCesiumVulkanApiVersion = VK_MAKE_VERSION(1, 1, 0);

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
  (void)kCesiumVulkanApiVersion;
  return facebook::jni::initialize(vm, []() {
    margelo::nitro::reactnativecesium::registerAllNatives();
  });
}
