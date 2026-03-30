#pragma once

#include "../renderer/IGPUBackend.hpp"

#ifdef __OBJC__
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#endif

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
  void* createRasterTexture(const uint8_t* pixels, int32_t width, int32_t height);

private:
  void createDepthTexture();
  void createFallbackTexture();
  void rebuildPipelinesIfNeeded();

  void* device_;               // id<MTLDevice>
  void* commandQueue_;         // id<MTLCommandQueue>
  void* metalLayer_;           // CAMetalLayer*
  void* terrainPipeline_;      // id<MTLRenderPipelineState>
  void* skyPipeline_;          // id<MTLRenderPipelineState>
  void* terrainDepthState_;    // reversed-Z GREATER, write=YES
  void* skyDepthState_;        // Always, write=NO
  void* depthTexture_;         // id<MTLTexture>
  void* fallbackTexture_;      // 1×1 white id<MTLTexture> (used when no overlay)

  // Per-frame
  void* currentDrawable_;
  void* currentCommandBuffer_;
  void* currentEncoder_;

  int viewportWidth_  = 0;
  int viewportHeight_ = 0;
};

} // namespace reactnativecesium
