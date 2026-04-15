#pragma once

#include "../renderer/RenderTypes.hpp"
#include "GltfToMesh.hpp"
#include "TileLifecycleManager.hpp"

#include <Cesium3DTilesSelection/IPrepareRendererResources.h>

#include <functional>
#include <vector>

namespace reactnativecesium {

// Raw decoded pixel data produced on the background load thread.
// Converted to a platform texture on the main thread via GPUTextureCreator.
struct RasterPixelData {
  std::vector<uint8_t> pixels; // RGBA8 decoded image
  int32_t width  = 0;
  int32_t height = 0;
};

// Factory: called on the MAIN THREAD to create a native GPU texture
// (id<MTLTexture> on iOS, VulkanTexture* on Android) from decoded RGBA8 pixels.
// Set from the platform bridge before any overlays are activated.
using GPUTextureCreator =
    std::function<void*(const uint8_t* pixels, int32_t width, int32_t height)>;

// Destructor: called to free a GPU texture previously created by GPUTextureCreator.
// On iOS: CFRelease(tex). On Android: VulkanBackend::freeRasterTexture(tex).
using GPUTextureDeleter = std::function<void(void* tex)>;

class ResourcePreparer
    : public Cesium3DTilesSelection::IPrepareRendererResources {
public:
  explicit ResourcePreparer(TileLifecycleManager& lifecycle);
  ~ResourcePreparer() override = default;

  void setGPUTextureCreator(GPUTextureCreator creator) {
    gpuTextureCreator_ = std::move(creator);
  }
  void setGPUTextureDeleter(GPUTextureDeleter deleter) {
    gpuTextureDeleter_ = std::move(deleter);
  }

  // Water mask texture lifecycle — identical signature to the imagery callbacks
  // but the platform allocates from a different descriptor pool (Vulkan Set 2).
  void setWaterMaskTextureCreator(GPUTextureCreator creator) {
    waterMaskCreator_ = std::move(creator);
  }
  void setWaterMaskTextureDeleter(GPUTextureDeleter deleter) {
    waterMaskDeleter_ = std::move(deleter);
  }

  CesiumAsync::Future<Cesium3DTilesSelection::TileLoadResultAndRenderResources>
  prepareInLoadThread(
      const CesiumAsync::AsyncSystem& asyncSystem,
      Cesium3DTilesSelection::TileLoadResult&& tileLoadResult,
      const glm::dmat4& transform,
      const std::any& rendererOptions) override;

  void* prepareInMainThread(Cesium3DTilesSelection::Tile& tile,
                            void* pLoadThreadResult) override;

  void free(Cesium3DTilesSelection::Tile& tile, void* pLoadThreadResult,
            void* pMainThreadResult) noexcept override;

  void* prepareRasterInLoadThread(CesiumGltf::ImageAsset& image,
                                  const std::any& rendererOptions) override;
  void* prepareRasterInMainThread(
      CesiumRasterOverlays::RasterOverlayTile& rasterTile,
      void* pLoadThreadResult) override;
  void freeRaster(const CesiumRasterOverlays::RasterOverlayTile& rasterTile,
                  void* pLoadThreadResult,
                  void* pMainThreadResult) noexcept override;
  void attachRasterInMainThread(
      const Cesium3DTilesSelection::Tile& tile,
      int32_t overlayTextureCoordinateID,
      const CesiumRasterOverlays::RasterOverlayTile& rasterTile,
      void* pMainThreadRendererResources,
      const glm::dvec2& translation,
      const glm::dvec2& scale) override;
  void detachRasterInMainThread(
      const Cesium3DTilesSelection::Tile& tile,
      int32_t overlayTextureCoordinateID,
      const CesiumRasterOverlays::RasterOverlayTile& rasterTile,
      void* pMainThreadRendererResources) noexcept override;

private:
  TileLifecycleManager& lifecycle_;
  GPUTextureCreator     gpuTextureCreator_;
  GPUTextureDeleter     gpuTextureDeleter_;
  GPUTextureCreator     waterMaskCreator_;
  GPUTextureDeleter     waterMaskDeleter_;
};

} // namespace reactnativecesium
