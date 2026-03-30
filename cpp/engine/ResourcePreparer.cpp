#include "ResourcePreparer.hpp"

#include <Cesium3DTilesSelection/Tile.h>
#include <CesiumGltf/ImageAsset.h>
#include <CesiumGltf/Model.h>

#include <cstring>

// CoreFoundation is a pure-C API available on Apple platforms; we use
// CFRelease to release ARC-retained Metal objects stored as void*.
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace reactnativecesium {

ResourcePreparer::ResourcePreparer(TileLifecycleManager& lifecycle)
    : lifecycle_(lifecycle) {}

CesiumAsync::Future<Cesium3DTilesSelection::TileLoadResultAndRenderResources>
ResourcePreparer::prepareInLoadThread(
    const CesiumAsync::AsyncSystem& asyncSystem,
    Cesium3DTilesSelection::TileLoadResult&& tileLoadResult,
    const glm::dmat4& transform,
    const std::any& /*rendererOptions*/) {

  const CesiumGltf::Model* pModel =
      std::get_if<CesiumGltf::Model>(&tileLoadResult.contentKind);

  TileGPUResources* res = new TileGPUResources();
  if (pModel) {
    res->primitives = GltfToMesh::convert(*pModel, transform);
  }

  return asyncSystem.createResolvedFuture(
      Cesium3DTilesSelection::TileLoadResultAndRenderResources{
          std::move(tileLoadResult), res});
}

void* ResourcePreparer::prepareInMainThread(
    Cesium3DTilesSelection::Tile& /*tile*/, void* pLoadThreadResult) {
  if (auto* res = static_cast<TileGPUResources*>(pLoadThreadResult)) {
    res->lastUsedFrame = lifecycle_.currentFrame();
  }
  return pLoadThreadResult;
}

void ResourcePreparer::free(Cesium3DTilesSelection::Tile& /*tile*/,
                             void* pLoadThreadResult,
                             void* pMainThreadResult) noexcept {
  if (pLoadThreadResult) {
    delete static_cast<TileGPUResources*>(pLoadThreadResult);
  } else if (pMainThreadResult) {
    delete static_cast<TileGPUResources*>(pMainThreadResult);
  }
}

// ── Raster overlay pipeline ─────────────────────────────────────────────────

void* ResourcePreparer::prepareRasterInLoadThread(CesiumGltf::ImageAsset& image,
                                                   const std::any&) {
  // Guard against invalid or compressed images.
  if (image.pixelData.empty() || image.width <= 0 || image.height <= 0) {
    return nullptr;
  }
  // Skip GPU-compressed formats (cannot decode to RGBA8 on CPU here).
  if (image.compressedPixelFormat != CesiumGltf::GpuCompressedPixelFormat::NONE) {
    return nullptr;
  }
  if (image.bytesPerChannel != 1) return nullptr; // only 8-bit supported

  auto* data = new RasterPixelData();
  data->width  = image.width;
  data->height = image.height;

  const size_t pixelCount = static_cast<size_t>(image.width) *
                            static_cast<size_t>(image.height);
  data->pixels.resize(pixelCount * 4); // RGBA8

  const auto* src = reinterpret_cast<const uint8_t*>(image.pixelData.data());

  if (image.channels == 4) {
    std::memcpy(data->pixels.data(), src, pixelCount * 4);
  } else if (image.channels == 3) {
    for (size_t i = 0; i < pixelCount; ++i) {
      data->pixels[i * 4 + 0] = src[i * 3 + 0];
      data->pixels[i * 4 + 1] = src[i * 3 + 1];
      data->pixels[i * 4 + 2] = src[i * 3 + 2];
      data->pixels[i * 4 + 3] = 255;
    }
  } else if (image.channels == 1) {
    for (size_t i = 0; i < pixelCount; ++i) {
      uint8_t v = src[i];
      data->pixels[i * 4 + 0] = v;
      data->pixels[i * 4 + 1] = v;
      data->pixels[i * 4 + 2] = v;
      data->pixels[i * 4 + 3] = 255;
    }
  } else {
    // Unsupported channel count — fill white placeholder.
    std::fill(data->pixels.begin(), data->pixels.end(), uint8_t(255));
  }

  return data;
}

void* ResourcePreparer::prepareRasterInMainThread(
    CesiumRasterOverlays::RasterOverlayTile& /*rasterTile*/,
    void* pLoadThreadResult) {
  auto* pixelData = static_cast<RasterPixelData*>(pLoadThreadResult);
  if (!pixelData) return nullptr;

  void* texture = nullptr;
  if (metalTextureCreator_) {
    texture = metalTextureCreator_(
        pixelData->pixels.data(), pixelData->width, pixelData->height);
  }

  delete pixelData;
  return texture; // retained id<MTLTexture> as void*
}

void ResourcePreparer::freeRaster(
    const CesiumRasterOverlays::RasterOverlayTile& /*rasterTile*/,
    void* pLoadThreadResult,
    void* pMainThreadResult) noexcept {
  // If prepareRasterInMainThread was never called, free the pixel data.
  if (pLoadThreadResult) {
    delete static_cast<RasterPixelData*>(pLoadThreadResult);
  }
  // Release the retained Metal texture.
#ifdef __APPLE__
  if (pMainThreadResult) {
    CFRelease(pMainThreadResult);
  }
#endif
}

void ResourcePreparer::attachRasterInMainThread(
    const Cesium3DTilesSelection::Tile& tile,
    int32_t /*overlayTextureCoordinateID*/,
    const CesiumRasterOverlays::RasterOverlayTile& /*rasterTile*/,
    void* pMainThreadRendererResources,
    const glm::dvec2& /*translation*/,
    const glm::dvec2& /*scale*/) {
  const auto* renderContent = tile.getContent().getRenderContent();
  if (!renderContent) return;
  // getRenderResources() returns the void* we set in prepareInMainThread.
  // The underlying TileGPUResources object is mutable even though the tile is const.
  // getRenderResources() returns void* even on const TileRenderContent.
  auto* res = static_cast<TileGPUResources*>(renderContent->getRenderResources());
  if (res) {
    res->overlayTexture = pMainThreadRendererResources;
  }
}

void ResourcePreparer::detachRasterInMainThread(
    const Cesium3DTilesSelection::Tile& tile,
    int32_t /*overlayTextureCoordinateID*/,
    const CesiumRasterOverlays::RasterOverlayTile& /*rasterTile*/,
    void* /*pMainThreadRendererResources*/) noexcept {
  const auto* renderContent = tile.getContent().getRenderContent();
  if (!renderContent) return;
  auto* res = static_cast<TileGPUResources*>(renderContent->getRenderResources());
  if (res) {
    res->overlayTexture = nullptr;
  }
}

} // namespace reactnativecesium
