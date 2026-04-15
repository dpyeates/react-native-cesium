#include "ResourcePreparer.hpp"

#include <Cesium3DTilesSelection/Tile.h>
#include <CesiumGltf/ImageAsset.h>
#include <CesiumGltf/Model.h>
#include <CesiumUtility/JsonValue.h>

#include <cstring>
#include <spdlog/spdlog.h>

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

    if (!pModel->meshes.empty() && !pModel->meshes[0].primitives.empty()) {
      const CesiumGltf::MeshPrimitive& prim = pModel->meshes[0].primitives[0];
      const CesiumUtility::JsonValue&  ex   = prim.extras;

      const auto* onlyWater = ex.getValuePtrForKey("OnlyWater");
      const auto* onlyLand  = ex.getValuePtrForKey("OnlyLand");
      const bool  isOnlyWater = onlyWater && onlyWater->getBool();
      const bool  isOnlyLand  = onlyLand  && onlyLand->getBool();

      const auto* texIdPtr = ex.getValuePtrForKey("WaterMaskTex");
      const int64_t texId  = texIdPtr ? texIdPtr->getSafeNumber<int64_t>().value_or(-2) : -3;

      const auto* wmTxPtr = ex.getValuePtrForKey("WaterMaskTranslationX");
      const auto* wmTyPtr = ex.getValuePtrForKey("WaterMaskTranslationY");
      const auto* wmSPtr  = ex.getValuePtrForKey("WaterMaskScale");
      res->wmTranslation.x = wmTxPtr ? (float)wmTxPtr->getSafeNumber<double>().value_or(0.0) : 0.0f;
      res->wmTranslation.y = wmTyPtr ? (float)wmTyPtr->getSafeNumber<double>().value_or(0.0) : 0.0f;
      res->wmScale         = wmSPtr  ? (float)wmSPtr->getSafeNumber<double>().value_or(1.0)  : 1.0f;
      res->isOnlyWater     = isOnlyWater;

      if (!isOnlyWater && !isOnlyLand) {
        if (texIdPtr && texId >= 0 &&
            static_cast<size_t>(texId) < pModel->textures.size()) {
          const int32_t srcId = pModel->textures[static_cast<size_t>(texId)].source;
          if (srcId >= 0 && static_cast<size_t>(srcId) < pModel->images.size()) {
            const auto& img = pModel->images[static_cast<size_t>(srcId)];
            const bool assetOk = img.pAsset &&
                                 img.pAsset->width == 256 &&
                                 img.pAsset->height == 256 &&
                                 img.pAsset->channels == 1 &&
                                 img.pAsset->bytesPerChannel == 1 &&
                                 img.pAsset->pixelData.size() == 65536;
            if (assetOk) {
              res->waterMaskPixels.resize(256 * 256 * 4);
              const auto* src =
                  reinterpret_cast<const uint8_t*>(img.pAsset->pixelData.data());
              for (int i = 0; i < 256 * 256; ++i) {
                const uint8_t v = src[i];
                res->waterMaskPixels[i * 4 + 0] = v;
                res->waterMaskPixels[i * 4 + 1] = v;
                res->waterMaskPixels[i * 4 + 2] = v;
                res->waterMaskPixels[i * 4 + 3] = 255;
              }
            } else {
              spdlog::warn("[WM] tile srcId={} has unexpected image format", srcId);
            }
          }
        }
      }
    }
  }

  return asyncSystem.createResolvedFuture(
      Cesium3DTilesSelection::TileLoadResultAndRenderResources{
          std::move(tileLoadResult), res});
}

void* ResourcePreparer::prepareInMainThread(
    Cesium3DTilesSelection::Tile& /*tile*/, void* pLoadThreadResult) {
  auto* res = static_cast<TileGPUResources*>(pLoadThreadResult);
  if (res) {
    res->lastUsedFrame = lifecycle_.currentFrame();
    if (!res->waterMaskPixels.empty() && waterMaskCreator_) {
      res->waterMaskTexture = waterMaskCreator_(
          res->waterMaskPixels.data(), 256, 256);
      res->waterMaskPixels.clear();
      res->waterMaskPixels.shrink_to_fit();
    } else if (!res->waterMaskPixels.empty() && !waterMaskCreator_) {
      spdlog::error("[WM] waterMaskCreator_ is null — water mask will not be uploaded");
    }
  }
  return pLoadThreadResult;
}

void ResourcePreparer::free(Cesium3DTilesSelection::Tile& /*tile*/,
                             void* pLoadThreadResult,
                             void* pMainThreadResult) noexcept {
  TileGPUResources* res = pLoadThreadResult
      ? static_cast<TileGPUResources*>(pLoadThreadResult)
      : static_cast<TileGPUResources*>(pMainThreadResult);
  if (res) {
    if (res->waterMaskTexture && waterMaskDeleter_) {
      waterMaskDeleter_(res->waterMaskTexture);
    }
    delete res;
  }
}

// ── Raster overlay pipeline ─────────────────────────────────────────────────

void* ResourcePreparer::prepareRasterInLoadThread(CesiumGltf::ImageAsset& image,
                                                   const std::any&) {
  if (image.pixelData.empty() || image.width <= 0 || image.height <= 0) {
    return nullptr;
  }
  if (image.compressedPixelFormat != CesiumGltf::GpuCompressedPixelFormat::NONE) {
    return nullptr;
  }
  if (image.bytesPerChannel != 1) return nullptr;

  auto* data = new RasterPixelData();
  data->width  = image.width;
  data->height = image.height;

  const size_t pixelCount = static_cast<size_t>(image.width) *
                            static_cast<size_t>(image.height);
  data->pixels.resize(pixelCount * 4);

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
  if (gpuTextureCreator_) {
    texture = gpuTextureCreator_(
        pixelData->pixels.data(), pixelData->width, pixelData->height);
  }

  delete pixelData;
  return texture;
}

void ResourcePreparer::freeRaster(
    const CesiumRasterOverlays::RasterOverlayTile& /*rasterTile*/,
    void* pLoadThreadResult,
    void* pMainThreadResult) noexcept {
  if (pLoadThreadResult) {
    delete static_cast<RasterPixelData*>(pLoadThreadResult);
  }
  if (pMainThreadResult && gpuTextureDeleter_) {
    gpuTextureDeleter_(pMainThreadResult);
  }
}

void ResourcePreparer::attachRasterInMainThread(
    const Cesium3DTilesSelection::Tile& tile,
    int32_t /*overlayTextureCoordinateID*/,
    const CesiumRasterOverlays::RasterOverlayTile& /*rasterTile*/,
    void* pMainThreadRendererResources,
    const glm::dvec2& translation,
    const glm::dvec2& scale) {
  const auto* renderContent = tile.getContent().getRenderContent();
  if (!renderContent) return;
  auto* res = static_cast<TileGPUResources*>(renderContent->getRenderResources());
  if (res) {
    res->overlayTexture     = pMainThreadRendererResources;
    res->overlayTranslation = glm::vec2(translation);
    res->overlayScale       = glm::vec2(scale);
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
    res->overlayTexture     = nullptr;
    res->overlayTranslation = {0.0f, 0.0f};
    res->overlayScale       = {1.0f, 1.0f};
  }
}

} // namespace reactnativecesium
