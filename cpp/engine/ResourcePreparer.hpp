#pragma once

#include "../renderer/RenderTypes.hpp"
#include "GltfToMesh.hpp"
#include "TileLifecycleManager.hpp"

#include <Cesium3DTilesSelection/IPrepareRendererResources.h>

#include <functional>
#include <vector>

namespace reactnativecesium {

// Raw decoded pixel data produced on the background load thread.
// Converted to a platform texture on the main thread via MetalTextureCreator.
struct RasterPixelData {
  std::vector<uint8_t> pixels; // RGBA8 decoded image
  int32_t width  = 0;
  int32_t height = 0;
};

// Factory: called on the MAIN THREAD to create a native Metal texture
// (id<MTLTexture> retained as void*) from decoded RGBA8 pixels.
// Set from CesiumBridge.mm before any overlays are activated.
using MetalTextureCreator =
    std::function<void*(const uint8_t* pixels, int32_t width, int32_t height)>;

class ResourcePreparer
    : public Cesium3DTilesSelection::IPrepareRendererResources {
public:
  explicit ResourcePreparer(TileLifecycleManager& lifecycle);
  ~ResourcePreparer() override = default;

  void setMetalTextureCreator(MetalTextureCreator creator) {
    metalTextureCreator_ = std::move(creator);
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
  MetalTextureCreator   metalTextureCreator_;
};

} // namespace reactnativecesium
