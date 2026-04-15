#pragma once

#include "renderer/IGPUBackend.hpp"

#ifdef __OBJC__
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#endif

#include <cstddef> // size_t

namespace reactnativecesium {

class MetalBackend : public IGPUBackend {
public:
  MetalBackend();
  ~MetalBackend() override;

  void initialize(void* nativeSurface, int width, int height) override;
  void resize(int width, int height) override;
  void shutdown() override;

  void beginFrame(const FrameParams& params) override;
  void drawScene(const FrameResult& frame) override;
  void endFrame() override;

  // Create a retained Metal texture from raw RGBA8 pixels. Returns void*
  // (retained id<MTLTexture>) that must be CFRelease'd when no longer needed.
  // Both imagery overlays and water mask textures use this same function; they
  // are distinguished only by which texture slot they are bound to at draw time.
  void* createRasterTexture(const uint8_t* pixels, int32_t width, int32_t height);

  /// 1 = off; 2 or 4 = MSAA when the device supports it. May block briefly
  /// to drain in-flight frames before rebuilding pipelines.
  void setMsaaSampleCount(int sampleCount);

private:
  void buildRenderPipelines();
  void createDepthTexture();
  void createFallbackTexture();
  void createWaterMaskFallback();

  void* device_;               // id<MTLDevice>
  void* commandQueue_;         // id<MTLCommandQueue>
  void* metalLayer_;           // CAMetalLayer* (weak — owned by the view)
  void* terrainPipeline_;      // id<MTLRenderPipelineState>
  void* skyPipeline_;          // id<MTLRenderPipelineState>
  void* terrainDepthState_;    // reversed-Z GREATER, write=YES
  void* skyDepthState_;        // Always, write=NO
  void* depthTexture_;              // id<MTLTexture>
  void* fallbackTexture_;           // 1×1 white id<MTLTexture>  (imagery fallback)
  void* fallbackWaterMaskTexture_;  // 1×1 zero  id<MTLTexture>  (water mask fallback)

  // Per-frame bookkeeping
  void* currentDrawable_;
  void* currentCommandBuffer_;
  void* currentEncoder_;

  int viewportWidth_  = 0;
  int viewportHeight_ = 0;
  int sampleCount_    = 1; // MSAA: 1, 2, or 4

  // ── Triple-buffered persistent vertex / index / UV buffers ────────────────
  // Using kMaxFramesInFlight slots avoids CPU-GPU data hazards without
  // allocating new MTLBuffers every frame.  The dispatch_semaphore limits the
  // CPU to kMaxFramesInFlight frames ahead of the GPU.
  static constexpr int kMaxFramesInFlight = 3;

  void* frameSemaphore_;  // dispatch_semaphore_t (stored as void* to keep header C++)
  int   frameIndex_ = 0;

  void*  vtxBufs_[kMaxFramesInFlight]; // id<MTLBuffer> — packed_float3 tile-local positions
  void*  idxBufs_[kMaxFramesInFlight]; // id<MTLBuffer> — uint32 indices
  void*  uvBufs_[kMaxFramesInFlight];  // id<MTLBuffer> — packed_float2 UVs
  void*  altBufs_[kMaxFramesInFlight]; // id<MTLBuffer> — float altitudes (metres, from double)
  size_t vtxCaps_[kMaxFramesInFlight]; // allocated byte capacities
  size_t idxCaps_[kMaxFramesInFlight];
  size_t uvCaps_[kMaxFramesInFlight];
  size_t altCaps_[kMaxFramesInFlight];
};

} // namespace reactnativecesium
