// VulkanBackend.cpp — Android Vulkan rendering backend
//
// Architecture mirrors MetalBackend.mm:
//   - RTC (Relative-to-Center) vertex positions: tile-local float offsets from
//     each tile's ECEF centre, computed on the CPU.
//   - Per-tile MVP matrix computed on the CPU in double precision (baking in the
//     large camera↔tile-centre translation), then cast to float32 for the GPU.
//   - Pre-computed vertex altitudes (ellipsoid height metres, double-precision
//     Bowring formula on CPU), passed as a separate vertex attribute to avoid
//     catastrophic cancellation in the shader.
//   - Single merged vertex + index buffer uploaded each frame.
//   - Reversed-Z infinite projection: depth clear = 0, compare = GREATER.
//   - Terrain drawn first (writes depth); sky drawn after with a depth test
//     (no write) so the full-screen atmospheric shader only runs on pixels not
//     covered by terrain.
//   - Water mask: UV computed from geographic lat/lon + tile bounds (not fragUV).
//   - Push constants: bytes 0-63 vertex (MVP), bytes 64-127 fragment.
//   - Triple-buffered persistent VkBuffers guarded by VkFence.
//
// Texture upload pipeline (P0-vk-uploads):
//   - Persistent host-visible staging ring (64 MB, mapped once at init).
//   - createRasterTexture / createWaterMaskTexture allocate a slice + memcpy
//     pixels in, then enqueue a PendingUpload — they DO NOT submit a command
//     buffer or call vkQueueWaitIdle.
//   - beginFrame submits the queued uploads in one command buffer with batched
//     pipeline barriers, signalling the per-frame upload fence. The matching
//     fence is waited on N=kMaxFramesInFlight frames later, at which point the
//     staging slices are reclaimed.
//   - Textures are guarded by a `pendingUpload` flag — drawScene falls back to
//     the white texture for a tile whose pixels have not yet hit VRAM (one
//     frame at most).

#include "VulkanBackend.h"

#include "engine/EngineTunables.hpp"

#include <android/native_window.h>
#include <android/log.h>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

// Pre-compiled SPIR-V headers (generated at build time by glslc + spv_to_header.cmake)
#include "terrain.vert.spv.h"
#include "terrain.frag.spv.h"
#include "terrain_dither.frag.spv.h"
#include "sky.vert.spv.h"
#include "sky.frag.spv.h"

#define LOG_TAG "VulkanBackend"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// VK_CHECK throws on error during init paths — these are non-recoverable. For
// per-frame paths VkResult is inspected explicitly so we can degrade gracefully.
#define VK_CHECK(call) do {                                                     \
  VkResult _r = (call);                                                          \
  if (_r != VK_SUCCESS) {                                                        \
    LOGE("%s failed: %d at %s:%d", #call, _r, __FILE__, __LINE__);               \
  }                                                                              \
} while(0)

#define VK_CHECK_FATAL(call) do {                                                \
  VkResult _r = (call);                                                          \
  if (_r != VK_SUCCESS) {                                                        \
    LOGE("%s failed: %d at %s:%d (FATAL)", #call, _r, __FILE__, __LINE__);       \
    return;                                                                      \
  }                                                                              \
} while(0)

// Per-frame UBO (set 0): camera ECEF for the fragment lighting view direction.
// MVP is now per-draw via push constants; only cameraEcef stays in the UBO.
struct TerrainUBO {
  float cameraEcef[4];   // 16 bytes
};

struct SkyUBO {
  float invVP[16];       // 64 bytes
  float cameraEcef[4];   // 16 bytes
  float lightDir[4];     // 16 bytes
};                       // 96 bytes

// Push constants split across vertex (bytes 0-63) and fragment (bytes 64-127).
// Total = 128 bytes (the Vulkan spec minimum guarantee on all devices).
struct TerrainVertexPC {
  float mvpMatrix[16];   // 64 bytes — per-tile RTC MVP (double-precision source)
};

struct TerrainFragmentPC {
  uint32_t hasOverlay;           // 4 bytes
  uint32_t isEllipsoidFallback;  // 4 bytes
  uint32_t isOnlyWater;          // 4 bytes
  uint32_t hasWaterMask;         // 4 bytes
  float    wmWest;               // 4 bytes — tile geographic bounds (radians)
  float    wmSouth;              // 4 bytes
  float    wmEast;               // 4 bytes
  float    wmNorth;              // 4 bytes
  float    rtcCenterX;           // 4 bytes — tile RTC centre ECEF (float)
  float    rtcCenterY;           // 4 bytes
  float    rtcCenterZ;           // 4 bytes
  float    translationX;         // 4 bytes — overlay UV transform
  float    translationY;         // 4 bytes
  float    scaleX;               // 4 bytes
  float    scaleY;               // 4 bytes
  float    lodFade;              // 4 bytes  — LOD dither threshold
};                               // 64 bytes

namespace reactnativecesium {

VulkanBackend::VulkanBackend() = default;

VulkanBackend::~VulkanBackend() { shutdown(); }

// ── Init ────────────────────────────────────────────────────────────────────

void VulkanBackend::initialize(void* nativeSurface, int width, int height) {
  window_ = nativeSurface;
  viewportWidth_  = width;
  viewportHeight_ = height;

  createInstance();
  VkAndroidSurfaceCreateInfoKHR surfaceInfo{};
  surfaceInfo.sType  = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
  surfaceInfo.window = static_cast<ANativeWindow*>(window_);
  VK_CHECK(vkCreateAndroidSurfaceKHR(instance_, &surfaceInfo, nullptr, &surface_));

  pickPhysicalDevice();
  createDevice();
  createCommandPool();
  createPipelineCache();
  // Defer swapchain creation until the first beginFrame() to avoid Android
  // compositor readiness issues. The swapchain, renderpass, depth resources,
  // framebuffers, and pipelines will all be created on-demand.
  // createSwapchain();
  // createRenderPass();
  // createDepthResources();
  // if (sampleCount_ != VK_SAMPLE_COUNT_1_BIT) createMsaaResources();
  // createFramebuffers();
  createCommandBuffers();
  createSyncObjects();
  createDescriptorSetLayout();
  createDescriptorPool();
  createWaterMaskPool();
  createPipelineLayout();
  // createGraphicsPipelinesAll();
  initStagingRing(kStagingSize);
  createFallbackTexture();
  createWaterMaskFallback();

  // Allocate sky UBO + descriptor set (persistently mapped)
  createBuffer(sizeof(SkyUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
               skyUboBuffer_, skyUboMemory_);
  VK_CHECK(vkMapMemory(device_, skyUboMemory_, 0, sizeof(SkyUBO), 0, &skyUboMapped_));

  VkDescriptorSetAllocateInfo skyAllocInfo{};
  skyAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  skyAllocInfo.descriptorPool = descriptorPool_;
  skyAllocInfo.descriptorSetCount = 1;
  skyAllocInfo.pSetLayouts = &skyDescSetLayout_;
  VK_CHECK(vkAllocateDescriptorSets(device_, &skyAllocInfo, &skyDescSet_));

  VkDescriptorBufferInfo skyBufInfo{};
  skyBufInfo.buffer = skyUboBuffer_;
  skyBufInfo.offset = 0;
  skyBufInfo.range  = sizeof(SkyUBO);

  VkWriteDescriptorSet skyWrite{};
  skyWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  skyWrite.dstSet = skyDescSet_;
  skyWrite.dstBinding = 0;
  skyWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  skyWrite.descriptorCount = 1;
  skyWrite.pBufferInfo = &skyBufInfo;
  vkUpdateDescriptorSets(device_, 1, &skyWrite, 0, nullptr);

  for (int i = 0; i < kMaxFramesInFlight; ++i) {
    createBuffer(sizeof(TerrainUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 terrainUboBufs_[i], terrainUboMems_[i]);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &terrainDescSetLayout_;
    VK_CHECK(vkAllocateDescriptorSets(device_, &allocInfo, &terrainDescSets_[i]));

    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = terrainUboBufs_[i];
    bufInfo.offset = 0;
    bufInfo.range  = sizeof(TerrainUBO);

    VkWriteDescriptorSet uboWrite{};
    uboWrite.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    uboWrite.dstSet          = terrainDescSets_[i];
    uboWrite.dstBinding      = 0;
    uboWrite.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboWrite.descriptorCount = 1;
    uboWrite.pBufferInfo     = &bufInfo;
    vkUpdateDescriptorSets(device_, 1, &uboWrite, 0, nullptr);

    VK_CHECK(vkMapMemory(device_, terrainUboMems_[i], 0, sizeof(TerrainUBO),
                         0, &terrainUboMapped_[i]));
  }

  {
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = descriptorPool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &terrainTexLayout_;
    VK_CHECK(vkAllocateDescriptorSets(device_, &allocInfo, &fallbackTexDescSet_));

    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfo.imageView   = fallbackTexture_.imageView;
    imgInfo.sampler     = fallbackTexture_.sampler;

    VkWriteDescriptorSet texWrite{};
    texWrite.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    texWrite.dstSet          = fallbackTexDescSet_;
    texWrite.dstBinding      = 0;
    texWrite.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    texWrite.descriptorCount = 1;
    texWrite.pImageInfo      = &imgInfo;
    vkUpdateDescriptorSets(device_, 1, &texWrite, 0, nullptr);
  }
}

void VulkanBackend::resize(int width, int height) {
  viewportWidth_  = width;
  viewportHeight_ = height;
  needsSwapchainRecreate_ = true;
}

void VulkanBackend::shutdown() {
  if (device_ == VK_NULL_HANDLE) return;
  vkDeviceWaitIdle(device_);

  persistPipelineCache();

  // Drain pending uploads (GPU is idle).
  {
    std::lock_guard<std::mutex> lk(stagingMutex_);
    for (auto& vec : uploadsInFlight_) vec.clear();
    pendingUploadsCurrent_.clear();
  }

  // Drain the deferred deletion queue — GPU is idle so all are safe to free.
  {
    std::lock_guard<std::mutex> lk(pendingDeletesMutex_);
    while (!pendingDeletes_.empty()) {
      auto& front = pendingDeletes_.front();
      destroyTexture(front.tex, front.pool);
      pendingDeletes_.pop_front();
    }
  }

  if (fallbackTexture_.sampler)     vkDestroySampler(device_, fallbackTexture_.sampler, nullptr);
  if (fallbackTexture_.imageView)   vkDestroyImageView(device_, fallbackTexture_.imageView, nullptr);
  if (fallbackTexture_.image)       vkDestroyImage(device_, fallbackTexture_.image, nullptr);
  if (fallbackTexture_.memory)      vkFreeMemory(device_, fallbackTexture_.memory, nullptr);
  fallbackTexture_ = {};
  delete fallbackTexturePtr_;
  fallbackTexturePtr_ = nullptr;

  if (fallbackWaterMaskTex_.sampler)   vkDestroySampler(device_, fallbackWaterMaskTex_.sampler, nullptr);
  if (fallbackWaterMaskTex_.imageView) vkDestroyImageView(device_, fallbackWaterMaskTex_.imageView, nullptr);
  if (fallbackWaterMaskTex_.image)     vkDestroyImage(device_, fallbackWaterMaskTex_.image, nullptr);
  if (fallbackWaterMaskTex_.memory)    vkFreeMemory(device_, fallbackWaterMaskTex_.memory, nullptr);
  fallbackWaterMaskTex_     = {};
  fallbackWaterMaskDescSet_ = VK_NULL_HANDLE;
  delete fallbackWaterMaskTexPtr_;
  fallbackWaterMaskTexPtr_ = nullptr;

  if (skyUboMapped_)  { vkUnmapMemory(device_, skyUboMemory_); skyUboMapped_ = nullptr; }
  if (skyUboBuffer_)  vkDestroyBuffer(device_, skyUboBuffer_, nullptr);
  if (skyUboMemory_)  vkFreeMemory(device_, skyUboMemory_, nullptr);
  skyUboBuffer_ = VK_NULL_HANDLE;
  skyUboMemory_ = VK_NULL_HANDLE;

  for (int i = 0; i < kMaxFramesInFlight; ++i) {
    if (terrainUboMapped_[i]) { vkUnmapMemory(device_, terrainUboMems_[i]); terrainUboMapped_[i] = nullptr; }
    if (terrainUboBufs_[i])   vkDestroyBuffer(device_, terrainUboBufs_[i], nullptr);
    if (terrainUboMems_[i])   vkFreeMemory(device_, terrainUboMems_[i], nullptr);
    terrainUboBufs_[i] = VK_NULL_HANDLE;
    terrainUboMems_[i] = VK_NULL_HANDLE;
  }

  for (int i = 0; i < kMaxFramesInFlight; ++i) {
    if (vtxMapped_[i]) { vkUnmapMemory(device_, vtxMems_[i]); vtxMapped_[i] = nullptr; }
    if (idxMapped_[i]) { vkUnmapMemory(device_, idxMems_[i]); idxMapped_[i] = nullptr; }
    if (uvMapped_[i])  { vkUnmapMemory(device_, uvMems_[i]);  uvMapped_[i]  = nullptr; }
    if (altMapped_[i]) { vkUnmapMemory(device_, altMems_[i]); altMapped_[i] = nullptr; }
    if (vtxBufs_[i]) vkDestroyBuffer(device_, vtxBufs_[i], nullptr);
    if (vtxMems_[i]) vkFreeMemory(device_, vtxMems_[i], nullptr);
    if (idxBufs_[i]) vkDestroyBuffer(device_, idxBufs_[i], nullptr);
    if (idxMems_[i]) vkFreeMemory(device_, idxMems_[i], nullptr);
    if (uvBufs_[i])  vkDestroyBuffer(device_, uvBufs_[i], nullptr);
    if (uvMems_[i])  vkFreeMemory(device_, uvMems_[i], nullptr);
    if (altBufs_[i]) vkDestroyBuffer(device_, altBufs_[i], nullptr);
    if (altMems_[i]) vkFreeMemory(device_, altMems_[i], nullptr);
    vtxBufs_[i] = idxBufs_[i] = uvBufs_[i] = altBufs_[i] = VK_NULL_HANDLE;
    vtxMems_[i] = idxMems_[i] = uvMems_[i] = altMems_[i] = VK_NULL_HANDLE;
    vtxCaps_[i] = idxCaps_[i] = uvCaps_[i] = altCaps_[i] = 0;
  }

  // Staging ring
  if (stagingMapped_)  { vkUnmapMemory(device_, stagingMemory_); stagingMapped_ = nullptr; }
  if (stagingBuffer_)  vkDestroyBuffer(device_, stagingBuffer_, nullptr);
  if (stagingMemory_)  vkFreeMemory(device_, stagingMemory_, nullptr);
  stagingBuffer_ = VK_NULL_HANDLE;
  stagingMemory_ = VK_NULL_HANDLE;
  for (auto& f : uploadFences_) {
    if (f) vkDestroyFence(device_, f, nullptr);
    f = VK_NULL_HANDLE;
  }

  for (auto& s : imageAvailableSemaphores_) vkDestroySemaphore(device_, s, nullptr);
  for (auto& s : renderFinishedSemaphores_) vkDestroySemaphore(device_, s, nullptr);
  for (auto& f : inFlightFences_)           vkDestroyFence(device_, f, nullptr);
  imageAvailableSemaphores_.clear();
  renderFinishedSemaphores_.clear();
  inFlightFences_.clear();

  cleanupSwapchain();

  destroyGraphicsPipelinesAll();
  if (terrainPipelineLayout_) vkDestroyPipelineLayout(device_, terrainPipelineLayout_, nullptr);
  if (skyPipelineLayout_)     vkDestroyPipelineLayout(device_, skyPipelineLayout_, nullptr);
  if (terrainDescSetLayout_)  vkDestroyDescriptorSetLayout(device_, terrainDescSetLayout_, nullptr);
  if (terrainTexLayout_)      vkDestroyDescriptorSetLayout(device_, terrainTexLayout_, nullptr);
  if (skyDescSetLayout_)      vkDestroyDescriptorSetLayout(device_, skyDescSetLayout_, nullptr);
  if (descriptorPool_)        vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
  if (waterMaskPool_)         vkDestroyDescriptorPool(device_, waterMaskPool_, nullptr);
  if (pipelineCache_)         vkDestroyPipelineCache(device_, pipelineCache_, nullptr);
  if (commandPool_)           vkDestroyCommandPool(device_, commandPool_, nullptr);
  if (renderPass_)            vkDestroyRenderPass(device_, renderPass_, nullptr);

  terrainPipelineLayout_ = skyPipelineLayout_ = VK_NULL_HANDLE;
  terrainDescSetLayout_ = terrainTexLayout_ = skyDescSetLayout_ = VK_NULL_HANDLE;
  descriptorPool_ = waterMaskPool_ = VK_NULL_HANDLE;
  pipelineCache_ = VK_NULL_HANDLE;
  commandPool_ = VK_NULL_HANDLE;
  renderPass_ = VK_NULL_HANDLE;

  vkDestroyDevice(device_, nullptr);
  device_ = VK_NULL_HANDLE;

  if (surface_)  vkDestroySurfaceKHR(instance_, surface_, nullptr);
  if (instance_) vkDestroyInstance(instance_, nullptr);
  surface_  = VK_NULL_HANDLE;
  instance_ = VK_NULL_HANDLE;
}

// ── Instance + Device ──────────────────────────────────────────────────────────

void VulkanBackend::createInstance() {
  VkApplicationInfo appInfo{};
  appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName   = "ReactNativeCesium";
  appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  appInfo.pEngineName        = "ReactNativeCesium";
  appInfo.engineVersion      = VK_MAKE_VERSION(1, 0, 0);
  appInfo.apiVersion         = VK_API_VERSION_1_1;

  const char* extensions[] = {
    VK_KHR_SURFACE_EXTENSION_NAME,
    VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
  };

  VkInstanceCreateInfo createInfo{};
  createInfo.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInfo.pApplicationInfo        = &appInfo;
  createInfo.enabledExtensionCount   = 2;
  createInfo.ppEnabledExtensionNames = extensions;
  VK_CHECK(vkCreateInstance(&createInfo, nullptr, &instance_));
}

void VulkanBackend::pickPhysicalDevice() {
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance_, &count, nullptr);
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(instance_, &count, devices.data());

  // Score: discrete >> integrated >> any. Within type, larger device-local
  // memory wins. Falls back to the first graphics+present queue if nothing
  // scores positively.
  int        bestScore = -1;
  uint32_t   bestQueue = 0;
  VkPhysicalDevice best = VK_NULL_HANDLE;

  for (auto& dev : devices) {
    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> qFamilies(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, qFamilies.data());

    uint32_t graphicsQueueIdx = UINT32_MAX;
    for (uint32_t i = 0; i < qCount; ++i) {
      VkBool32 presentSupport = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface_, &presentSupport);
      if ((qFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && presentSupport) {
        graphicsQueueIdx = i;
        break;
      }
    }
    if (graphicsQueueIdx == UINT32_MAX) continue;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(dev, &props);

    int score = 0;
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)   score = 1000;
    else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score = 500;
    else                                                            score = 100;

    if (score > bestScore) {
      bestScore = score;
      bestQueue = graphicsQueueIdx;
      best      = dev;
    }
  }

  if (best == VK_NULL_HANDLE) {
    LOGE("No suitable Vulkan physical device found");
    return;
  }

  physicalDevice_ = best;
  graphicsFamily_ = bestQueue;

  // Cache properties relevant to sampler / pipeline state.
  VkPhysicalDeviceFeatures   feats{};
  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceFeatures(physicalDevice_, &feats);
  vkGetPhysicalDeviceProperties(physicalDevice_, &props);
  supportsAnisotropy_ = feats.samplerAnisotropy == VK_TRUE;
  maxAnisotropy_      = props.limits.maxSamplerAnisotropy;

  // One-time log of the selected GPU so we can tell a real hardware device from
  // a software rasterizer (e.g. the Android Emulator's SwiftShader / a slow
  // gfxstream translation layer), which is the dominant factor in emulator FPS.
  const char* typeStr = "OTHER";
  switch (props.deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   typeStr = "DISCRETE_GPU";   break;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: typeStr = "INTEGRATED_GPU"; break;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    typeStr = "VIRTUAL_GPU";    break;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            typeStr = "CPU (software)";  break;
    default:                                     typeStr = "OTHER";          break;
  }
  LOGI("Vulkan GPU: \"%s\" type=%s apiVersion=%u.%u.%u driverVersion=0x%x",
       props.deviceName, typeStr,
       VK_VERSION_MAJOR(props.apiVersion),
       VK_VERSION_MINOR(props.apiVersion),
       VK_VERSION_PATCH(props.apiVersion),
       props.driverVersion);
  if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
    LOGW("Selected a CPU/software Vulkan device — expect very low FPS. "
         "This is typical on the Android Emulator without host-GPU acceleration; "
         "measure on a physical device for representative performance.");
  }

  const VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts &
                                    props.limits.framebufferDepthSampleCounts;
  if      (counts & VK_SAMPLE_COUNT_8_BIT) supportedMsaaMask_ = VK_SAMPLE_COUNT_8_BIT;
  else if (counts & VK_SAMPLE_COUNT_4_BIT) supportedMsaaMask_ = VK_SAMPLE_COUNT_4_BIT;
  else if (counts & VK_SAMPLE_COUNT_2_BIT) supportedMsaaMask_ = VK_SAMPLE_COUNT_2_BIT;
  else                                     supportedMsaaMask_ = VK_SAMPLE_COUNT_1_BIT;
}

void VulkanBackend::createDevice() {
  float queuePriority = 1.0f;
  VkDeviceQueueCreateInfo queueInfo{};
  queueInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queueInfo.queueFamilyIndex = graphicsFamily_;
  queueInfo.queueCount       = 1;
  queueInfo.pQueuePriorities = &queuePriority;

  const char* devExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

  VkPhysicalDeviceFeatures features{};
  features.samplerAnisotropy = supportsAnisotropy_ ? VK_TRUE : VK_FALSE;

  VkDeviceCreateInfo createInfo{};
  createInfo.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  createInfo.queueCreateInfoCount    = 1;
  createInfo.pQueueCreateInfos       = &queueInfo;
  createInfo.enabledExtensionCount   = 1;
  createInfo.ppEnabledExtensionNames = devExtensions;
  createInfo.pEnabledFeatures        = &features;
  VK_CHECK(vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_));

  vkGetDeviceQueue(device_, graphicsFamily_, 0, &graphicsQueue_);
  presentQueue_ = graphicsQueue_;
}

VkSampleCountFlagBits VulkanBackend::resolveMsaaSamples(int requested) const {
  if (requested <= 1) return VK_SAMPLE_COUNT_1_BIT;
  // Clamp request to the largest mask the device actually supports.
  VkSampleCountFlagBits desired =
      requested >= 8 ? VK_SAMPLE_COUNT_8_BIT
                     : (requested >= 4 ? VK_SAMPLE_COUNT_4_BIT
                                       : VK_SAMPLE_COUNT_2_BIT);
  if (static_cast<int>(desired) > static_cast<int>(supportedMsaaMask_))
    desired = supportedMsaaMask_;
  return desired;
}

VkFormat VulkanBackend::pickDepthFormat() const {
  for (VkFormat candidate : {VK_FORMAT_D32_SFLOAT,
                              VK_FORMAT_D24_UNORM_S8_UINT,
                              VK_FORMAT_D16_UNORM}) {
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(physicalDevice_, candidate, &props);
    if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
      return candidate;
  }
  LOGE("No supported depth format found; defaulting to D32_SFLOAT");
  return VK_FORMAT_D32_SFLOAT;
}

VkPresentModeKHR VulkanBackend::pickPresentMode() const {
  uint32_t count = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &count, nullptr);
  std::vector<VkPresentModeKHR> modes(count);
  vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &count, modes.data());

  bool hasMailbox = false, hasFifoRelaxed = false;
  for (auto m : modes) {
    if (m == VK_PRESENT_MODE_MAILBOX_KHR)      hasMailbox     = true;
    if (m == VK_PRESENT_MODE_FIFO_RELAXED_KHR) hasFifoRelaxed = true;
  }
  // Prefer FIFO_RELAXED for stutter resilience on phones where we sometimes
  // miss a vsync — it presents the late frame immediately rather than holding
  // for the next vsync. MAILBOX is "lower latency" but can drop frames; not
  // ideal for a globe view that's already battery-sensitive.
  if (hasFifoRelaxed) return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
  if (hasMailbox)     return VK_PRESENT_MODE_MAILBOX_KHR;
  return VK_PRESENT_MODE_FIFO_KHR; // always available per spec
}

// ── Swapchain ──────────────────────────────────────────────────────────────────

void VulkanBackend::createSwapchain() {
  VkSurfaceCapabilitiesKHR caps;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &caps);

  uint32_t fmtCount;
  vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &fmtCount, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(fmtCount);
  vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &fmtCount, formats.data());

  swapchainFormat_ = formats[0].format;
  VkColorSpaceKHR colorSpace = formats[0].colorSpace;
  for (auto& fmt : formats) {
    if (fmt.format == VK_FORMAT_B8G8R8A8_SRGB && fmt.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      swapchainFormat_ = fmt.format;
      colorSpace = fmt.colorSpace;
      break;
    }
    if (fmt.format == VK_FORMAT_R8G8B8A8_SRGB) {
      swapchainFormat_ = fmt.format;
      colorSpace = fmt.colorSpace;
    }
  }

  if (caps.currentExtent.width != UINT32_MAX) {
    swapchainExtent_ = caps.currentExtent;
  } else {
    swapchainExtent_.width  = std::clamp(static_cast<uint32_t>(viewportWidth_),
                                         caps.minImageExtent.width, caps.maxImageExtent.width);
    swapchainExtent_.height = std::clamp(static_cast<uint32_t>(viewportHeight_),
                                         caps.minImageExtent.height, caps.maxImageExtent.height);
  }

  uint32_t imageCount = caps.minImageCount + 1;
  if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
    imageCount = caps.maxImageCount;

  depthFormat_ = pickDepthFormat();

  VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  for (auto candidate : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                          VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                          VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                          VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
    if (caps.supportedCompositeAlpha & candidate) {
      compositeAlpha = candidate;
      break;
    }
  }

  VkSwapchainKHR oldSwapchain = swapchain_;

  VkSwapchainCreateInfoKHR swapInfo{};
  swapInfo.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  swapInfo.surface          = surface_;
  swapInfo.minImageCount    = imageCount;
  swapInfo.imageFormat      = swapchainFormat_;
  swapInfo.imageColorSpace  = colorSpace;
  swapInfo.imageExtent      = swapchainExtent_;
  swapInfo.imageArrayLayers = 1;
  swapInfo.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  swapInfo.preTransform     = caps.currentTransform;
  swapInfo.compositeAlpha   = compositeAlpha;
  swapInfo.presentMode      = pickPresentMode();
  swapInfo.clipped          = VK_TRUE;
  swapInfo.oldSwapchain     = oldSwapchain;
  VK_CHECK(vkCreateSwapchainKHR(device_, &swapInfo, nullptr, &swapchain_));

  if (oldSwapchain != VK_NULL_HANDLE) {
    vkDestroySwapchainKHR(device_, oldSwapchain, nullptr);
  }

  vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
  swapchainImages_.resize(imageCount);
  vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data());

  swapchainImageViews_.resize(imageCount);
  for (uint32_t i = 0; i < imageCount; ++i) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image    = swapchainImages_[i];
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format   = swapchainFormat_;
    viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel   = 0;
    viewInfo.subresourceRange.levelCount     = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount     = 1;
    VK_CHECK(vkCreateImageView(device_, &viewInfo, nullptr, &swapchainImageViews_[i]));
  }
}

void VulkanBackend::createRenderPass() {
  const bool msaa = (sampleCount_ != VK_SAMPLE_COUNT_1_BIT);

  // Colour attachment: when MSAA is enabled this is the multisampled colour
  // target (does not store), and we add a single-sample resolve attachment
  // that becomes PRESENT_SRC. When MSAA is off the colour attachment IS the
  // swapchain image directly.
  VkAttachmentDescription colorAttach{};
  colorAttach.format         = swapchainFormat_;
  colorAttach.samples        = sampleCount_;
  colorAttach.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
  colorAttach.storeOp        = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                    : VK_ATTACHMENT_STORE_OP_STORE;
  colorAttach.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  colorAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  colorAttach.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
  colorAttach.finalLayout    = msaa ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                    : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

  VkAttachmentDescription depthAttach{};
  depthAttach.format         = depthFormat_;
  depthAttach.samples        = sampleCount_;
  depthAttach.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depthAttach.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depthAttach.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  depthAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depthAttach.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
  depthAttach.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentDescription resolveAttach{};
  resolveAttach.format         = swapchainFormat_;
  resolveAttach.samples        = VK_SAMPLE_COUNT_1_BIT;
  resolveAttach.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  resolveAttach.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
  resolveAttach.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  resolveAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  resolveAttach.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
  resolveAttach.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

  VkAttachmentReference colorRef{};
  colorRef.attachment = 0;
  colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkAttachmentReference depthRef{};
  depthRef.attachment = 1;
  depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentReference resolveRef{};
  resolveRef.attachment = 2;
  resolveRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount    = 1;
  subpass.pColorAttachments       = &colorRef;
  subpass.pDepthStencilAttachment = &depthRef;
  if (msaa) subpass.pResolveAttachments = &resolveRef;

  VkSubpassDependency dependency{};
  dependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass    = 0;
  dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dependency.srcAccessMask = 0;
  dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

  std::vector<VkAttachmentDescription> attachments = {colorAttach, depthAttach};
  if (msaa) attachments.push_back(resolveAttach);

  VkRenderPassCreateInfo rpInfo{};
  rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rpInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
  rpInfo.pAttachments    = attachments.data();
  rpInfo.subpassCount    = 1;
  rpInfo.pSubpasses      = &subpass;
  rpInfo.dependencyCount = 1;
  rpInfo.pDependencies   = &dependency;
  VK_CHECK(vkCreateRenderPass(device_, &rpInfo, nullptr, &renderPass_));
}

void VulkanBackend::createDepthResources() {
  if (depthImageView_)  vkDestroyImageView(device_, depthImageView_, nullptr);
  if (depthImage_)      vkDestroyImage(device_, depthImage_, nullptr);
  if (depthMemory_)     vkFreeMemory(device_, depthMemory_, nullptr);

  VkImageCreateInfo imgInfo{};
  imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imgInfo.imageType     = VK_IMAGE_TYPE_2D;
  imgInfo.format        = depthFormat_;
  imgInfo.extent.width  = swapchainExtent_.width;
  imgInfo.extent.height = swapchainExtent_.height;
  imgInfo.extent.depth  = 1;
  imgInfo.mipLevels     = 1;
  imgInfo.arrayLayers   = 1;
  imgInfo.samples       = sampleCount_;
  imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
  imgInfo.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  VK_CHECK(vkCreateImage(device_, &imgInfo, nullptr, &depthImage_));

  VkMemoryRequirements memReqs;
  vkGetImageMemoryRequirements(device_, depthImage_, &memReqs);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize  = memReqs.size;
  allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VK_CHECK(vkAllocateMemory(device_, &allocInfo, nullptr, &depthMemory_));
  VK_CHECK(vkBindImageMemory(device_, depthImage_, depthMemory_, 0));

  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image    = depthImage_;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format   = depthFormat_;
  viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
  viewInfo.subresourceRange.baseMipLevel   = 0;
  viewInfo.subresourceRange.levelCount     = 1;
  viewInfo.subresourceRange.baseArrayLayer = 0;
  viewInfo.subresourceRange.layerCount     = 1;
  VK_CHECK(vkCreateImageView(device_, &viewInfo, nullptr, &depthImageView_));
}

void VulkanBackend::createMsaaResources() {
  if (msaaColorView_)   vkDestroyImageView(device_, msaaColorView_, nullptr);
  if (msaaColorImage_)  vkDestroyImage(device_, msaaColorImage_, nullptr);
  if (msaaColorMemory_) vkFreeMemory(device_, msaaColorMemory_, nullptr);

  VkImageCreateInfo imgInfo{};
  imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imgInfo.imageType     = VK_IMAGE_TYPE_2D;
  imgInfo.format        = swapchainFormat_;
  imgInfo.extent        = {swapchainExtent_.width, swapchainExtent_.height, 1};
  imgInfo.mipLevels     = 1;
  imgInfo.arrayLayers   = 1;
  imgInfo.samples       = sampleCount_;
  imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
  imgInfo.usage         = VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT |
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VK_CHECK(vkCreateImage(device_, &imgInfo, nullptr, &msaaColorImage_));

  VkMemoryRequirements memReqs;
  vkGetImageMemoryRequirements(device_, msaaColorImage_, &memReqs);

  // Prefer LAZILY_ALLOCATED for the transient MSAA target — on tiled mobile
  // GPUs this avoids actually allocating physical memory.
  bool foundLazy = false;
  uint32_t typeIdx = findMemoryType(
      memReqs.memoryTypeBits,
      VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, &foundLazy);
  if (!foundLazy) {
    typeIdx = findMemoryType(memReqs.memoryTypeBits,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  }

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize  = memReqs.size;
  allocInfo.memoryTypeIndex = typeIdx;
  VK_CHECK(vkAllocateMemory(device_, &allocInfo, nullptr, &msaaColorMemory_));
  VK_CHECK(vkBindImageMemory(device_, msaaColorImage_, msaaColorMemory_, 0));

  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image    = msaaColorImage_;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format   = swapchainFormat_;
  viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VK_CHECK(vkCreateImageView(device_, &viewInfo, nullptr, &msaaColorView_));
}

void VulkanBackend::createFramebuffers() {
  framebuffers_.resize(swapchainImageViews_.size());
  const bool msaa = (sampleCount_ != VK_SAMPLE_COUNT_1_BIT);
  for (size_t i = 0; i < swapchainImageViews_.size(); ++i) {
    std::vector<VkImageView> attachments;
    if (msaa) {
      attachments = {msaaColorView_, depthImageView_, swapchainImageViews_[i]};
    } else {
      attachments = {swapchainImageViews_[i], depthImageView_};
    }

    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass      = renderPass_;
    fbInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    fbInfo.pAttachments    = attachments.data();
    fbInfo.width           = swapchainExtent_.width;
    fbInfo.height          = swapchainExtent_.height;
    fbInfo.layers          = 1;
    VK_CHECK(vkCreateFramebuffer(device_, &fbInfo, nullptr, &framebuffers_[i]));
  }
}

void VulkanBackend::createCommandPool() {
  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
                              VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  poolInfo.queueFamilyIndex = graphicsFamily_;
  VK_CHECK(vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_));
}

void VulkanBackend::createCommandBuffers() {
  commandBuffers_.resize(kMaxFramesInFlight);
  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.commandPool        = commandPool_;
  allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers_.size());
  VK_CHECK(vkAllocateCommandBuffers(device_, &allocInfo, commandBuffers_.data()));

  // Separate command buffers for upload (one per frame in flight).
  uploadCommandBuffers_.resize(kMaxFramesInFlight);
  allocInfo.commandBufferCount = static_cast<uint32_t>(uploadCommandBuffers_.size());
  VK_CHECK(vkAllocateCommandBuffers(device_, &allocInfo, uploadCommandBuffers_.data()));
}

void VulkanBackend::createSyncObjects() {
  imageAvailableSemaphores_.resize(kMaxFramesInFlight);
  // renderFinishedSemaphores_ will be created after swapchain creation
  // since we need to know swapchainImages_.size()
  inFlightFences_.resize(kMaxFramesInFlight);

  VkSemaphoreCreateInfo semInfo{};
  semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

  for (int i = 0; i < kMaxFramesInFlight; ++i) {
    VK_CHECK(vkCreateSemaphore(device_, &semInfo, nullptr, &imageAvailableSemaphores_[i]));
    VK_CHECK(vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFences_[i]));
    VK_CHECK(vkCreateFence(device_, &fenceInfo, nullptr, &uploadFences_[i]));
  }
  // renderFinishedSemaphores_ created in beginFrame after swapchain is ready
}

// ── Pipeline cache (vk-pipeline-cache) ───────────────────────────────────────

namespace {
std::string pipelineCachePath(const std::string& cacheDir) {
  if (cacheDir.empty()) return std::string();
  return cacheDir + "/cesium_pipeline.cache";
}
} // namespace

void VulkanBackend::createPipelineCache() {
  std::vector<uint8_t> initial;
  const std::string path = pipelineCachePath(cacheDir_);
  if (!path.empty()) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (f.is_open()) {
      const std::streamsize size = f.tellg();
      if (size > 0) {
        initial.resize(static_cast<size_t>(size));
        f.seekg(0, std::ios::beg);
        f.read(reinterpret_cast<char*>(initial.data()), size);
      }
    }
  }

  VkPipelineCacheCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  info.initialDataSize = initial.size();
  info.pInitialData    = initial.empty() ? nullptr : initial.data();
  VK_CHECK(vkCreatePipelineCache(device_, &info, nullptr, &pipelineCache_));
}

void VulkanBackend::persistPipelineCache() {
  if (!pipelineCache_) return;
  const std::string path = pipelineCachePath(cacheDir_);
  if (path.empty()) return;

  size_t size = 0;
  if (vkGetPipelineCacheData(device_, pipelineCache_, &size, nullptr) != VK_SUCCESS) return;
  if (size == 0) return;

  std::vector<uint8_t> data(size);
  if (vkGetPipelineCacheData(device_, pipelineCache_, &size, data.data()) != VK_SUCCESS) return;

  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f.is_open()) return;
  f.write(reinterpret_cast<const char*>(data.data()),
          static_cast<std::streamsize>(size));
}

// ── Descriptor / pipeline layouts ───────────────────────────────────────────────

void VulkanBackend::createDescriptorSetLayout() {
  VkDescriptorSetLayoutBinding uboBinding{};
  uboBinding.binding         = 0;
  uboBinding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  uboBinding.descriptorCount = 1;
  uboBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutCreateInfo uboLayoutInfo{};
  uboLayoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  uboLayoutInfo.bindingCount = 1;
  uboLayoutInfo.pBindings    = &uboBinding;
  VK_CHECK(vkCreateDescriptorSetLayout(device_, &uboLayoutInfo, nullptr, &terrainDescSetLayout_));

  VkDescriptorSetLayoutBinding samplerBinding{};
  samplerBinding.binding         = 0;
  samplerBinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  samplerBinding.descriptorCount = 1;
  samplerBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutCreateInfo texLayoutInfo{};
  texLayoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  texLayoutInfo.bindingCount = 1;
  texLayoutInfo.pBindings    = &samplerBinding;
  VK_CHECK(vkCreateDescriptorSetLayout(device_, &texLayoutInfo, nullptr, &terrainTexLayout_));

  VkDescriptorSetLayoutBinding skyUboBinding{};
  skyUboBinding.binding         = 0;
  skyUboBinding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  skyUboBinding.descriptorCount = 1;
  skyUboBinding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutCreateInfo skyLayoutInfo{};
  skyLayoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  skyLayoutInfo.bindingCount = 1;
  skyLayoutInfo.pBindings    = &skyUboBinding;
  VK_CHECK(vkCreateDescriptorSetLayout(device_, &skyLayoutInfo, nullptr, &skyDescSetLayout_));
}

void VulkanBackend::createPipelineLayout() {
  // P2-arg-buffers: one combined push range over both stages, so we issue a
  // single vkCmdPushConstants per draw instead of two. The Vulkan spec
  // allows multi-stage ranges and the GLSL/Metal shaders read the same
  // block layout regardless.
  VkPushConstantRange pushRange{};
  pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  pushRange.offset     = 0;
  pushRange.size       = sizeof(TerrainVertexPC) + sizeof(TerrainFragmentPC); // 128 bytes

  VkDescriptorSetLayout terrainSetLayouts[] = {
      terrainDescSetLayout_, terrainTexLayout_, terrainTexLayout_,
  };

  VkPipelineLayoutCreateInfo terrainLayoutInfo{};
  terrainLayoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  terrainLayoutInfo.setLayoutCount         = 3;
  terrainLayoutInfo.pSetLayouts            = terrainSetLayouts;
  terrainLayoutInfo.pushConstantRangeCount = 1;
  terrainLayoutInfo.pPushConstantRanges    = &pushRange;
  VK_CHECK(vkCreatePipelineLayout(device_, &terrainLayoutInfo, nullptr, &terrainPipelineLayout_));

  VkPipelineLayoutCreateInfo skyLayoutInfo{};
  skyLayoutInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  skyLayoutInfo.setLayoutCount = 1;
  skyLayoutInfo.pSetLayouts    = &skyDescSetLayout_;
  VK_CHECK(vkCreatePipelineLayout(device_, &skyLayoutInfo, nullptr, &skyPipelineLayout_));
}

void VulkanBackend::createDescriptorPool() {
  std::array<VkDescriptorPoolSize, 2> poolSizes{};
  poolSizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  poolSizes[0].descriptorCount = static_cast<uint32_t>(kMaxFramesInFlight + 1);
  poolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  poolSizes[1].descriptorCount = static_cast<uint32_t>(kMaxRasterTextures + 1);

  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  poolInfo.maxSets       = static_cast<uint32_t>(kMaxFramesInFlight + 1 + kMaxRasterTextures + 1);
  poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
  poolInfo.pPoolSizes    = poolSizes.data();
  VK_CHECK(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_));
}

void VulkanBackend::createWaterMaskPool() {
  VkDescriptorPoolSize poolSize{};
  poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  poolSize.descriptorCount = static_cast<uint32_t>(kMaxRasterTextures + 1);

  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  poolInfo.maxSets       = static_cast<uint32_t>(kMaxRasterTextures + 1);
  poolInfo.poolSizeCount = 1;
  poolInfo.pPoolSizes    = &poolSize;
  VK_CHECK(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &waterMaskPool_));
}

// ── Fallback textures (synchronous path; only used at init) ────────────────────

void VulkanBackend::createFallbackTexture() {
  const uint8_t white[4] = {255, 255, 255, 255};
  VulkanTexture* t = createImageFromPixels(white, 1, 1,
                                           descriptorPool_,
                                           VK_SAMPLE_COUNT_1_BIT,
                                           /*wantMipmaps=*/false);
  if (t) {
    fallbackTexture_ = *t;
    fallbackTexturePtr_ = t;  // Keep alive - pending upload references this!
  }
}

void VulkanBackend::createWaterMaskFallback() {
  const uint8_t zero[4] = {0, 0, 0, 255};
  VulkanTexture* t = createImageFromPixels(zero, 1, 1,
                                           waterMaskPool_,
                                           VK_SAMPLE_COUNT_1_BIT,
                                           /*wantMipmaps=*/false);
  if (t) {
    fallbackWaterMaskTex_     = *t;
    fallbackWaterMaskDescSet_ = t->descriptorSet;
    fallbackWaterMaskTexPtr_ = t;  // Keep alive - pending upload references this!
  }
}

// ── Shader compilation ─────────────────────────────────────────────────────────

static VkShaderModule createShaderModule(VkDevice device, const uint32_t* code, uint32_t codeSize) {
  VkShaderModuleCreateInfo createInfo{};
  createInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  createInfo.codeSize = codeSize;
  createInfo.pCode    = code;

  VkShaderModule module;
  VK_CHECK(vkCreateShaderModule(device, &createInfo, nullptr, &module));
  return module;
}

// ── Pipeline creation (deduped via a small builder, P2-pipelines-dedupe) ──────

namespace {

struct PipelineBuildSpec {
  VkShaderModule vert;
  VkShaderModule frag;
  // Vertex input description — empty for sky.
  uint32_t bindingCount;
  const VkVertexInputBindingDescription*   bindings;
  uint32_t attributeCount;
  const VkVertexInputAttributeDescription* attributes;
  bool depthTest;
  bool depthWrite;
  VkCompareOp depthCompare;
  VkCullModeFlags cull;
  VkSampleCountFlagBits samples;
};

VkPipeline buildPipeline(VkDevice                 device,
                         VkPipelineLayout         layout,
                         VkRenderPass             renderPass,
                         VkPipelineCache          cache,
                         const PipelineBuildSpec& spec) {
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = spec.vert;
  stages[0].pName  = "main";
  stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = spec.frag;
  stages[1].pName  = "main";

  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInput.vertexBindingDescriptionCount   = spec.bindingCount;
  vertexInput.pVertexBindingDescriptions      = spec.bindings;
  vertexInput.vertexAttributeDescriptionCount = spec.attributeCount;
  vertexInput.pVertexAttributeDescriptions    = spec.attributes;

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo viewportState{};
  viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1;
  viewportState.scissorCount  = 1;

  VkPipelineRasterizationStateCreateInfo rasterizer{};
  rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.lineWidth   = 1.0f;
  rasterizer.cullMode    = spec.cull;
  rasterizer.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

  VkPipelineMultisampleStateCreateInfo multisampling{};
  multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisampling.rasterizationSamples = spec.samples;

  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable  = spec.depthTest  ? VK_TRUE : VK_FALSE;
  depthStencil.depthWriteEnable = spec.depthWrite ? VK_TRUE : VK_FALSE;
  depthStencil.depthCompareOp   = spec.depthCompare; // Reversed-Z (GREATER family)

  VkPipelineColorBlendAttachmentState colorBlendAttach{};
  colorBlendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

  VkPipelineColorBlendStateCreateInfo colorBlend{};
  colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlend.attachmentCount = 1;
  colorBlend.pAttachments    = &colorBlendAttach;

  VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamicState{};
  dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = 2;
  dynamicState.pDynamicStates    = dynStates;

  VkGraphicsPipelineCreateInfo pipelineInfo{};
  pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineInfo.stageCount          = 2;
  pipelineInfo.pStages             = stages;
  pipelineInfo.pVertexInputState   = &vertexInput;
  pipelineInfo.pInputAssemblyState = &inputAssembly;
  pipelineInfo.pViewportState      = &viewportState;
  pipelineInfo.pRasterizationState = &rasterizer;
  pipelineInfo.pMultisampleState   = &multisampling;
  pipelineInfo.pDepthStencilState  = &depthStencil;
  pipelineInfo.pColorBlendState    = &colorBlend;
  pipelineInfo.pDynamicState       = &dynamicState;
  pipelineInfo.layout              = layout;
  pipelineInfo.renderPass          = renderPass;
  pipelineInfo.subpass             = 0;

  VkPipeline result = VK_NULL_HANDLE;
  VK_CHECK(vkCreateGraphicsPipelines(device, cache, 1, &pipelineInfo, nullptr, &result));
  return result;
}

} // namespace

void VulkanBackend::createSkyPipeline() {
  VkShaderModule vertModule = createShaderModule(device_, spv_sky_vert, spv_sky_vert_size);
  VkShaderModule fragModule = createShaderModule(device_, spv_sky_frag, spv_sky_frag_size);

  PipelineBuildSpec spec{};
  spec.vert = vertModule;
  spec.frag = fragModule;
  spec.bindingCount   = 0;
  spec.bindings       = nullptr;
  spec.attributeCount = 0;
  spec.attributes     = nullptr;
  // Depth-test the sky against the terrain (which is drawn first and writes
  // depth) so the expensive atmospheric raymarch only runs on pixels that
  // aren't covered by geometry. sky.vert emits z=0 (the reversed-Z far plane)
  // and the depth buffer is cleared to 0, so GREATER_OR_EQUAL passes only where
  // no terrain wrote a closer (>0) depth. No depth write — the sky never
  // occludes anything.
  spec.depthTest      = true;
  spec.depthWrite     = false;
  spec.depthCompare   = VK_COMPARE_OP_GREATER_OR_EQUAL;
  spec.cull           = VK_CULL_MODE_NONE;
  spec.samples        = sampleCount_;
  skyPipeline_ = buildPipeline(device_, skyPipelineLayout_, renderPass_,
                               pipelineCache_, spec);

  vkDestroyShaderModule(device_, vertModule, nullptr);
  vkDestroyShaderModule(device_, fragModule, nullptr);
}

void VulkanBackend::createTerrainPipeline() {
  // Vertex bindings: position (vec3) | uv (vec2) | altitude (float).
  static const VkVertexInputBindingDescription bindings[3] = {
      {0, 3 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX},
      {1, 2 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX},
      {2,     sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX},
  };
  static const VkVertexInputAttributeDescription attrs[3] = {
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
      {1, 1, VK_FORMAT_R32G32_SFLOAT,    0},
      {2, 2, VK_FORMAT_R32_SFLOAT,       0},
  };

  VkShaderModule vertModule       = createShaderModule(
      device_, spv_terrain_vert, spv_terrain_vert_size);
  VkShaderModule fragSolidModule  = createShaderModule(
      device_, spv_terrain_frag, spv_terrain_frag_size);
  VkShaderModule fragDitherModule = createShaderModule(
      device_, spv_terrain_dither_frag, spv_terrain_dither_frag_size);

  auto buildOne = [&](VkShaderModule fragModule) {
    PipelineBuildSpec spec{};
    spec.vert = vertModule;
    spec.frag = fragModule;
    spec.bindingCount   = 3;
    spec.bindings       = bindings;
    spec.attributeCount = 3;
    spec.attributes     = attrs;
    spec.depthTest      = true;
    spec.depthWrite     = true;
    spec.depthCompare   = VK_COMPARE_OP_GREATER; // Reversed-Z
    spec.cull           = VK_CULL_MODE_BACK_BIT;
    spec.samples        = sampleCount_;
    return buildPipeline(device_, terrainPipelineLayout_, renderPass_,
                         pipelineCache_, spec);
  };

  // Solid pipeline: no LOD-fade discard in the fragment shader, so the GPU's
  // early-Z optimisation runs at full speed.  This is the pipeline used for
  // the overwhelming majority of draws outside an active LOD transition.
  terrainSolidPipeline_  = buildOne(fragSolidModule);
  // Dither pipeline: includes IGN-based discard.  Used only for draws where
  // DrawPrimitive::lodFade < 1.0f (see CesiumEngine::updateFrame which
  // partitions draws so the switch happens at most once per frame).
  terrainDitherPipeline_ = buildOne(fragDitherModule);

  vkDestroyShaderModule(device_, vertModule,       nullptr);
  vkDestroyShaderModule(device_, fragSolidModule,  nullptr);
  vkDestroyShaderModule(device_, fragDitherModule, nullptr);
}

void VulkanBackend::createGraphicsPipelinesAll() {
  createSkyPipeline();
  createTerrainPipeline();
}

void VulkanBackend::destroyGraphicsPipelinesAll() {
  if (terrainSolidPipeline_)  vkDestroyPipeline(device_, terrainSolidPipeline_,  nullptr);
  if (terrainDitherPipeline_) vkDestroyPipeline(device_, terrainDitherPipeline_, nullptr);
  if (skyPipeline_)           vkDestroyPipeline(device_, skyPipeline_,           nullptr);
  terrainSolidPipeline_  = VK_NULL_HANDLE;
  terrainDitherPipeline_ = VK_NULL_HANDLE;
  skyPipeline_           = VK_NULL_HANDLE;
}

// ── Memory / buffer helpers ─────────────────────────────────────────────────────

uint32_t VulkanBackend::findMemoryType(uint32_t typeFilter,
                                       VkMemoryPropertyFlags props,
                                       bool* outFound) {
  VkPhysicalDeviceMemoryProperties memProps;
  vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps);
  for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
    if ((typeFilter & (1 << i)) &&
        (memProps.memoryTypes[i].propertyFlags & props) == props) {
      if (outFound) *outFound = true;
      return i;
    }
  }
  if (outFound) {
    *outFound = false;
  } else {
    LOGE("Failed to find suitable memory type (filter=0x%x props=0x%x)",
         typeFilter, props);
  }
  return 0;
}

bool VulkanBackend::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                 VkMemoryPropertyFlags props,
                                 VkBuffer& buffer, VkDeviceMemory& memory) {
  VkBufferCreateInfo bufInfo{};
  bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufInfo.size  = size;
  bufInfo.usage = usage;
  bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(device_, &bufInfo, nullptr, &buffer) != VK_SUCCESS) {
    return false;
  }

  VkMemoryRequirements memReqs;
  vkGetBufferMemoryRequirements(device_, buffer, &memReqs);

  bool found = false;
  uint32_t typeIdx = findMemoryType(memReqs.memoryTypeBits, props, &found);
  if (!found) {
    LOGE("createBuffer: no compatible memory type for usage=0x%x props=0x%x",
         usage, props);
    vkDestroyBuffer(device_, buffer, nullptr);
    buffer = VK_NULL_HANDLE;
    return false;
  }

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize  = memReqs.size;
  allocInfo.memoryTypeIndex = typeIdx;
  if (vkAllocateMemory(device_, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
    vkDestroyBuffer(device_, buffer, nullptr);
    buffer = VK_NULL_HANDLE;
    return false;
  }
  if (vkBindBufferMemory(device_, buffer, memory, 0) != VK_SUCCESS) {
    vkDestroyBuffer(device_, buffer, nullptr);
    vkFreeMemory(device_, memory, nullptr);
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

void VulkanBackend::ensureBuffer(VkBuffer& buffer, VkDeviceMemory& memory,
                                 void*& mapped, size_t& capacity, size_t needed,
                                 const void* data, VkBufferUsageFlags usage) {
  if (needed == 0) return;
  if (needed > capacity) {
    if (mapped) { vkUnmapMemory(device_, memory); mapped = nullptr; }
    if (buffer) vkDestroyBuffer(device_, buffer, nullptr);
    if (memory) vkFreeMemory(device_, memory, nullptr);
    size_t newCap = std::max(needed, capacity > 0 ? capacity * 2 : needed);
    newCap = (newCap + 4095UL) & ~4095UL;
    if (!createBuffer(static_cast<VkDeviceSize>(newCap), usage,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      buffer, memory)) {
      capacity = 0;
      return;
    }
    capacity = newCap;
    if (vkMapMemory(device_, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
      mapped = nullptr;
      return;
    }
  }
  if (!mapped) return;
  memcpy(mapped, data, needed);
}

VkCommandBuffer VulkanBackend::beginSingleTimeCommands() {
  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandPool        = commandPool_;
  allocInfo.commandBufferCount = 1;

  VkCommandBuffer cmd = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(device_, &allocInfo, &cmd) != VK_SUCCESS) {
    LOGE("beginSingleTimeCommands: vkAllocateCommandBuffers failed");
    return VK_NULL_HANDLE;
  }

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
    LOGE("beginSingleTimeCommands: vkBeginCommandBuffer failed");
    vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
    return VK_NULL_HANDLE;
  }
  return cmd;
}

bool VulkanBackend::endSingleTimeCommands(VkCommandBuffer cmd) {
  if (cmd == VK_NULL_HANDLE) return false;
  vkEndCommandBuffer(cmd);

  VkSubmitInfo submitInfo{};
  submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers    = &cmd;

  VkResult r = vkQueueSubmit(graphicsQueue_, 1, &submitInfo, VK_NULL_HANDLE);
  if (r != VK_SUCCESS) {
    LOGE("endSingleTimeCommands: vkQueueSubmit failed: %d", r);
    vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
    return false;
  }
  // We still wait here because this path is reserved for init / fallback. The
  // hot per-tile upload path uses the deferred staging ring instead.
  vkQueueWaitIdle(graphicsQueue_);
  vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
  return true;
}

// ── Mipmap generation ──────────────────────────────────────────────────────────

void VulkanBackend::recordGenerateMipmaps(VkCommandBuffer cmd, VkImage image,
                                          uint32_t width, uint32_t height,
                                          uint32_t mipLevels) {
  // Pre-condition: every level is in TRANSFER_DST_OPTIMAL.
  // Post-condition: every level is in SHADER_READ_ONLY_OPTIMAL.
  int32_t mipW = static_cast<int32_t>(width);
  int32_t mipH = static_cast<int32_t>(height);

  for (uint32_t i = 1; i < mipLevels; ++i) {
    // Transition source mip (i-1) to TRANSFER_SRC.
    VkImageMemoryBarrier toSrc{};
    toSrc.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.image               = image;
    toSrc.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    toSrc.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toSrc.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &toSrc);

    VkImageBlit blit{};
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = {mipW, mipH, 1};
    blit.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.mipLevel       = i - 1;
    blit.srcSubresource.baseArrayLayer = 0;
    blit.srcSubresource.layerCount     = 1;
    blit.dstOffsets[0] = {0, 0, 0};
    blit.dstOffsets[1] = {std::max(1, mipW / 2), std::max(1, mipH / 2), 1};
    blit.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.mipLevel       = i;
    blit.dstSubresource.baseArrayLayer = 0;
    blit.dstSubresource.layerCount     = 1;
    vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_LINEAR);

    // Source mip (i-1) → SHADER_READ.
    VkImageMemoryBarrier toShader{};
    toShader.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toShader.image               = image;
    toShader.srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    toShader.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    toShader.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toShader.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &toShader);

    if (mipW > 1) mipW /= 2;
    if (mipH > 1) mipH /= 2;
  }

  // Last mip → SHADER_READ.
  VkImageMemoryBarrier last{};
  last.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  last.image               = image;
  last.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
  last.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
  last.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  last.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  last.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  last.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  last.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, mipLevels - 1, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &last);
}

// ── Persistent staging ring ────────────────────────────────────────────────────

void VulkanBackend::initStagingRing(VkDeviceSize bytes) {
  if (!createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    stagingBuffer_, stagingMemory_)) {
    LOGE("Staging ring allocation failed (size=%llu) — falling back to "
         "synchronous per-tile upload",
         static_cast<unsigned long long>(bytes));
    return;
  }
  void* mapped = nullptr;
  if (vkMapMemory(device_, stagingMemory_, 0, bytes, 0, &mapped) != VK_SUCCESS) {
    LOGE("Staging ring vkMapMemory failed");
    vkDestroyBuffer(device_, stagingBuffer_, nullptr);
    vkFreeMemory(device_, stagingMemory_, nullptr);
    stagingBuffer_ = VK_NULL_HANDLE;
    stagingMemory_ = VK_NULL_HANDLE;
    return;
  }
  stagingMapped_         = static_cast<uint8_t*>(mapped);
  stagingHead_           = 0;
  stagingBytesThisBatch_ = 0;
}

VkDeviceSize VulkanBackend::allocateStagingSlice(VkDeviceSize size) {
  if (!stagingMapped_ || size == 0 || size > kStagingSize) return SIZE_MAX;

  VkDeviceSize aligned = (size + kStagingAlignment - 1) & ~(kStagingAlignment - 1);

  // per-batch budget. Refuse the allocation if granting it would let a
  // single beginFrame->flushPendingUploads cycle eat more than 1/N of the
  // ring. The caller (createImageFromPixels) falls back to the synchronous
  // single-tile path for that tile so the rest of the batch still goes via
  // the async ring.
  if (stagingBytesThisBatch_ + aligned > kStagingPerFrameBudget) {
    return SIZE_MAX;
  }

  VkDeviceSize off;
  if (stagingHead_ + aligned > kStagingSize) {
    off = 0;
    stagingHead_ = aligned;
  } else {
    off = stagingHead_;
    stagingHead_ += aligned;
  }
  stagingBytesThisBatch_ += aligned;
  return off;
}

void VulkanBackend::flushPendingUploads() {
  std::vector<PendingUpload> uploads;
  {
    std::lock_guard<std::mutex> lk(stagingMutex_);
    if (pendingUploadsCurrent_.empty()) {
      return;
    }
    uploads.swap(pendingUploadsCurrent_);
  }

  VkCommandBuffer cmd = uploadCommandBuffers_[frameIndex_];
  if (cmd == VK_NULL_HANDLE) {
    LOGE("flushPendingUploads: upload command buffer is NULL!");
    return;
  }

  vkResetCommandBuffer(cmd, 0);

  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &bi);

  // Phase 1: batched UNDEFINED → TRANSFER_DST_OPTIMAL barriers.
  std::vector<VkImageMemoryBarrier> toTransfer;
  toTransfer.reserve(uploads.size());
  for (const auto& u : uploads) {
    // Defensive: mipLevels should be 1-15 for any reasonable texture.
    uint32_t safeMipLevels = u.tex->mipLevels;
    if (safeMipLevels == 0 || safeMipLevels > 16) {
      LOGE("flushPendingUploads: corrupt mipLevels=%u for tex=%p, clamping to 1",
           safeMipLevels, (void*)u.tex);
      safeMipLevels = 1;
      u.tex->mipLevels = 1;
    }

    VkImageMemoryBarrier b{};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask       = 0;
    b.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = u.tex->image;
    b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, safeMipLevels, 0, 1};
    toTransfer.push_back(b);
  }
  if (!toTransfer.empty()) {
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr,
        static_cast<uint32_t>(toTransfer.size()), toTransfer.data());
  }

  // Phase 2: per-image vkCmdCopyBufferToImage from the staging ring.
  if (stagingBuffer_ == VK_NULL_HANDLE) {
    LOGE("flushPendingUploads: stagingBuffer_ is NULL! Aborting upload");
    vkEndCommandBuffer(cmd);
    return;
  }
  for (const auto& u : uploads) {
    VkBufferImageCopy region{};
    region.bufferOffset      = u.stagingOffset;
    region.bufferRowLength   = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource  = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent       = {u.width, u.height, 1};
    vkCmdCopyBufferToImage(cmd, stagingBuffer_, u.tex->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  }

  // Phase 3: per-image mipmap chain (only for textures that asked for it) +
  // a batched TRANSFER_DST → SHADER_READ for the rest.
  std::vector<VkImageMemoryBarrier> toShader;
  toShader.reserve(uploads.size());
  for (const auto& u : uploads) {
    if (u.tex->mipLevels > 1) {
      recordGenerateMipmaps(cmd, u.tex->image, u.width, u.height,
                            u.tex->mipLevels);
    } else {
      VkImageMemoryBarrier b{};
      b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      b.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
      b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
      b.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      b.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image               = u.tex->image;
      b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      toShader.push_back(b);
    }
  }
  if (!toShader.empty()) {
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr,
        static_cast<uint32_t>(toShader.size()), toShader.data());
  }

  vkEndCommandBuffer(cmd);

  // async submit. The fence lets beginFrame reclaim the staging slices in
  // kMaxFramesInFlight frames' time without ever calling vkQueueWaitIdle.
  // Ordering: this submit precedes endFrame()'s draw submit on the same
  // graphicsQueue_, so the image layout transitions recorded above are
  // visible to subsequent fragment-shader reads via the standard in-queue
  // execution + memory-barrier rules — no extra semaphore needed.
  vkResetFences(device_, 1, &uploadFences_[frameIndex_]);

  VkSubmitInfo si{};
  si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers    = &cmd;

  VkResult submitResult =
      vkQueueSubmit(graphicsQueue_, 1, &si, uploadFences_[frameIndex_]);
  if (submitResult != VK_SUCCESS) {
    LOGE("flushPendingUploads: vkQueueSubmit failed: %d", submitResult);
    return;
  }

  // Mark each texture as visible to subsequent draws (no longer pending).
  // Safe to flip here because the next draw submit happens after this one on
  // the same queue, and the upload CB ends in a TRANSFER → FRAGMENT_SHADER
  // memory barrier.
  for (auto& u : uploads) u.tex->pendingUpload = false;

  // Move uploads into this slot's in-flight list so beginFrame can free
  // their staging slices once uploadFences_[frameIndex_] is signalled (which
  // happens at most kMaxFramesInFlight frames from now).
  {
    std::lock_guard<std::mutex> lk(stagingMutex_);
    auto& inflight = uploadsInFlight_[frameIndex_];
    inflight.insert(inflight.end(), uploads.begin(), uploads.end());
    stagingBytesThisBatch_ = 0;
  }
}

// ── Generic image creator (P2-pipelines-dedupe) ─────────────────────────────

VulkanTexture* VulkanBackend::createImageFromPixels(const uint8_t* pixels,
                                                    int32_t width, int32_t height,
                                                    VkDescriptorPool pool,
                                                    VkSampleCountFlagBits samples,
                                                    bool wantMipmaps) {
  if (!device_ || !pixels || width <= 0 || height <= 0 || pool == VK_NULL_HANDLE)
    return nullptr;

  auto* tex = new VulkanTexture();
  tex->width  = static_cast<uint32_t>(width);
  tex->height = static_cast<uint32_t>(height);
  tex->mipLevels = wantMipmaps
      ? static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1u
      : 1u;

  VkImageCreateInfo imgInfo{};
  imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imgInfo.imageType     = VK_IMAGE_TYPE_2D;
  imgInfo.format        = VK_FORMAT_R8G8B8A8_UNORM;
  imgInfo.extent        = {tex->width, tex->height, 1};
  imgInfo.mipLevels     = tex->mipLevels;
  imgInfo.arrayLayers   = 1;
  imgInfo.samples       = samples;
  imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
  imgInfo.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT |  // mipmap blit src
                          VK_IMAGE_USAGE_SAMPLED_BIT;
  imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(device_, &imgInfo, nullptr, &tex->image) != VK_SUCCESS) {
    delete tex;
    return nullptr;
  }

  VkMemoryRequirements memReqs;
  vkGetImageMemoryRequirements(device_, tex->image, &memReqs);

  bool typeFound = false;
  uint32_t typeIdx = findMemoryType(memReqs.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                    &typeFound);
  if (!typeFound) {
    LOGE("createImageFromPixels: no DEVICE_LOCAL memory type available");
    vkDestroyImage(device_, tex->image, nullptr);
    delete tex;
    return nullptr;
  }
  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize  = memReqs.size;
  allocInfo.memoryTypeIndex = typeIdx;
  if (vkAllocateMemory(device_, &allocInfo, nullptr, &tex->memory) != VK_SUCCESS) {
    vkDestroyImage(device_, tex->image, nullptr);
    delete tex;
    return nullptr;
  }
  vkBindImageMemory(device_, tex->image, tex->memory, 0);

  // Try the deferred staging path first; fall back to synchronous if the ring
  // is full or we are still pre-init (no command buffers yet).
  const VkDeviceSize imageBytes =
      static_cast<VkDeviceSize>(width) * height * 4;
  bool deferred = false;
  if (stagingMapped_ != nullptr && commandPool_ != VK_NULL_HANDLE) {
    std::lock_guard<std::mutex> lk(stagingMutex_);
    VkDeviceSize off = allocateStagingSlice(imageBytes);
    if (off != static_cast<VkDeviceSize>(SIZE_MAX) && off + imageBytes <= kStagingSize) {
      memcpy(stagingMapped_ + off, pixels, static_cast<size_t>(imageBytes));
      tex->pendingUpload = true;
      pendingUploadsCurrent_.push_back(
          {tex, off, tex->width, tex->height, totalFrameCount_});
      deferred = true;
    }
  }

  if (!deferred) {
    // Synchronous fallback (init-time or staging-ring exhausted).
    VkBuffer staging;
    VkDeviceMemory stagingMem;
    if (!createBuffer(imageBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      staging, stagingMem)) {
      vkDestroyImage(device_, tex->image, nullptr);
      vkFreeMemory(device_, tex->memory, nullptr);
      delete tex;
      return nullptr;
    }
    void* mapped = nullptr;
    vkMapMemory(device_, stagingMem, 0, imageBytes, 0, &mapped);
    memcpy(mapped, pixels, static_cast<size_t>(imageBytes));
    vkUnmapMemory(device_, stagingMem);

    VkCommandBuffer cmd = beginSingleTimeCommands();
    if (cmd != VK_NULL_HANDLE) {
      VkImageMemoryBarrier toTransfer{};
      toTransfer.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      toTransfer.srcAccessMask       = 0;
      toTransfer.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
      toTransfer.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
      toTransfer.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      toTransfer.image               = tex->image;
      toTransfer.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, tex->mipLevels, 0, 1};
      vkCmdPipelineBarrier(cmd,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          0, 0, nullptr, 0, nullptr, 1, &toTransfer);

      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageExtent      = {tex->width, tex->height, 1};
      vkCmdCopyBufferToImage(cmd, staging, tex->image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

      if (tex->mipLevels > 1) {
        recordGenerateMipmaps(cmd, tex->image, tex->width, tex->height,
                              tex->mipLevels);
      } else {
        VkImageMemoryBarrier toShader{};
        toShader.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toShader.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        toShader.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        toShader.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toShader.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toShader.image               = tex->image;
        toShader.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toShader);
      }
      endSingleTimeCommands(cmd);
    }
    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMem, nullptr);
  }

  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image    = tex->image;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format   = VK_FORMAT_R8G8B8A8_UNORM;
  viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, tex->mipLevels, 0, 1};
  vkCreateImageView(device_, &viewInfo, nullptr, &tex->imageView);

  VkSamplerCreateInfo samplerInfo{};
  samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  samplerInfo.magFilter    = VK_FILTER_LINEAR;
  samplerInfo.minFilter    = VK_FILTER_LINEAR;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  samplerInfo.minLod       = 0.0f;
  samplerInfo.maxLod       = static_cast<float>(tex->mipLevels);
  if (supportsAnisotropy_) {
    samplerInfo.anisotropyEnable = VK_TRUE;
    samplerInfo.maxAnisotropy    = std::min(8.0f, maxAnisotropy_);
  }
  vkCreateSampler(device_, &samplerInfo, nullptr, &tex->sampler);

  // Allocate a descriptor set for this texture.
  VkDescriptorSetAllocateInfo dsAllocInfo{};
  dsAllocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  dsAllocInfo.descriptorPool     = pool;
  dsAllocInfo.descriptorSetCount = 1;
  dsAllocInfo.pSetLayouts        = &terrainTexLayout_;
  VkResult dsResult = vkAllocateDescriptorSets(device_, &dsAllocInfo, &tex->descriptorSet);
  if (dsResult != VK_SUCCESS) {
    LOGE("vkAllocateDescriptorSets failed: %d (pool exhausted?)", dsResult);
    tex->descriptorSet = VK_NULL_HANDLE;
  } else {
    VkDescriptorImageInfo dsImgInfo{};
    dsImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dsImgInfo.imageView   = tex->imageView;
    dsImgInfo.sampler     = tex->sampler;

    VkWriteDescriptorSet texWrite{};
    texWrite.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    texWrite.dstSet          = tex->descriptorSet;
    texWrite.dstBinding      = 0;
    texWrite.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    texWrite.descriptorCount = 1;
    texWrite.pImageInfo      = &dsImgInfo;
    vkUpdateDescriptorSets(device_, 1, &texWrite, 0, nullptr);
  }
  return tex;
}

// ── Public texture creators ─────────────────────────────────────────────────────

void* VulkanBackend::createRasterTexture(const uint8_t* pixels, int32_t width, int32_t height) {
  return createImageFromPixels(pixels, width, height,
                               descriptorPool_, VK_SAMPLE_COUNT_1_BIT,
                               /*wantMipmaps=*/true);
}
void VulkanBackend::freeRasterTexture(void* texPtr) {
  if (!texPtr || !device_) return;
  auto* tex = static_cast<VulkanTexture*>(texPtr);
  std::lock_guard<std::mutex> lk(pendingDeletesMutex_);
  pendingDeletes_.push_back({tex, totalFrameCount_, descriptorPool_});
}
void* VulkanBackend::createWaterMaskTexture(const uint8_t* pixels, int32_t width, int32_t height) {
  // Water mask is downsampled hi-frequency data; mipmaps add real value here too.
  return createImageFromPixels(pixels, width, height,
                               waterMaskPool_, VK_SAMPLE_COUNT_1_BIT,
                               /*wantMipmaps=*/true);
}
void VulkanBackend::freeWaterMaskTexture(void* texPtr) {
  if (!texPtr || !device_) return;
  auto* tex = static_cast<VulkanTexture*>(texPtr);
  std::lock_guard<std::mutex> lk(pendingDeletesMutex_);
  pendingDeletes_.push_back({tex, totalFrameCount_, waterMaskPool_});
}

void VulkanBackend::destroyTexture(VulkanTexture* tex, VkDescriptorPool pool) {
  if (!tex) return;
  if (tex->descriptorSet && pool != VK_NULL_HANDLE) {
    vkFreeDescriptorSets(device_, pool, 1, &tex->descriptorSet);
  }
  if (tex->sampler)   vkDestroySampler(device_, tex->sampler, nullptr);
  if (tex->imageView) vkDestroyImageView(device_, tex->imageView, nullptr);
  if (tex->image)     vkDestroyImage(device_, tex->image, nullptr);
  if (tex->memory)    vkFreeMemory(device_, tex->memory, nullptr);
  delete tex;
}

void VulkanBackend::flushPendingDeletes() {
  std::lock_guard<std::mutex> lk(pendingDeletesMutex_);
  while (!pendingDeletes_.empty()) {
    const auto& front = pendingDeletes_.front();
    if (totalFrameCount_ < front.frameIndex + static_cast<uint64_t>(kMaxFramesInFlight))
      break;
    destroyTexture(front.tex, front.pool);
    pendingDeletes_.pop_front();
  }
}

void VulkanBackend::setMsaaSampleCount(int sampleCount) {
  // Defer MSAA changes to the next frame boundary so we don't tear down the
  // render pass mid-frame. Stash the request; beginFrame applies it before any
  // GPU work for the frame.
  pendingMsaaRequest_ = sampleCount;
}

// ── Swapchain recreation ───────────────────────────────────────────────────────

void VulkanBackend::cleanupSwapchain() {
  for (auto& fb : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
  framebuffers_.clear();

  if (msaaColorView_)   vkDestroyImageView(device_, msaaColorView_, nullptr);
  if (msaaColorImage_)  vkDestroyImage(device_, msaaColorImage_, nullptr);
  if (msaaColorMemory_) vkFreeMemory(device_, msaaColorMemory_, nullptr);
  msaaColorView_ = VK_NULL_HANDLE; msaaColorImage_ = VK_NULL_HANDLE; msaaColorMemory_ = VK_NULL_HANDLE;

  if (depthImageView_) vkDestroyImageView(device_, depthImageView_, nullptr);
  if (depthImage_)     vkDestroyImage(device_, depthImage_, nullptr);
  if (depthMemory_)    vkFreeMemory(device_, depthMemory_, nullptr);
  depthImageView_ = VK_NULL_HANDLE;
  depthImage_     = VK_NULL_HANDLE;
  depthMemory_    = VK_NULL_HANDLE;

  for (auto& iv : swapchainImageViews_) vkDestroyImageView(device_, iv, nullptr);
  swapchainImageViews_.clear();

  if (swapchain_) {
    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
  }
}

void VulkanBackend::recreateSwapchain() {
  // P2-vk-recreate: drain only the in-flight frames, not the whole device.
  if (!inFlightFences_.empty()) {
    vkWaitForFences(device_, static_cast<uint32_t>(inFlightFences_.size()),
                    inFlightFences_.data(), VK_TRUE, UINT64_MAX);
  }
  cleanupSwapchain();
  createSwapchain();
  createDepthResources();
  if (sampleCount_ != VK_SAMPLE_COUNT_1_BIT) createMsaaResources();
  createFramebuffers();

  // The per-image semaphore array is keyed on swapchain image index, which
  // can change with image count. Resize it to match.
  for (auto& s : renderFinishedSemaphores_) {
    if (s) vkDestroySemaphore(device_, s, nullptr);
  }
  renderFinishedSemaphores_.assign(swapchainImages_.size(), VK_NULL_HANDLE);
  VkSemaphoreCreateInfo si{}; si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  for (auto& s : renderFinishedSemaphores_) {
    VK_CHECK(vkCreateSemaphore(device_, &si, nullptr, &s));
  }

  needsSwapchainRecreate_ = false;
}

// ── Per-frame rendering ────────────────────────────────────────────────────────

void VulkanBackend::beginFrame(const FrameParams& /*params*/) {
  if (device_ == VK_NULL_HANDLE) return;

  // On Android, defer swapchain creation until the first frame to ensure the
  // surface compositor is fully ready. Creating it too early can cause
  // vkAcquireNextImageKHR to block indefinitely.
  if (swapchain_ == VK_NULL_HANDLE) {
    createSwapchain();

    // Create renderFinishedSemaphores now that we know swapchainImages_.size()
    renderFinishedSemaphores_.resize(swapchainImages_.size());
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (size_t i = 0; i < renderFinishedSemaphores_.size(); ++i) {
      VK_CHECK(vkCreateSemaphore(device_, &semInfo, nullptr, &renderFinishedSemaphores_[i]));
    }

    createRenderPass();
    createDepthResources();
    if (sampleCount_ != VK_SAMPLE_COUNT_1_BIT) createMsaaResources();
    createFramebuffers();
    createGraphicsPipelinesAll();
  }

  // Apply pending MSAA change between frames (safe — the previous frame's
  // command buffer has finished by the time we wait on the fence below).
  if (pendingMsaaRequest_ != 0) {
    const VkSampleCountFlagBits target = resolveMsaaSamples(pendingMsaaRequest_);
    pendingMsaaRequest_ = 0;
    if (target != sampleCount_) {
      vkDeviceWaitIdle(device_); // bounded; one-shot when MSAA toggled
      destroyGraphicsPipelinesAll();
      if (renderPass_) { vkDestroyRenderPass(device_, renderPass_, nullptr); renderPass_ = VK_NULL_HANDLE; }
      // Need to free the framebuffers + depth + msaa color since attachments
      // now have a different sample count.
      for (auto& fb : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
      framebuffers_.clear();
      if (depthImageView_) { vkDestroyImageView(device_, depthImageView_, nullptr); depthImageView_ = VK_NULL_HANDLE; }
      if (depthImage_)     { vkDestroyImage(device_, depthImage_, nullptr); depthImage_ = VK_NULL_HANDLE; }
      if (depthMemory_)    { vkFreeMemory(device_, depthMemory_, nullptr); depthMemory_ = VK_NULL_HANDLE; }
      if (msaaColorView_)  { vkDestroyImageView(device_, msaaColorView_, nullptr); msaaColorView_ = VK_NULL_HANDLE; }
      if (msaaColorImage_) { vkDestroyImage(device_, msaaColorImage_, nullptr); msaaColorImage_ = VK_NULL_HANDLE; }
      if (msaaColorMemory_){ vkFreeMemory(device_, msaaColorMemory_, nullptr); msaaColorMemory_ = VK_NULL_HANDLE; }

      sampleCount_ = target;
      createRenderPass();
      createDepthResources();
      if (sampleCount_ != VK_SAMPLE_COUNT_1_BIT) createMsaaResources();
      createFramebuffers();
      createGraphicsPipelinesAll();
    }
  }

  if (needsSwapchainRecreate_) {
    recreateSwapchain();
  }

  vkWaitForFences(device_, 1, &inFlightFences_[frameIndex_], VK_TRUE, UINT64_MAX);

  ++totalFrameCount_;
  flushPendingDeletes();

  // reclaim staging slices owned by this slot's previous async upload
  // submit. The upload fence is created SIGNALED, so the first wait through
  // each slot is a no-op; on subsequent passes it ensures the GPU has finished
  // reading those staging bytes before we let allocateStagingSlice hand them
  // back out.
  if (uploadFences_[frameIndex_] != VK_NULL_HANDLE) {
    vkWaitForFences(device_, 1, &uploadFences_[frameIndex_], VK_TRUE, UINT64_MAX);
  }
  {
    std::lock_guard<std::mutex> lk(stagingMutex_);
    uploadsInFlight_[frameIndex_].clear();
  }

  // Submit any uploads queued since last beginFrame.
  flushPendingUploads();

  // Use a 1-second timeout instead of UINT64_MAX to avoid indefinite hangs.
  // On a properly functioning swapchain, this returns immediately.
  VkResult result = vkAcquireNextImageKHR(device_, swapchain_, 1000000000ULL,
                                          imageAvailableSemaphores_[frameIndex_],
                                          VK_NULL_HANDLE, &imageIndex_);

  if (result == VK_TIMEOUT) {
    LOGE("VulkanBackend::beginFrame: vkAcquireNextImageKHR timed out");
    frameBegan_ = false;
    return;
  }
  if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
    recreateSwapchain();
    result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                   imageAvailableSemaphores_[frameIndex_],
                                   VK_NULL_HANDLE, &imageIndex_);
  }
  if (result != VK_SUCCESS) {
    frameBegan_ = false;
    return;
  }

  vkResetFences(device_, 1, &inFlightFences_[frameIndex_]);
  vkResetCommandBuffer(commandBuffers_[frameIndex_], 0);

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  vkBeginCommandBuffer(commandBuffers_[frameIndex_], &beginInfo);

  std::array<VkClearValue, 3> clearValues{};
  clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  clearValues[1].depthStencil = {0.0f, 0}; // Reversed-Z: 0 = far
  clearValues[2].color = {{0.0f, 0.0f, 0.0f, 1.0f}};

  VkRenderPassBeginInfo rpBegin{};
  rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rpBegin.renderPass        = renderPass_;
  rpBegin.framebuffer       = framebuffers_[imageIndex_];
  rpBegin.renderArea.offset = {0, 0};
  rpBegin.renderArea.extent = swapchainExtent_;
  rpBegin.clearValueCount   = (sampleCount_ != VK_SAMPLE_COUNT_1_BIT) ? 3u : 2u;
  rpBegin.pClearValues      = clearValues.data();

  vkCmdBeginRenderPass(commandBuffers_[frameIndex_], &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

  // Negative viewport height flips Y to match the OpenGL/Metal clip-space
  // convention that the shared C++ projection matrix targets (Vulkan 1.1+).
  VkViewport viewport{};
  viewport.x        = 0.0f;
  viewport.y        = static_cast<float>(swapchainExtent_.height);
  viewport.width    = static_cast<float>(swapchainExtent_.width);
  viewport.height   = -static_cast<float>(swapchainExtent_.height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(commandBuffers_[frameIndex_], 0, 1, &viewport);

  VkRect2D scissor{};
  scissor.offset = {0, 0};
  scissor.extent = swapchainExtent_;
  vkCmdSetScissor(commandBuffers_[frameIndex_], 0, 1, &scissor);

  frameBegan_ = true;
}

void VulkanBackend::drawScene(const FrameResult& frame) {
  if (!frameBegan_) return;

  VkCommandBuffer cmd = commandBuffers_[frameIndex_];

  // ── Terrain pass (opaque, writes depth) ─────────────────────────────────────
  // Drawn FIRST so the sky pass below can depth-test against the terrain and
  // skip the expensive full-screen atmospheric raymarch on every covered pixel.
  // Wrapped in a lambda so its early-outs still fall through to the sky pass.
  auto drawTerrain = [&]() {
  if (frame.draws.empty() || !terrainSolidPipeline_ || !terrainDitherPipeline_) {
    return;
  }

  const int fi = frameIndex_;

  // check the per-slot geometry signature cache. When it matches we
  // know the persistent vtx/idx/uv/alt buffers already hold the right bytes
  // and the engine intentionally left the merged CPU arrays empty.
  const uint64_t sig         = frame.geometrySignature;
  const bool     slotMatches = (sig != 0ULL) && (sig == geomSlotSig_[fi]);

  if (!slotMatches && (frame.localPositions.empty() || frame.indices.empty() ||
                       frame.altitudes.empty())) {
    return;
  }

  if (!slotMatches) {
    const size_t vtxBytes = frame.localPositions.size() * sizeof(float);
    const size_t idxBytes = frame.indices.size()        * sizeof(uint32_t);
    const size_t uvBytes  = frame.uvs.size()            * sizeof(float);
    const size_t altBytes = frame.altitudes.size()      * sizeof(float);

    ensureBuffer(vtxBufs_[fi], vtxMems_[fi], vtxMapped_[fi], vtxCaps_[fi],
                 vtxBytes, frame.localPositions.data(),
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    ensureBuffer(idxBufs_[fi], idxMems_[fi], idxMapped_[fi], idxCaps_[fi],
                 idxBytes, frame.indices.data(),
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    ensureBuffer(uvBufs_[fi], uvMems_[fi], uvMapped_[fi], uvCaps_[fi],
                 uvBytes, frame.uvs.data(),
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    ensureBuffer(altBufs_[fi], altMems_[fi], altMapped_[fi], altCaps_[fi],
                 altBytes, frame.altitudes.data(),
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    // Record what this slot now holds — set only after every buffer has
    // been refreshed. If `sig == 0` we deliberately propagate it so future
    // frames re-upload on this slot (e.g. an active fade is invalidating
    // the cache).
    geomSlotSig_[fi] = sig;
  }

  if (!vtxBufs_[fi] || !idxBufs_[fi]) return;

  TerrainUBO terrU{};
  terrU.cameraEcef[0] = frame.cameraEcef.x;
  terrU.cameraEcef[1] = frame.cameraEcef.y;
  terrU.cameraEcef[2] = frame.cameraEcef.z;
  terrU.cameraEcef[3] = 0.0f;
  memcpy(terrainUboMapped_[fi], &terrU, sizeof(TerrainUBO));

  // Start with the solid pipeline. CesiumEngine has stable_partition-ed draws so
  // every lodFade==1 draw is first; we only switch to the dither pipeline once,
  // when the first fading draw is reached.
  VkPipeline currentPipeline = terrainSolidPipeline_;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, currentPipeline);

  VkBuffer     vertexBuffers[] = {vtxBufs_[fi], uvBufs_[fi], altBufs_[fi]};
  VkDeviceSize offsets[]       = {0, 0, 0};
  vkCmdBindVertexBuffers(cmd, 0, 3, vertexBuffers, offsets);
  vkCmdBindIndexBuffer(cmd, idxBufs_[fi], 0, VK_INDEX_TYPE_UINT32);

  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, terrainPipelineLayout_,
                          0, 1, &terrainDescSets_[fi], 0, nullptr);

  VkDescriptorSet lastTexSet      = fallbackTexDescSet_;
  VkDescriptorSet lastWaterMaskSet = fallbackWaterMaskDescSet_;
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, terrainPipelineLayout_,
                          1, 1, &lastTexSet, 0, nullptr);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, terrainPipelineLayout_,
                          2, 1, &lastWaterMaskSet, 0, nullptr);

  // Combined push-constant block: vertex bytes 0-63, fragment bytes 64-127.
  // Issued once per draw instead of two separate vkCmdPushConstants calls
  // (P2-arg-buffers).
  struct CombinedPC {
    TerrainVertexPC   v;
    TerrainFragmentPC f;
  };
  static_assert(sizeof(CombinedPC) == 128, "PC layout drift");
  CombinedPC pc{};

  for (const auto& draw : frame.draws) {
    if (draw.indexCount == 0) continue;

    // ── Pipeline switch for LOD-fade dither ─────────────────────────────────
    // Solid pipeline (no discard) for fully-visible tiles keeps Adreno/Mali
    // early-Z enabled at full speed; dither pipeline only used for actively-
    // fading tiles. Draws are partitioned so this switch fires at most once
    // per frame.
    VkPipeline wantedPipeline = (draw.lodFade < 1.0f)
        ? terrainDitherPipeline_ : terrainSolidPipeline_;
    if (wantedPipeline != currentPipeline) {
      currentPipeline = wantedPipeline;
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, currentPipeline);
    }

    const float* mp = glm::value_ptr(draw.mvpMatrix);
    for (int i = 0; i < 16; ++i) pc.v.mvpMatrix[i] = mp[i];

    pc.f.hasOverlay          = (draw.overlayTexture && draw.hasUVs) ? 1u : 0u;
    pc.f.isEllipsoidFallback = draw.isEllipsoidFallback ? 1u : 0u;
    pc.f.isOnlyWater         = draw.isOnlyWater ? 1u : 0u;
    pc.f.hasWaterMask        = (draw.waterMaskTexture != nullptr) ? 1u : 0u;
    pc.f.wmWest              = draw.wmTileBounds.x;
    pc.f.wmSouth             = draw.wmTileBounds.y;
    pc.f.wmEast              = draw.wmTileBounds.z;
    pc.f.wmNorth             = draw.wmTileBounds.w;
    pc.f.rtcCenterX          = draw.rtcCenterEcef.x;
    pc.f.rtcCenterY          = draw.rtcCenterEcef.y;
    pc.f.rtcCenterZ          = draw.rtcCenterEcef.z;
    pc.f.translationX        = draw.overlayTranslation.x;
    pc.f.translationY        = draw.overlayTranslation.y;
    pc.f.scaleX              = draw.overlayScale.x;
    pc.f.scaleY              = draw.overlayScale.y;
    pc.f.lodFade             = draw.lodFade;
    vkCmdPushConstants(cmd, terrainPipelineLayout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(CombinedPC), &pc);

    VkDescriptorSet texSet = fallbackTexDescSet_;
    if (draw.overlayTexture && draw.hasUVs) {
      auto* tex = static_cast<VulkanTexture*>(draw.overlayTexture);
      // Skip textures still pending their first GPU upload — would read
      // undefined data otherwise. Falls back to white for one frame.
      if (tex && tex->descriptorSet && !tex->pendingUpload) {
        texSet = tex->descriptorSet;
      }
    }
    if (texSet != lastTexSet) {
      lastTexSet = texSet;
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, terrainPipelineLayout_,
                              1, 1, &lastTexSet, 0, nullptr);
    }

    VkDescriptorSet wmSet = fallbackWaterMaskDescSet_;
    if (draw.waterMaskTexture) {
      auto* wmTex = static_cast<VulkanTexture*>(draw.waterMaskTexture);
      if (wmTex && wmTex->descriptorSet && !wmTex->pendingUpload) {
        wmSet = wmTex->descriptorSet;
      }
    }
    if (wmSet != lastWaterMaskSet) {
      lastWaterMaskSet = wmSet;
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, terrainPipelineLayout_,
                              2, 1, &lastWaterMaskSet, 0, nullptr);
    }

    uint32_t firstIndex = draw.indexByteOffset / sizeof(uint32_t);
    vkCmdDrawIndexed(cmd, draw.indexCount, 1, firstIndex, 0, 0);
  }
  }; // drawTerrain
  drawTerrain();

  // ── Sky pass ────────────────────────────────────────────────────────────────
  // Drawn LAST and depth-tested (no write): the raymarch only executes on pixels
  // where no terrain was rendered (depth still at the cleared reversed-Z far
  // plane). This avoids shading the full screen and then overdrawing it.
  if (skyPipeline_ && skyDescSet_) {
    SkyUBO skyU{};
    const float* iv = glm::value_ptr(frame.invVP);
    for (int i = 0; i < 16; ++i) skyU.invVP[i] = iv[i];
    skyU.cameraEcef[0] = frame.cameraEcef.x;
    skyU.cameraEcef[1] = frame.cameraEcef.y;
    skyU.cameraEcef[2] = frame.cameraEcef.z;
    float cl = sqrtf(frame.cameraEcef.x * frame.cameraEcef.x +
                     frame.cameraEcef.y * frame.cameraEcef.y +
                     frame.cameraEcef.z * frame.cameraEcef.z);
    if (cl > 0.0f) {
      skyU.lightDir[0] = frame.cameraEcef.x / cl;
      skyU.lightDir[1] = frame.cameraEcef.y / cl;
      skyU.lightDir[2] = frame.cameraEcef.z / cl;
    }

    memcpy(skyUboMapped_, &skyU, sizeof(SkyUBO));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipelineLayout_,
                            0, 1, &skyDescSet_, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
  }
}

void VulkanBackend::endFrame() {
  if (!frameBegan_) return;
  frameBegan_ = false;

  VkCommandBuffer cmd = commandBuffers_[frameIndex_];
  vkCmdEndRenderPass(cmd);
  VK_CHECK(vkEndCommandBuffer(cmd));

  VkSemaphore waitSemaphores[]   = {imageAvailableSemaphores_[frameIndex_]};
  VkSemaphore signalSemaphores[] = {renderFinishedSemaphores_[imageIndex_]};
  VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};

  VkSubmitInfo submitInfo{};
  submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.waitSemaphoreCount   = 1;
  submitInfo.pWaitSemaphores      = waitSemaphores;
  submitInfo.pWaitDstStageMask    = waitStages;
  submitInfo.commandBufferCount   = 1;
  submitInfo.pCommandBuffers      = &cmd;
  submitInfo.signalSemaphoreCount = 1;
  submitInfo.pSignalSemaphores    = signalSemaphores;
  VK_CHECK(vkQueueSubmit(graphicsQueue_, 1, &submitInfo, inFlightFences_[frameIndex_]));

  VkPresentInfoKHR presentInfo{};
  presentInfo.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  presentInfo.waitSemaphoreCount = 1;
  presentInfo.pWaitSemaphores    = signalSemaphores;
  presentInfo.swapchainCount     = 1;
  presentInfo.pSwapchains        = &swapchain_;
  presentInfo.pImageIndices      = &imageIndex_;

  VkResult result = vkQueuePresentKHR(presentQueue_, &presentInfo);
  if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
    needsSwapchainRecreate_ = true;
  }

  frameIndex_ = (frameIndex_ + 1) % kMaxFramesInFlight;
}

} // namespace reactnativecesium
