// VulkanBackend.cpp — Android Vulkan rendering backend
//
// Architecture mirrors MetalBackend.mm:
//   - Eye-relative float positions computed on CPU (double precision).
//   - Single merged vertex + index buffer uploaded each frame.
//   - Reversed-Z infinite projection: depth clear = 0, compare = GREATER.
//   - Sky drawn first (no depth write), terrain drawn after.
//   - Imagery overlays: UV buffer alongside positions; push constants select
//     overlay texture per draw.
//   - Triple-buffered persistent VkBuffers guarded by VkFence.

#include "VulkanBackend.h"

#include <android/native_window.h>
#include <android/log.h>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>

// Pre-compiled SPIR-V headers (generated at build time by glslc + spv_to_header.cmake)
#include "terrain.vert.spv.h"
#include "terrain.frag.spv.h"
#include "sky.vert.spv.h"
#include "sky.frag.spv.h"

#define LOG_TAG "VulkanBackend"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define VK_CHECK(call) do {                                   \
  VkResult _r = (call);                                       \
  if (_r != VK_SUCCESS) {                                     \
    LOGE("%s failed: %d at %s:%d", #call, _r, __FILE__, __LINE__); \
  }                                                           \
} while(0)

struct TerrainUBO {
  float vpMatrix[16];
  float cameraEcef[4];
};

struct SkyUBO {
  float invVP[16];
  float cameraEcef[4];
  float lightDir[4];
};

struct OverlayPushConstants {
  uint32_t hasOverlay;
  uint32_t isEllipsoidFallback;
  float    translationX;
  float    translationY;
  float    scaleX;
  float    scaleY;
};

namespace reactnativecesium {

VulkanBackend::VulkanBackend() = default;

VulkanBackend::~VulkanBackend() { shutdown(); }

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
  createSwapchain();
  createRenderPass();
  createDepthResources();
  createFramebuffers();
  createCommandBuffers();
  createSyncObjects();
  createDescriptorSetLayout();
  createDescriptorPool();
  createPipelineLayout();
  createSkyPipeline();
  createTerrainPipeline();
  createFallbackTexture();

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

  // Allocate terrain UBOs + descriptor sets (set 0: UBO only, one per frame)
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

    // Persistent mapping — HOST_COHERENT so no explicit flush needed.
    VK_CHECK(vkMapMemory(device_, terrainUboMems_[i], 0, sizeof(TerrainUBO),
                         0, &terrainUboMapped_[i]));
  }

  // Allocate fallback texture descriptor set (set 1)
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

  LOGI("VulkanBackend initialized: %dx%d, %zu swapchain images",
       width, height, swapchainImages_.size());
}

void VulkanBackend::resize(int width, int height) {
  viewportWidth_  = width;
  viewportHeight_ = height;
  needsSwapchainRecreate_ = true;
}

void VulkanBackend::shutdown() {
  if (device_ == VK_NULL_HANDLE) return;
  vkDeviceWaitIdle(device_);

  // Drain the deferred deletion queue — GPU is idle so all are safe to free.
  {
    std::lock_guard<std::mutex> lk(pendingDeletesMutex_);
    while (!pendingDeletes_.empty()) {
      destroyTexture(pendingDeletes_.front().tex);
      pendingDeletes_.pop_front();
    }
  }

  // Free fallback texture
  if (fallbackTexture_.sampler)     vkDestroySampler(device_, fallbackTexture_.sampler, nullptr);
  if (fallbackTexture_.imageView)   vkDestroyImageView(device_, fallbackTexture_.imageView, nullptr);
  if (fallbackTexture_.image)       vkDestroyImage(device_, fallbackTexture_.image, nullptr);
  if (fallbackTexture_.memory)      vkFreeMemory(device_, fallbackTexture_.memory, nullptr);
  fallbackTexture_ = {};

  // Free sky UBO (unmap before destroy)
  if (skyUboMapped_)  { vkUnmapMemory(device_, skyUboMemory_); skyUboMapped_ = nullptr; }
  if (skyUboBuffer_)  vkDestroyBuffer(device_, skyUboBuffer_, nullptr);
  if (skyUboMemory_)  vkFreeMemory(device_, skyUboMemory_, nullptr);
  skyUboBuffer_ = VK_NULL_HANDLE;
  skyUboMemory_ = VK_NULL_HANDLE;

  // Free terrain UBOs (unmap before destroy)
  for (int i = 0; i < kMaxFramesInFlight; ++i) {
    if (terrainUboMapped_[i]) { vkUnmapMemory(device_, terrainUboMems_[i]); terrainUboMapped_[i] = nullptr; }
    if (terrainUboBufs_[i])   vkDestroyBuffer(device_, terrainUboBufs_[i], nullptr);
    if (terrainUboMems_[i])   vkFreeMemory(device_, terrainUboMems_[i], nullptr);
    terrainUboBufs_[i] = VK_NULL_HANDLE;
    terrainUboMems_[i] = VK_NULL_HANDLE;
  }

  // Free persistent frame buffers
  for (int i = 0; i < kMaxFramesInFlight; ++i) {
    if (vtxBufs_[i]) vkDestroyBuffer(device_, vtxBufs_[i], nullptr);
    if (vtxMems_[i]) vkFreeMemory(device_, vtxMems_[i], nullptr);
    if (idxBufs_[i]) vkDestroyBuffer(device_, idxBufs_[i], nullptr);
    if (idxMems_[i]) vkFreeMemory(device_, idxMems_[i], nullptr);
    if (uvBufs_[i])  vkDestroyBuffer(device_, uvBufs_[i], nullptr);
    if (uvMems_[i])  vkFreeMemory(device_, uvMems_[i], nullptr);
    vtxBufs_[i] = idxBufs_[i] = uvBufs_[i] = VK_NULL_HANDLE;
    vtxMems_[i] = idxMems_[i] = uvMems_[i] = VK_NULL_HANDLE;
    vtxCaps_[i] = idxCaps_[i] = uvCaps_[i] = 0;
  }

  for (auto& s : imageAvailableSemaphores_) vkDestroySemaphore(device_, s, nullptr);
  for (auto& s : renderFinishedSemaphores_) vkDestroySemaphore(device_, s, nullptr);
  for (auto& f : inFlightFences_)           vkDestroyFence(device_, f, nullptr);
  imageAvailableSemaphores_.clear();
  renderFinishedSemaphores_.clear();
  inFlightFences_.clear();

  cleanupSwapchain();

  if (terrainPipeline_)       vkDestroyPipeline(device_, terrainPipeline_, nullptr);
  if (skyPipeline_)           vkDestroyPipeline(device_, skyPipeline_, nullptr);
  if (terrainPipelineLayout_) vkDestroyPipelineLayout(device_, terrainPipelineLayout_, nullptr);
  if (skyPipelineLayout_)     vkDestroyPipelineLayout(device_, skyPipelineLayout_, nullptr);
  if (terrainDescSetLayout_)  vkDestroyDescriptorSetLayout(device_, terrainDescSetLayout_, nullptr);
  if (terrainTexLayout_)      vkDestroyDescriptorSetLayout(device_, terrainTexLayout_, nullptr);
  if (skyDescSetLayout_)      vkDestroyDescriptorSetLayout(device_, skyDescSetLayout_, nullptr);
  if (descriptorPool_)        vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
  if (commandPool_)           vkDestroyCommandPool(device_, commandPool_, nullptr);
  if (renderPass_)            vkDestroyRenderPass(device_, renderPass_, nullptr);

  terrainPipeline_ = skyPipeline_ = VK_NULL_HANDLE;
  terrainPipelineLayout_ = skyPipelineLayout_ = VK_NULL_HANDLE;
  terrainDescSetLayout_ = terrainTexLayout_ = skyDescSetLayout_ = VK_NULL_HANDLE;
  descriptorPool_ = VK_NULL_HANDLE;
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

  for (auto& dev : devices) {
    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> qFamilies(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, qFamilies.data());

    for (uint32_t i = 0; i < qCount; ++i) {
      VkBool32 presentSupport = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface_, &presentSupport);

      if ((qFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && presentSupport) {
        physicalDevice_ = dev;
        graphicsFamily_ = i;
        return;
      }
    }
  }
  LOGE("No suitable Vulkan physical device found");
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

// ── Depth format selection ─────────────────────────────────────────────────────

VkFormat VulkanBackend::pickDepthFormat() const {
  // Prefer D32 for maximum precision; fall back to packed D24/S8 or D16.
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

// ── Swapchain ──────────────────────────────────────────────────────────────────

void VulkanBackend::createSwapchain() {
  VkSurfaceCapabilitiesKHR caps;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &caps);

  uint32_t fmtCount;
  vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &fmtCount, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(fmtCount);
  vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &fmtCount, formats.data());

  // Prefer sRGB
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

  // Determine extent
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

  // Pick depth format once (stable across recreations on the same device).
  depthFormat_ = pickDepthFormat();

  // Prefer OPAQUE composite alpha (avoids blending with the window behind the
  // surface); fall back through the other modes in priority order.
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
  swapInfo.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
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
  VkAttachmentDescription colorAttach{};
  colorAttach.format         = swapchainFormat_;
  colorAttach.samples        = VK_SAMPLE_COUNT_1_BIT;
  colorAttach.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
  colorAttach.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
  colorAttach.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  colorAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  colorAttach.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
  colorAttach.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

  VkAttachmentDescription depthAttach{};
  depthAttach.format         = depthFormat_;
  depthAttach.samples        = VK_SAMPLE_COUNT_1_BIT;
  depthAttach.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depthAttach.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depthAttach.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  depthAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depthAttach.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
  depthAttach.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentReference colorRef{};
  colorRef.attachment = 0;
  colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkAttachmentReference depthRef{};
  depthRef.attachment = 1;
  depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount    = 1;
  subpass.pColorAttachments       = &colorRef;
  subpass.pDepthStencilAttachment = &depthRef;

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

  std::array<VkAttachmentDescription, 2> attachments = {colorAttach, depthAttach};

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
  imgInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
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

void VulkanBackend::createFramebuffers() {
  framebuffers_.resize(swapchainImageViews_.size());
  for (size_t i = 0; i < swapchainImageViews_.size(); ++i) {
    std::array<VkImageView, 2> attachments = {swapchainImageViews_[i], depthImageView_};

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
  // RESET: per-buffer reset for frame command buffers.
  // TRANSIENT: hint allocator to prefer fast short-lived allocations (benefits
  //            the one-time staging uploads that share this pool).
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
}

void VulkanBackend::createSyncObjects() {
  imageAvailableSemaphores_.resize(kMaxFramesInFlight);
  renderFinishedSemaphores_.resize(kMaxFramesInFlight);
  inFlightFences_.resize(kMaxFramesInFlight);

  VkSemaphoreCreateInfo semInfo{};
  semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

  for (int i = 0; i < kMaxFramesInFlight; ++i) {
    VK_CHECK(vkCreateSemaphore(device_, &semInfo, nullptr, &imageAvailableSemaphores_[i]));
    VK_CHECK(vkCreateSemaphore(device_, &semInfo, nullptr, &renderFinishedSemaphores_[i]));
    VK_CHECK(vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFences_[i]));
  }
}

// ── Descriptor Layouts + Pipeline Layouts ──────────────────────────────────────

void VulkanBackend::createDescriptorSetLayout() {
  // Terrain set 0: UBO at binding 0 (vertex + fragment)
  VkDescriptorSetLayoutBinding uboBinding{};
  uboBinding.binding         = 0;
  uboBinding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  uboBinding.descriptorCount = 1;
  uboBinding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutCreateInfo uboLayoutInfo{};
  uboLayoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  uboLayoutInfo.bindingCount = 1;
  uboLayoutInfo.pBindings    = &uboBinding;
  VK_CHECK(vkCreateDescriptorSetLayout(device_, &uboLayoutInfo, nullptr, &terrainDescSetLayout_));

  // Terrain set 1: combined image sampler at binding 0 (fragment only)
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

  // Sky: binding 0 = UBO only
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
  // Terrain: set 0 = UBO, set 1 = texture, plus push constants for overlay params
  VkPushConstantRange pushRange{};
  pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  pushRange.offset     = 0;
  pushRange.size       = sizeof(OverlayPushConstants);

  VkDescriptorSetLayout terrainSetLayouts[] = {terrainDescSetLayout_, terrainTexLayout_};

  VkPipelineLayoutCreateInfo terrainLayoutInfo{};
  terrainLayoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  terrainLayoutInfo.setLayoutCount         = 2;
  terrainLayoutInfo.pSetLayouts            = terrainSetLayouts;
  terrainLayoutInfo.pushConstantRangeCount = 1;
  terrainLayoutInfo.pPushConstantRanges    = &pushRange;
  VK_CHECK(vkCreatePipelineLayout(device_, &terrainLayoutInfo, nullptr, &terrainPipelineLayout_));

  // Sky: no push constants
  VkPipelineLayoutCreateInfo skyLayoutInfo{};
  skyLayoutInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  skyLayoutInfo.setLayoutCount = 1;
  skyLayoutInfo.pSetLayouts    = &skyDescSetLayout_;
  VK_CHECK(vkCreatePipelineLayout(device_, &skyLayoutInfo, nullptr, &skyPipelineLayout_));
}

void VulkanBackend::createDescriptorPool() {
  // UBO sets: kMaxFramesInFlight terrain + 1 sky
  // Sampler sets: 1 fallback + up to kMaxRasterTextures raster overlay textures
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

// ── Pipeline creation ──────────────────────────────────────────────────────────

void VulkanBackend::createSkyPipeline() {
  VkShaderModule vertModule = createShaderModule(device_, spv_sky_vert, spv_sky_vert_size);
  VkShaderModule fragModule = createShaderModule(device_, spv_sky_frag, spv_sky_frag_size);

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vertModule;
  stages[0].pName  = "main";
  stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fragModule;
  stages[1].pName  = "main";

  // No vertex input (fullscreen triangle from gl_VertexIndex)
  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

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
  rasterizer.cullMode    = VK_CULL_MODE_NONE;
  rasterizer.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

  VkPipelineMultisampleStateCreateInfo multisampling{};
  multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable  = VK_FALSE;
  depthStencil.depthWriteEnable = VK_FALSE;

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
  pipelineInfo.layout              = skyPipelineLayout_;
  pipelineInfo.renderPass          = renderPass_;
  pipelineInfo.subpass             = 0;
  VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &skyPipeline_));

  vkDestroyShaderModule(device_, vertModule, nullptr);
  vkDestroyShaderModule(device_, fragModule, nullptr);
}

void VulkanBackend::createTerrainPipeline() {
  VkShaderModule vertModule = createShaderModule(device_, spv_terrain_vert, spv_terrain_vert_size);
  VkShaderModule fragModule = createShaderModule(device_, spv_terrain_frag, spv_terrain_frag_size);

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vertModule;
  stages[0].pName  = "main";
  stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fragModule;
  stages[1].pName  = "main";

  // Vertex bindings: position (float3) at binding 0, UV (float2) at binding 1
  VkVertexInputBindingDescription bindings[2]{};
  bindings[0].binding   = 0;
  bindings[0].stride    = 3 * sizeof(float);
  bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  bindings[1].binding   = 1;
  bindings[1].stride    = 2 * sizeof(float);
  bindings[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

  VkVertexInputAttributeDescription attrs[2]{};
  attrs[0].binding  = 0;
  attrs[0].location = 0;
  attrs[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
  attrs[0].offset   = 0;
  attrs[1].binding  = 1;
  attrs[1].location = 1;
  attrs[1].format   = VK_FORMAT_R32G32_SFLOAT;
  attrs[1].offset   = 0;

  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInput.vertexBindingDescriptionCount   = 2;
  vertexInput.pVertexBindingDescriptions      = bindings;
  vertexInput.vertexAttributeDescriptionCount = 2;
  vertexInput.pVertexAttributeDescriptions    = attrs;

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
  rasterizer.cullMode    = VK_CULL_MODE_BACK_BIT;
  rasterizer.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

  VkPipelineMultisampleStateCreateInfo multisampling{};
  multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  // Reversed-Z: depth clear = 0.0 (far), compare = GREATER (closer wins)
  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable  = VK_TRUE;
  depthStencil.depthWriteEnable = VK_TRUE;
  depthStencil.depthCompareOp   = VK_COMPARE_OP_GREATER;

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
  pipelineInfo.layout              = terrainPipelineLayout_;
  pipelineInfo.renderPass          = renderPass_;
  pipelineInfo.subpass             = 0;
  VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &terrainPipeline_));

  vkDestroyShaderModule(device_, vertModule, nullptr);
  vkDestroyShaderModule(device_, fragModule, nullptr);
}

// ── Helpers ────────────────────────────────────────────────────────────────────

uint32_t VulkanBackend::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags props) {
  VkPhysicalDeviceMemoryProperties memProps;
  vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps);
  for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
    if ((typeFilter & (1 << i)) &&
        (memProps.memoryTypes[i].propertyFlags & props) == props) {
      return i;
    }
  }
  LOGE("Failed to find suitable memory type");
  return 0;
}

void VulkanBackend::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                  VkMemoryPropertyFlags props,
                                  VkBuffer& buffer, VkDeviceMemory& memory) {
  VkBufferCreateInfo bufInfo{};
  bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufInfo.size  = size;
  bufInfo.usage = usage;
  bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VK_CHECK(vkCreateBuffer(device_, &bufInfo, nullptr, &buffer));

  VkMemoryRequirements memReqs;
  vkGetBufferMemoryRequirements(device_, buffer, &memReqs);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize  = memReqs.size;
  allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, props);
  VK_CHECK(vkAllocateMemory(device_, &allocInfo, nullptr, &memory));
  VK_CHECK(vkBindBufferMemory(device_, buffer, memory, 0));
}

void VulkanBackend::ensureBuffer(VkBuffer& buffer, VkDeviceMemory& memory,
                                  size_t& capacity, size_t needed,
                                  const void* data, VkBufferUsageFlags usage) {
  if (needed == 0) return;
  if (needed > capacity) {
    if (buffer) vkDestroyBuffer(device_, buffer, nullptr);
    if (memory) vkFreeMemory(device_, memory, nullptr);
    size_t newCap = std::max(needed, capacity > 0 ? capacity * 2 : needed);
    newCap = (newCap + 4095UL) & ~4095UL;
    createBuffer(static_cast<VkDeviceSize>(newCap), usage,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 buffer, memory);
    capacity = newCap;
  }
  void* mapped;
  vkMapMemory(device_, memory, 0, needed, 0, &mapped);
  memcpy(mapped, data, needed);
  vkUnmapMemory(device_, memory);
}

VkCommandBuffer VulkanBackend::beginSingleTimeCommands() {
  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandPool        = commandPool_;
  allocInfo.commandBufferCount = 1;

  VkCommandBuffer cmd;
  vkAllocateCommandBuffers(device_, &allocInfo, &cmd);

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &beginInfo);
  return cmd;
}

void VulkanBackend::endSingleTimeCommands(VkCommandBuffer cmd) {
  vkEndCommandBuffer(cmd);

  VkSubmitInfo submitInfo{};
  submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers    = &cmd;

  vkQueueSubmit(graphicsQueue_, 1, &submitInfo, VK_NULL_HANDLE);
  vkQueueWaitIdle(graphicsQueue_);
  vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
}

void VulkanBackend::uploadToImage(VkBuffer staging, VkImage image,
                                   uint32_t width, uint32_t height) {
  // All three steps (UNDEFINED→TRANSFER_DST, copy, TRANSFER_DST→SHADER_READ)
  // are recorded in a single command buffer, so only one vkQueueWaitIdle is needed.
  VkCommandBuffer cmd = beginSingleTimeCommands();

  const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

  VkImageMemoryBarrier toTransfer{};
  toTransfer.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  toTransfer.srcAccessMask       = 0;
  toTransfer.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
  toTransfer.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
  toTransfer.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.image               = image;
  toTransfer.subresourceRange    = range;
  vkCmdPipelineBarrier(cmd,
    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
    0, 0, nullptr, 0, nullptr, 1, &toTransfer);

  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent      = {width, height, 1};
  vkCmdCopyBufferToImage(cmd, staging, image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  VkImageMemoryBarrier toShader{};
  toShader.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  toShader.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
  toShader.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
  toShader.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toShader.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toShader.image               = image;
  toShader.subresourceRange    = range;
  vkCmdPipelineBarrier(cmd,
    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
    0, 0, nullptr, 0, nullptr, 1, &toShader);

  endSingleTimeCommands(cmd);
}

// ── Fallback texture (1x1 white) ───────────────────────────────────────────────

void VulkanBackend::createFallbackTexture() {
  const uint8_t white[4] = {255, 255, 255, 255};

  VkImageCreateInfo imgInfo{};
  imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imgInfo.imageType     = VK_IMAGE_TYPE_2D;
  imgInfo.format        = VK_FORMAT_R8G8B8A8_UNORM;
  imgInfo.extent        = {1, 1, 1};
  imgInfo.mipLevels     = 1;
  imgInfo.arrayLayers   = 1;
  imgInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
  imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
  imgInfo.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VK_CHECK(vkCreateImage(device_, &imgInfo, nullptr, &fallbackTexture_.image));

  VkMemoryRequirements memReqs;
  vkGetImageMemoryRequirements(device_, fallbackTexture_.image, &memReqs);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize  = memReqs.size;
  allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VK_CHECK(vkAllocateMemory(device_, &allocInfo, nullptr, &fallbackTexture_.memory));
  VK_CHECK(vkBindImageMemory(device_, fallbackTexture_.image, fallbackTexture_.memory, 0));

  VkBuffer staging;
  VkDeviceMemory stagingMem;
  createBuffer(4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
               staging, stagingMem);

  void* mapped;
  vkMapMemory(device_, stagingMem, 0, 4, 0, &mapped);
  memcpy(mapped, white, 4);
  vkUnmapMemory(device_, stagingMem);

  uploadToImage(staging, fallbackTexture_.image, 1, 1);

  vkDestroyBuffer(device_, staging, nullptr);
  vkFreeMemory(device_, stagingMem, nullptr);

  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image    = fallbackTexture_.image;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format   = VK_FORMAT_R8G8B8A8_UNORM;
  viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VK_CHECK(vkCreateImageView(device_, &viewInfo, nullptr, &fallbackTexture_.imageView));

  VkSamplerCreateInfo samplerInfo{};
  samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  samplerInfo.magFilter    = VK_FILTER_LINEAR;
  samplerInfo.minFilter    = VK_FILTER_LINEAR;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VK_CHECK(vkCreateSampler(device_, &samplerInfo, nullptr, &fallbackTexture_.sampler));
}

// ── Raster texture creation/deletion ───────────────────────────────────────────

void* VulkanBackend::createRasterTexture(const uint8_t* pixels, int32_t width, int32_t height) {
  if (!device_ || !pixels || width <= 0 || height <= 0) return nullptr;

  auto* tex = new VulkanTexture();
  VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * height * 4;

  VkBuffer staging;
  VkDeviceMemory stagingMem;
  createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
               staging, stagingMem);

  void* mapped;
  vkMapMemory(device_, stagingMem, 0, imageSize, 0, &mapped);
  memcpy(mapped, pixels, static_cast<size_t>(imageSize));
  vkUnmapMemory(device_, stagingMem);

  VkImageCreateInfo imgInfo{};
  imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imgInfo.imageType     = VK_IMAGE_TYPE_2D;
  imgInfo.format        = VK_FORMAT_R8G8B8A8_UNORM;
  imgInfo.extent        = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
  imgInfo.mipLevels     = 1;
  imgInfo.arrayLayers   = 1;
  imgInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
  imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
  imgInfo.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VK_CHECK(vkCreateImage(device_, &imgInfo, nullptr, &tex->image));

  VkMemoryRequirements memReqs;
  vkGetImageMemoryRequirements(device_, tex->image, &memReqs);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize  = memReqs.size;
  allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VK_CHECK(vkAllocateMemory(device_, &allocInfo, nullptr, &tex->memory));
  VK_CHECK(vkBindImageMemory(device_, tex->image, tex->memory, 0));

  uploadToImage(staging, tex->image,
                static_cast<uint32_t>(width), static_cast<uint32_t>(height));

  vkDestroyBuffer(device_, staging, nullptr);
  vkFreeMemory(device_, stagingMem, nullptr);

  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image    = tex->image;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format   = VK_FORMAT_R8G8B8A8_UNORM;
  viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VK_CHECK(vkCreateImageView(device_, &viewInfo, nullptr, &tex->imageView));

  VkSamplerCreateInfo samplerInfo{};
  samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  samplerInfo.magFilter    = VK_FILTER_LINEAR;
  samplerInfo.minFilter    = VK_FILTER_LINEAR;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VK_CHECK(vkCreateSampler(device_, &samplerInfo, nullptr, &tex->sampler));

  // Allocate a descriptor set for this texture (set 1: texture only)
  VkDescriptorSetAllocateInfo dsAllocInfo{};
  dsAllocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  dsAllocInfo.descriptorPool     = descriptorPool_;
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

void VulkanBackend::freeRasterTexture(void* texPtr) {
  if (!texPtr || !device_) return;
  auto* tex = static_cast<VulkanTexture*>(texPtr);

  // Enqueue for deferred deletion rather than stalling the GPU now.
  // The texture will be destroyed once kMaxFramesInFlight frames have elapsed,
  // guaranteeing no in-flight command buffer is still referencing it.
  std::lock_guard<std::mutex> lk(pendingDeletesMutex_);
  pendingDeletes_.push_back({tex, totalFrameCount_});
}

void VulkanBackend::destroyTexture(VulkanTexture* tex) {
  if (!tex) return;
  if (tex->descriptorSet) {
    vkFreeDescriptorSets(device_, descriptorPool_, 1, &tex->descriptorSet);
  }
  if (tex->sampler)   vkDestroySampler(device_, tex->sampler, nullptr);
  if (tex->imageView) vkDestroyImageView(device_, tex->imageView, nullptr);
  if (tex->image)     vkDestroyImage(device_, tex->image, nullptr);
  if (tex->memory)    vkFreeMemory(device_, tex->memory, nullptr);
  delete tex;
}

void VulkanBackend::flushPendingDeletes() {
  std::lock_guard<std::mutex> lk(pendingDeletesMutex_);
  // Entries are enqueued in monotonically increasing frameIndex order, so we
  // can stop as soon as the front entry is not yet safe to destroy.
  while (!pendingDeletes_.empty()) {
    const auto& front = pendingDeletes_.front();
    if (totalFrameCount_ < front.frameIndex + static_cast<uint64_t>(kMaxFramesInFlight))
      break;
    destroyTexture(front.tex);
    pendingDeletes_.pop_front();
  }
}

void VulkanBackend::setMsaaSampleCount(int /*sampleCount*/) {
  // MSAA not implemented in initial version — requires resolve attachments.
  // Accepted but ignored; can be wired up in a follow-up pass.
}

// ── Swapchain recreation ───────────────────────────────────────────────────────

void VulkanBackend::cleanupSwapchain() {
  for (auto& fb : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
  framebuffers_.clear();

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
  vkDeviceWaitIdle(device_);
  cleanupSwapchain();
  createSwapchain();
  createDepthResources();
  createFramebuffers();
  needsSwapchainRecreate_ = false;
}

// ── Per-frame rendering ────────────────────────────────────────────────────────

void VulkanBackend::beginFrame(const FrameParams& /*params*/) {
  if (device_ == VK_NULL_HANDLE) return;

  if (needsSwapchainRecreate_) {
    recreateSwapchain();
  }

  vkWaitForFences(device_, 1, &inFlightFences_[frameIndex_], VK_TRUE, UINT64_MAX);

  // GPU is done with frameIndex_ — safe to destroy textures enqueued that long ago.
  ++totalFrameCount_;
  flushPendingDeletes();

  VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                          imageAvailableSemaphores_[frameIndex_],
                                          VK_NULL_HANDLE, &imageIndex_);
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

  std::array<VkClearValue, 2> clearValues{};
  clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  clearValues[1].depthStencil = {0.0f, 0}; // Reversed-Z: 0 = far

  VkRenderPassBeginInfo rpBegin{};
  rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rpBegin.renderPass        = renderPass_;
  rpBegin.framebuffer       = framebuffers_[imageIndex_];
  rpBegin.renderArea.offset = {0, 0};
  rpBegin.renderArea.extent = swapchainExtent_;
  rpBegin.clearValueCount   = static_cast<uint32_t>(clearValues.size());
  rpBegin.pClearValues      = clearValues.data();

  vkCmdBeginRenderPass(commandBuffers_[frameIndex_], &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

  // Negative viewport height flips Y to match the OpenGL/Metal clip-space
  // convention that the shared C++ projection matrix targets (Vulkan 1.1+).
  // The spec auto-adjusts triangle winding so VK_FRONT_FACE_COUNTER_CLOCKWISE
  // still works correctly.
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

  // ── Sky pass ──────────────────────────────────────────────────────────────
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

  // ── Terrain pass ──────────────────────────────────────────────────────────
  if (frame.draws.empty() || frame.eyeRelPositions.empty() || frame.indices.empty() ||
      !terrainPipeline_) {
    return;
  }

  const int fi = frameIndex_;

  const size_t vtxBytes = frame.eyeRelPositions.size() * sizeof(float);
  const size_t idxBytes = frame.indices.size() * sizeof(uint32_t);
  const size_t uvBytes  = frame.uvs.size() * sizeof(float);

  ensureBuffer(vtxBufs_[fi], vtxMems_[fi], vtxCaps_[fi], vtxBytes,
               frame.eyeRelPositions.data(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  ensureBuffer(idxBufs_[fi], idxMems_[fi], idxCaps_[fi], idxBytes,
               frame.indices.data(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
  ensureBuffer(uvBufs_[fi], uvMems_[fi], uvCaps_[fi], uvBytes,
               frame.uvs.data(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

  if (!vtxBufs_[fi] || !idxBufs_[fi]) return;

  // Update terrain UBO
  TerrainUBO terrU{};
  const float* vp = glm::value_ptr(frame.vpMatrix);
  for (int i = 0; i < 16; ++i) terrU.vpMatrix[i] = vp[i];
  terrU.cameraEcef[0] = frame.cameraEcef.x;
  terrU.cameraEcef[1] = frame.cameraEcef.y;
  terrU.cameraEcef[2] = frame.cameraEcef.z;

  memcpy(terrainUboMapped_[fi], &terrU, sizeof(TerrainUBO));

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, terrainPipeline_);

  VkBuffer vertexBuffers[] = {vtxBufs_[fi], uvBufs_[fi]};
  VkDeviceSize offsets[]   = {0, 0};
  vkCmdBindVertexBuffers(cmd, 0, 2, vertexBuffers, offsets);
  vkCmdBindIndexBuffer(cmd, idxBufs_[fi], 0, VK_INDEX_TYPE_UINT32);

  // Bind set 0 (UBO) once for the whole terrain pass.
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, terrainPipelineLayout_,
                          0, 1, &terrainDescSets_[fi], 0, nullptr);

  // Bind set 1 (texture) — start with fallback; rebind per draw as needed.
  VkDescriptorSet lastTexSet = fallbackTexDescSet_;
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, terrainPipelineLayout_,
                          1, 1, &lastTexSet, 0, nullptr);

  for (const auto& draw : frame.draws) {
    if (draw.indexCount == 0) continue;

    OverlayPushConstants ov{};
    ov.hasOverlay          = (draw.overlayTexture && draw.hasUVs) ? 1u : 0u;
    ov.isEllipsoidFallback = draw.isEllipsoidFallback ? 1u : 0u;
    ov.translationX = draw.overlayTranslation.x;
    ov.translationY = draw.overlayTranslation.y;
    ov.scaleX       = draw.overlayScale.x;
    ov.scaleY       = draw.overlayScale.y;

    vkCmdPushConstants(cmd, terrainPipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(OverlayPushConstants), &ov);

    // Determine which texture descriptor set to use for this draw.
    VkDescriptorSet texSet = fallbackTexDescSet_;
    if (draw.overlayTexture && draw.hasUVs) {
      auto* tex = static_cast<VulkanTexture*>(draw.overlayTexture);
      if (tex && tex->descriptorSet) {
        texSet = tex->descriptorSet;
      }
    }

    if (texSet != lastTexSet) {
      lastTexSet = texSet;
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, terrainPipelineLayout_,
                              1, 1, &lastTexSet, 0, nullptr);
    }

    uint32_t firstIndex = draw.indexByteOffset / sizeof(uint32_t);
    vkCmdDrawIndexed(cmd, draw.indexCount, 1, firstIndex, 0, 0);
  }
}

void VulkanBackend::endFrame() {
  if (!frameBegan_) return;
  frameBegan_ = false;

  VkCommandBuffer cmd = commandBuffers_[frameIndex_];
  vkCmdEndRenderPass(cmd);
  VK_CHECK(vkEndCommandBuffer(cmd));

  VkSemaphore waitSemaphores[]   = {imageAvailableSemaphores_[frameIndex_]};
  VkSemaphore signalSemaphores[] = {renderFinishedSemaphores_[frameIndex_]};
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
