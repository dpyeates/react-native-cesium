#pragma once

#include "RenderTypes.hpp"

#include <glm/glm.hpp>

namespace reactnativecesium {

class IGPUBackend {
public:
  virtual ~IGPUBackend() = default;

  virtual void initialize(void* nativeSurface, int width, int height) = 0;
  virtual void resize(int width, int height) = 0;
  virtual void shutdown() = 0;

  virtual void beginFrame(const FrameParams& params) = 0;
  virtual void drawScene(const FrameResult& frame) = 0;
  virtual void endFrame() = 0;
};

} // namespace reactnativecesium
