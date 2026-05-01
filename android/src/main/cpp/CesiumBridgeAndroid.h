#pragma once

#include <jni.h>
#include <memory>
#include <string>

#include "engine/CameraTargetState.hpp"
#include "engine/CesiumEngine.hpp"
#include "engine/MetricsAggregator.hpp"

namespace reactnativecesium {
class VulkanBackend;
struct FrameResult;
} // namespace reactnativecesium

// Thin Android shell. All cross-platform logic (camera demand state +
// smoothing, FPS / metrics aggregation, credit HTML stripping) lives in
// shared C++ next to CesiumEngine. This class only owns:
//   - the JNI/window surface,
//   - the Vulkan backend instance, and
//   - the EngineConfig snapshot mirrored from JS props.
struct CesiumBridgeAndroid {
  void init(JNIEnv* env, jobject surface, int width, int height,
            const std::string& cacheDir, const std::string& cacertPath);
  void shutdown();

  void resize(int width, int height);

  void updateIonAccessToken(const std::string& token, int64_t assetId);
  void updateImageryAssetId(int64_t assetId);
  void updateCamera(double lat, double lon, double alt,
                    double heading, double pitch, double roll);
  void updateCameraQuaternion(double lat, double lon, double alt,
                              double heading, double pitch, double roll,
                              double qw, double qx, double qy, double qz);
  void setVerticalFovDeg(double degrees);
  void setMaximumScreenSpaceError(double v);
  void setMaximumSimultaneousTileLoads(int32_t v);
  void setLoadingDescendantLimit(int32_t v);
  void setMsaaSampleCount(int samples);

  // Optional knobs (default values mirror EngineConfig defaults).
  void setMaximumCachedMiB(int32_t v);
  void setPreloadAncestors(bool v);
  void setPreloadSiblings(bool v);
  void setForbidHoles(bool v);
  void setEnableWaterMask(bool v);
  void setEnableFogCulling(bool v);
  void setEnforceCulledScreenSpaceError(bool v);
  void setCulledScreenSpaceError(double v);
  void setEnableLodTransitionPeriod(bool v);
  void setLodTransitionLength(double v);
  void setSqliteCacheMaxRows(int32_t v);
  void setTaskProcessorThreads(int32_t v);

  void markNeedsRender();
  bool shouldRenderNextFrame();
  void renderFrame(double dt);

  // Camera readback
  double readCameraLatitude();
  double readCameraLongitude();
  double readCameraAltitude();
  double readCameraHeading();
  double readCameraPitch();
  double readCameraRoll();
  double readVerticalFovDeg();
  double readViewCorrectionW();
  double readViewCorrectionX();
  double readViewCorrectionY();
  double readViewCorrectionZ();

  // Metrics readback (delegated to MetricsAggregator)
  double metricsFps()                const { return metrics_.latest().fps; }
  int    metricsTilesRendered()      const { return metrics_.latest().tilesRendered; }
  int    metricsTilesLoading()       const { return metrics_.latest().tilesLoading; }
  int    metricsTilesVisited()       const { return metrics_.latest().tilesVisited; }
  bool   metricsIonTokenConfigured() const { return metrics_.latest().ionTokenConfigured; }
  bool   metricsTilesetReady()       const { return metrics_.latest().tilesetReady; }
  bool   metricsTlsConfigured()      const { return metrics_.latest().tlsConfigured; }
  const std::string& metricsCreditsPlainText() const {
    return metrics_.latest().creditsPlainText;
  }

private:
  void buildEngine();
  // Push the current `config_` state to the live engine (runtime-mutable
  // fields go through tileset_->getOptions(); token/asset id changes force
  // a tileset rebuild).
  void applyEngineConfig();

  std::unique_ptr<reactnativecesium::VulkanBackend> vulkanBackend_;
  std::unique_ptr<reactnativecesium::CesiumEngine>  engine_;
  std::unique_ptr<reactnativecesium::FrameResult>   frameResult_;

  int  viewportWidth_  = 0;
  int  viewportHeight_ = 0;
  bool initialized_    = false;

  reactnativecesium::EngineConfig config_;
  reactnativecesium::CameraTargetState target_;
  reactnativecesium::MetricsAggregator metrics_;
};
