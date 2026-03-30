#include <jni.h>
#include <fbjni/fbjni.h>
#include <vulkan/vulkan.h>

#include "ReactNativeCesiumOnLoad.hpp"

#include <Cesium3DTilesContent/registerAllTileContentTypes.h>

static const uint32_t kCesiumVulkanApiVersion = VK_MAKE_VERSION(1, 1, 0);

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
  (void)kCesiumVulkanApiVersion;
  Cesium3DTilesContent::registerAllTileContentTypes();
  return facebook::jni::initialize(vm, []() {
    margelo::nitro::reactnativecesium::registerAllNatives();
  });
}
