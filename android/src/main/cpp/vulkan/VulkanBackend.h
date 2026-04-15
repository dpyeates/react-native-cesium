#pragma once

#include "renderer/IGPUBackend.hpp"

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace reactnativecesium {

struct VulkanTexture {
  VkImage         image         = VK_NULL_HANDLE;
  VkDeviceMemory  memory        = VK_NULL_HANDLE;
  VkImageView     imageView     = VK_NULL_HANDLE;
  VkSampler       sampler       = VK_NULL_HANDLE;
  VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
};

class VulkanBackend : public IGPUBackend {
public:
  VulkanBackend();
  ~VulkanBackend() override;

  void initialize(void* nativeSurface, int width, int height) override;
  void resize(int width, int height) override;
  void shutdown() override;

  void beginFrame(const FrameParams& params) override;
  void drawScene(const FrameResult& frame) override;
  void endFrame() override;

  void* createRasterTexture(const uint8_t* pixels, int32_t width, int32_t height);
  void  freeRasterTexture(void* tex);

  // Water mask textures — same pixel format but allocated from a separate pool
  // so they can be bound at descriptor set 2 (vs imagery at set 1).
  void* createWaterMaskTexture(const uint8_t* pixels, int32_t width, int32_t height);
  void  freeWaterMaskTexture(void* tex);

  void setMsaaSampleCount(int sampleCount);

private:
  void createInstance();
  void pickPhysicalDevice();
  void createDevice();
  void createSwapchain();
  void createRenderPass();
  void createDepthResources();
  void createFramebuffers();
  void createCommandPool();
  void createCommandBuffers();
  void createSyncObjects();
  void createDescriptorSetLayout();
  void createPipelineLayout();
  void createSkyPipeline();
  void createTerrainPipeline();
  void createFallbackTexture();
  void createDescriptorPool();
  void createWaterMaskPool();
  void createWaterMaskFallback();

  void cleanupSwapchain();
  void recreateSwapchain();

  VkFormat pickDepthFormat() const;

  uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags props);
  void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags props, VkBuffer& buffer,
                    VkDeviceMemory& memory);
  void ensureBuffer(VkBuffer& buffer, VkDeviceMemory& memory, size_t& capacity,
                    size_t needed, const void* data, VkBufferUsageFlags usage);
  VkCommandBuffer beginSingleTimeCommands();
  void endSingleTimeCommands(VkCommandBuffer commandBuffer);

  // Upload pixel data from a staging buffer into image, transitioning layout
  // UNDEFINED → TRANSFER_DST → SHADER_READ_ONLY in a single command buffer.
  void uploadToImage(VkBuffer staging, VkImage image, uint32_t width, uint32_t height);

  void* window_ = nullptr; // ANativeWindow*

  VkInstance               instance_       = VK_NULL_HANDLE;
  VkPhysicalDevice         physicalDevice_ = VK_NULL_HANDLE;
  VkDevice                 device_         = VK_NULL_HANDLE;
  VkQueue                  graphicsQueue_  = VK_NULL_HANDLE;
  VkQueue                  presentQueue_   = VK_NULL_HANDLE;
  uint32_t                 graphicsFamily_ = 0;
  VkSurfaceKHR             surface_        = VK_NULL_HANDLE;
  VkSwapchainKHR           swapchain_      = VK_NULL_HANDLE;
  VkFormat                 swapchainFormat_    = VK_FORMAT_UNDEFINED;
  VkFormat                 depthFormat_        = VK_FORMAT_D32_SFLOAT;
  VkExtent2D               swapchainExtent_    = {0, 0};
  std::vector<VkImage>     swapchainImages_;
  std::vector<VkImageView> swapchainImageViews_;

  VkRenderPass             renderPass_     = VK_NULL_HANDLE;
  std::vector<VkFramebuffer> framebuffers_;

  VkImage        depthImage_      = VK_NULL_HANDLE;
  VkDeviceMemory depthMemory_     = VK_NULL_HANDLE;
  VkImageView    depthImageView_  = VK_NULL_HANDLE;

  VkCommandPool              commandPool_ = VK_NULL_HANDLE;
  std::vector<VkCommandBuffer> commandBuffers_;

  VkDescriptorSetLayout terrainDescSetLayout_ = VK_NULL_HANDLE; // set 0: UBO
  VkDescriptorSetLayout terrainTexLayout_     = VK_NULL_HANDLE; // set 1: texture
  VkDescriptorSetLayout skyDescSetLayout_     = VK_NULL_HANDLE;
  VkPipelineLayout      terrainPipelineLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout      skyPipelineLayout_     = VK_NULL_HANDLE;
  VkPipeline            terrainPipeline_       = VK_NULL_HANDLE;
  VkPipeline            skyPipeline_           = VK_NULL_HANDLE;

  VkDescriptorPool descriptorPool_     = VK_NULL_HANDLE;
  VkDescriptorPool waterMaskPool_      = VK_NULL_HANDLE;

  VulkanTexture   fallbackTexture_;
  VkDescriptorSet fallbackTexDescSet_     = VK_NULL_HANDLE;
  VulkanTexture   fallbackWaterMaskTex_;
  VkDescriptorSet fallbackWaterMaskDescSet_ = VK_NULL_HANDLE;

  // Sky UBO — persistently mapped
  VkBuffer        skyUboBuffer_  = VK_NULL_HANDLE;
  VkDeviceMemory  skyUboMemory_  = VK_NULL_HANDLE;
  void*           skyUboMapped_  = nullptr;
  VkDescriptorSet skyDescSet_    = VK_NULL_HANDLE;

  static constexpr int kMaxFramesInFlight = 3;
  // Upper bound on simultaneously live raster overlay textures.
  // Cesium can cache many hundreds of tiles at high zoom; 2048 gives headroom.
  static constexpr int kMaxRasterTextures = 2048;

  std::vector<VkSemaphore> imageAvailableSemaphores_;
  std::vector<VkSemaphore> renderFinishedSemaphores_;
  std::vector<VkFence>     inFlightFences_;
  int      frameIndex_  = 0;
  uint32_t imageIndex_  = 0;
  bool     frameBegan_  = false;

  // Triple-buffered persistent vertex/index/UV/altitude buffers
  VkBuffer       vtxBufs_[kMaxFramesInFlight] = {};
  VkDeviceMemory vtxMems_[kMaxFramesInFlight] = {};
  size_t         vtxCaps_[kMaxFramesInFlight] = {};

  VkBuffer       idxBufs_[kMaxFramesInFlight] = {};
  VkDeviceMemory idxMems_[kMaxFramesInFlight] = {};
  size_t         idxCaps_[kMaxFramesInFlight] = {};

  VkBuffer       uvBufs_[kMaxFramesInFlight]  = {};
  VkDeviceMemory uvMems_[kMaxFramesInFlight]  = {};
  size_t         uvCaps_[kMaxFramesInFlight]  = {};

  VkBuffer       altBufs_[kMaxFramesInFlight] = {};  // float altitude per vertex
  VkDeviceMemory altMems_[kMaxFramesInFlight] = {};
  size_t         altCaps_[kMaxFramesInFlight] = {};

  // Per-frame UBO: just cameraEcef (fragment stage, used for lighting vd).
  // MVP is now per-draw via push constants.
  VkBuffer        terrainUboBufs_[kMaxFramesInFlight] = {};
  VkDeviceMemory  terrainUboMems_[kMaxFramesInFlight] = {};
  void*           terrainUboMapped_[kMaxFramesInFlight] = {};
  VkDescriptorSet terrainDescSets_[kMaxFramesInFlight] = {};

  int  viewportWidth_  = 0;
  int  viewportHeight_ = 0;
  bool needsSwapchainRecreate_ = false;

  // Deferred texture deletion — freeRasterTexture enqueues here so no GPU stall
  // on eviction; beginFrame flushes entries old enough to be safe.
  // pool identifies which VkDescriptorPool owns the descriptor set; if
  // VK_NULL_HANDLE the fallback / UBO pool (descriptorPool_) is used.
  struct PendingDelete {
    VulkanTexture*   tex;
    uint64_t         frameIndex;
    VkDescriptorPool pool = VK_NULL_HANDLE;
  };
  std::deque<PendingDelete> pendingDeletes_;
  std::mutex                pendingDeletesMutex_;
  uint64_t                  totalFrameCount_ = 0;

  void flushPendingDeletes();
  void destroyTexture(VulkanTexture* tex, VkDescriptorPool pool);
};

} // namespace reactnativecesium
