#pragma once

#include "engine/EngineTunables.hpp"
#include "renderer/IGPUBackend.hpp"

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace reactnativecesium {

struct VulkanTexture {
  VkImage         image         = VK_NULL_HANDLE;
  VkDeviceMemory  memory        = VK_NULL_HANDLE;
  VkImageView     imageView     = VK_NULL_HANDLE;
  VkSampler       sampler       = VK_NULL_HANDLE;
  VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
  uint32_t        width         = 0;
  uint32_t        height        = 0;
  uint32_t        mipLevels     = 1;
  // True until the deferred upload command for this texture has been submitted.
  // Bound textures with `pendingUpload == true` are skipped (the fallback is
  // used instead) — prevents reading from a still-uninitialised image.
  bool            pendingUpload = false;
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

  // Cache directory used to persist the VkPipelineCache between sessions and
  // (in future) any other on-disk Vulkan artefacts. Must be called before
  // `initialize` to take effect on cold start.
  void setCacheDir(const std::string& cacheDir) { cacheDir_ = cacheDir; }

  void* createRasterTexture(const uint8_t* pixels, int32_t width, int32_t height);
  void  freeRasterTexture(void* tex);

  // Water mask textures — same pixel format but allocated from a separate pool
  // so they can be bound at descriptor set 2 (vs imagery at set 1).
  void* createWaterMaskTexture(const uint8_t* pixels, int32_t width, int32_t height);
  void  freeWaterMaskTexture(void* tex);

  void setMsaaSampleCount(int sampleCount);

private:
  // ── Init helpers ─────────────────────────────────────────────────────────
  void createInstance();
  void pickPhysicalDevice();
  void createDevice();
  void createSwapchain();
  void createRenderPass();
  void createDepthResources();
  void createMsaaResources();
  void createFramebuffers();
  void createCommandPool();
  void createCommandBuffers();
  void createSyncObjects();
  void createDescriptorSetLayout();
  void createPipelineLayout();
  void createSkyPipeline();
  void createTerrainPipeline();
  void createGraphicsPipelinesAll();
  void destroyGraphicsPipelinesAll();
  void createPipelineCache();
  void persistPipelineCache();
  void createFallbackTexture();
  void createDescriptorPool();
  void createWaterMaskPool();
  void createWaterMaskFallback();

  void cleanupSwapchain();
  void recreateSwapchain();

  // Pick the best supported MSAA count <= `requested` (1, 2, 4, or 8).
  // `requested` of 0 or 1 disables MSAA.
  VkSampleCountFlagBits resolveMsaaSamples(int requested) const;

  VkFormat pickDepthFormat() const;

  // Picks a present mode: prefer MAILBOX (if `wantLowLatency`), then FIFO_RELAXED,
  // then the default FIFO (always available per spec).
  VkPresentModeKHR pickPresentMode() const;

  uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags props,
                          bool* outFound = nullptr);

  // Creates a HOST_VISIBLE | HOST_COHERENT or DEVICE_LOCAL buffer.
  bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags props, VkBuffer& buffer,
                    VkDeviceMemory& memory);
  // Grow `buffer`/`memory` if needed and copy `data` (size = needed bytes)
  // into it via the persistently mapped pointer (`mapped`). On reallocation
  // the previous mapping is unmapped and `mapped` is rewritten with the new
  // map; otherwise the existing map is reused, saving a map/unmap pair per
  // frame for each of the four merged terrain buffers.
  void ensureBuffer(VkBuffer& buffer, VkDeviceMemory& memory, void*& mapped,
                    size_t& capacity, size_t needed, const void* data,
                    VkBufferUsageFlags usage);

  // ── Deferred upload pipeline ─────────────────────────────────────────────
  // Each call to createRasterTexture / createWaterMaskTexture allocates a slice
  // of the persistent staging ring, copies the pixels in, and queues a
  // PendingUpload entry. The next beginFrame() submits all pending uploads in
  // one command buffer with a single batched pipeline barrier, no
  // vkQueueWaitIdle, no per-tile staging buffer churn.
  struct PendingUpload {
    VulkanTexture* tex;
    VkDeviceSize   stagingOffset;
    uint32_t       width;
    uint32_t       height;
    uint64_t       enqueueFrame;
  };
  void initStagingRing(VkDeviceSize bytes);
  // Reserve `size` bytes from the staging ring; returns the offset and updates
  // the head. Returns SIZE_MAX if the ring is full this frame (the call site
  // falls back to an unbatched synchronous upload for that one tile).
  VkDeviceSize allocateStagingSlice(VkDeviceSize size);
  // Submit all queued uploads on the graphics queue (no wait). The submit's
  // signal fence is one of `uploadFences_`; in N frames' time we know the GPU
  // is done with the staging slices and can free their offsets.
  void flushPendingUploads();

  // Synchronous fallback used during init (fallback texture) and as the
  // last-resort path when the staging ring is exhausted for a frame.
  VkCommandBuffer beginSingleTimeCommands();
  bool            endSingleTimeCommands(VkCommandBuffer commandBuffer);

  // Generic image creator (replaces createRasterTexture / createWaterMaskTexture
  // duplication). Allocates the image, image view, sampler, descriptor set, and
  // either enqueues a deferred upload (preferred) or runs the synchronous path.
  VulkanTexture* createImageFromPixels(const uint8_t* pixels,
                                       int32_t width, int32_t height,
                                       VkDescriptorPool pool,
                                       VkSampleCountFlagBits samples,
                                       bool wantMipmaps);

  // Generates mipmap chain via blit (assumes the image has TRANSFER_SRC bit
  // and was allocated with mipLevels > 1). Records into `cmd`, leaves the
  // image in SHADER_READ_ONLY_OPTIMAL.
  void recordGenerateMipmaps(VkCommandBuffer cmd, VkImage image,
                             uint32_t width, uint32_t height,
                             uint32_t mipLevels);

  void* window_ = nullptr; // ANativeWindow*
  std::string cacheDir_;

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

  // Anisotropy / MSAA support cached at device-pick time.
  bool                  supportsAnisotropy_ = false;
  float                 maxAnisotropy_      = 1.0f;
  VkSampleCountFlagBits supportedMsaaMask_  = VK_SAMPLE_COUNT_1_BIT;
  VkSampleCountFlagBits sampleCount_        = VK_SAMPLE_COUNT_1_BIT;

  VkRenderPass             renderPass_     = VK_NULL_HANDLE;
  std::vector<VkFramebuffer> framebuffers_;

  // Single-sample depth (always present).
  VkImage        depthImage_      = VK_NULL_HANDLE;
  VkDeviceMemory depthMemory_     = VK_NULL_HANDLE;
  VkImageView    depthImageView_  = VK_NULL_HANDLE;

  // Multi-sample colour resolve target (present iff sampleCount_ > 1).
  VkImage        msaaColorImage_  = VK_NULL_HANDLE;
  VkDeviceMemory msaaColorMemory_ = VK_NULL_HANDLE;
  VkImageView    msaaColorView_   = VK_NULL_HANDLE;

  VkCommandPool                commandPool_ = VK_NULL_HANDLE;
  std::vector<VkCommandBuffer> commandBuffers_;
  std::vector<VkCommandBuffer> uploadCommandBuffers_; // one per frame-in-flight

  VkPipelineCache  pipelineCache_  = VK_NULL_HANDLE;

  VkDescriptorSetLayout terrainDescSetLayout_ = VK_NULL_HANDLE; // set 0: UBO
  VkDescriptorSetLayout terrainTexLayout_     = VK_NULL_HANDLE; // set 1: texture
  VkDescriptorSetLayout skyDescSetLayout_     = VK_NULL_HANDLE;
  VkPipelineLayout      terrainPipelineLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout      skyPipelineLayout_     = VK_NULL_HANDLE;
  // Two terrain pipelines built from the same GLSL source (compiled twice via
  // -DTERRAIN_DITHER=1 in android/CMakeLists.txt).  The solid pipeline omits
  // the LOD-fade discard so early-Z stays enabled at full speed on Adreno/Mali;
  // the dither pipeline is used only for draws where DrawPrimitive::lodFade<1.
  VkPipeline            terrainSolidPipeline_  = VK_NULL_HANDLE;
  VkPipeline            terrainDitherPipeline_ = VK_NULL_HANDLE;
  VkPipeline            skyPipeline_           = VK_NULL_HANDLE;

  VkDescriptorPool descriptorPool_     = VK_NULL_HANDLE;
  VkDescriptorPool waterMaskPool_      = VK_NULL_HANDLE;

  VulkanTexture   fallbackTexture_;
  VulkanTexture*  fallbackTexturePtr_     = nullptr;  // Heap pointer kept alive for pending upload
  VkDescriptorSet fallbackTexDescSet_     = VK_NULL_HANDLE;
  VulkanTexture   fallbackWaterMaskTex_;
  VulkanTexture*  fallbackWaterMaskTexPtr_ = nullptr;  // Heap pointer kept alive for pending upload
  VkDescriptorSet fallbackWaterMaskDescSet_ = VK_NULL_HANDLE;

  // Sky UBO — persistently mapped
  VkBuffer        skyUboBuffer_  = VK_NULL_HANDLE;
  VkDeviceMemory  skyUboMemory_  = VK_NULL_HANDLE;
  void*           skyUboMapped_  = nullptr;
  VkDescriptorSet skyDescSet_    = VK_NULL_HANDLE;

  static constexpr int kMaxFramesInFlight = tunables::kMaxFramesInFlight;
  static constexpr int kMaxRasterTextures = tunables::kMaxRasterTextures;

  // Per-frame-in-flight semaphores: signalled by acquire and waited on by the
  // frame's command-buffer submit. Sized by kMaxFramesInFlight.
  std::vector<VkSemaphore> imageAvailableSemaphores_;
  // Per-swapchain-image semaphores: signalled by the frame's submit and waited
  // on by present. Sized by swapchainImages_.size() — keyed on imageIndex_,
  // not frameIndex_. This avoids the well-known 3-image swapchain race where
  // the same per-frame semaphore can be signalled twice for two in-flight
  // images (Khronos sample VulkanGuide chapter 1).
  std::vector<VkSemaphore> renderFinishedSemaphores_;
  std::vector<VkFence>     inFlightFences_;
  int      frameIndex_  = 0;
  uint32_t imageIndex_  = 0;
  bool     frameBegan_  = false;

  // Triple-buffered persistent vertex/index/UV/altitude buffers.
  // The mapped pointers are kept alive for the lifetime of the buffer (only
  // unmapped when the underlying memory is freed on reallocation/shutdown),
  // mirroring how skyUboMapped_ / terrainUboMapped_ are handled — this saves
  // four map/unmap pairs per frame.
  VkBuffer       vtxBufs_[kMaxFramesInFlight]   = {};
  VkDeviceMemory vtxMems_[kMaxFramesInFlight]   = {};
  void*          vtxMapped_[kMaxFramesInFlight] = {};
  size_t         vtxCaps_[kMaxFramesInFlight]   = {};

  VkBuffer       idxBufs_[kMaxFramesInFlight]   = {};
  VkDeviceMemory idxMems_[kMaxFramesInFlight]   = {};
  void*          idxMapped_[kMaxFramesInFlight] = {};
  size_t         idxCaps_[kMaxFramesInFlight]   = {};

  VkBuffer       uvBufs_[kMaxFramesInFlight]    = {};
  VkDeviceMemory uvMems_[kMaxFramesInFlight]    = {};
  void*          uvMapped_[kMaxFramesInFlight]  = {};
  size_t         uvCaps_[kMaxFramesInFlight]    = {};

  VkBuffer       altBufs_[kMaxFramesInFlight]   = {};  // float altitude per vertex
  VkDeviceMemory altMems_[kMaxFramesInFlight]   = {};
  void*          altMapped_[kMaxFramesInFlight] = {};
  size_t         altCaps_[kMaxFramesInFlight]   = {};

  // ── per-slot geometry signature cache ────────────────────────────────
  // When FrameResult::geometrySignature matches the value last written into
  // this slot, the four merged buffers above already contain exactly the
  // bytes the engine would have produced this frame and the host memcpys can
  // be skipped. 0 means "this slot is stale" — backends should always
  // memcpy when geomSlotSig_[fi] is 0 or differs from the incoming sig.
  uint64_t geomSlotSig_[kMaxFramesInFlight] = {};

  // Per-frame UBO: just cameraEcef (fragment stage, used for lighting vd).
  // MVP is now per-draw via push constants.
  VkBuffer        terrainUboBufs_[kMaxFramesInFlight] = {};
  VkDeviceMemory  terrainUboMems_[kMaxFramesInFlight] = {};
  void*           terrainUboMapped_[kMaxFramesInFlight] = {};
  VkDescriptorSet terrainDescSets_[kMaxFramesInFlight] = {};

  int  viewportWidth_  = 0;
  int  viewportHeight_ = 0;
  bool needsSwapchainRecreate_ = false;
  int  pendingMsaaRequest_     = 0; // applied between frames

  // ── Persistent staging ring (P0-vk-uploads) ──────────────────────────────
  // Single host-visible buffer, persistently mapped. Each upload allocates a
  // 256-byte aligned slice; the head wraps every kStagingSize bytes. Slices
  // are kept alive for kMaxFramesInFlight frames after submit (free in
  // beginFrame when the matching upload fence is signalled).
  static constexpr VkDeviceSize kStagingSize = 64ULL * 1024ULL * 1024ULL;
  static constexpr VkDeviceSize kStagingAlignment = 256;
  // Per-batch (per beginFrame flush) allocation budget. With async upload
  // submission, the ring head must never wrap past slices still being read by
  // the GPU. Capping a single batch to kStagingSize / kMaxFramesInFlight
  // guarantees that by the time the head wraps back, the slot that wrote
  // those bytes has already had its uploadFences_ signalled (we wait on it
  // in beginFrame before clearing uploadsInFlight_). When a single batch
  // would exceed the budget the call site falls back to the synchronous
  // per-tile upload path that already exists for the pre-init case.
  static constexpr VkDeviceSize kStagingPerFrameBudget =
      kStagingSize / kMaxFramesInFlight;
  VkBuffer       stagingBuffer_ = VK_NULL_HANDLE;
  VkDeviceMemory stagingMemory_ = VK_NULL_HANDLE;
  uint8_t*       stagingMapped_ = nullptr;
  VkDeviceSize   stagingHead_   = 0;
  // Bytes allocated from the staging ring since the last flush. Reset to 0
  // inside flushPendingUploads() once those bytes have been moved into
  // uploadsInFlight_[frameIndex_].
  VkDeviceSize   stagingBytesThisBatch_ = 0;
  VkFence        uploadFences_[kMaxFramesInFlight] = {};
  std::vector<PendingUpload> pendingUploadsCurrent_;     // queued, not yet submitted
  std::vector<PendingUpload> uploadsInFlight_[kMaxFramesInFlight]; // submitted
  std::mutex                 stagingMutex_;

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
