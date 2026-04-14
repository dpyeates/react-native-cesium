#pragma once

#include <jni.h>
#include <memory>
#include <string>

namespace reactnativecesium {
class VulkanBackend;
class CesiumEngine;
struct FrameResult;
struct CameraParams;
}

struct CesiumBridgeAndroid {
  void init(JNIEnv* env, jobject surface, int width, int height,
            const std::string& cacheDir, const std::string& cacertPath);
  void shutdown();

  void resize(int width, int height);

  void updateIonAccessToken(const std::string& token, int64_t assetId);
  void updateImageryAssetId(int64_t assetId);
  void updateCamera(double lat, double lon, double alt,
                    double heading, double pitch, double roll);
  void setVerticalFovDeg(double degrees);
  void setMaximumScreenSpaceError(double v);
  void setMaximumSimultaneousTileLoads(int v);
  void setLoadingDescendantLimit(int v);
  void setMsaaSampleCount(int samples);

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

  // Metrics
  double metricsFps() const { return metricsFps_; }
  int    metricsTilesRendered() const { return metricsTilesRendered_; }
  int    metricsTilesLoading() const { return metricsTilesLoading_; }
  int    metricsTilesVisited() const { return metricsTilesVisited_; }
  bool   metricsIonTokenConfigured() const { return metricsIonTokenConfigured_; }
  bool   metricsTilesetReady() const { return metricsTilesetReady_; }
  const std::string& metricsCreditsPlainText() const { return metricsCreditsPlainText_; }

private:
  void buildEngine();
  reactnativecesium::CameraParams makeCamTarget();

  std::unique_ptr<reactnativecesium::VulkanBackend> vulkanBackend_;
  std::unique_ptr<reactnativecesium::CesiumEngine>  engine_;
  std::unique_ptr<reactnativecesium::FrameResult>   frameResult_;

  int viewportWidth_  = 0;
  int viewportHeight_ = 0;
  bool initialized_   = false;

  std::string cacheDir_;
  std::string cacertPath_;
  std::string ionAccessToken_;
  int64_t     ionTilesetAssetId_ = 1;

  double maxSSE_       = 32.0;
  double maxSimLoads_  = 12.0;
  double loadDescLim_  = 20.0;

  // Camera demand target
  double camTargetLat_     = 46.15;
  double camTargetLon_     = 7.35;
  double camTargetAlt_     = 12000.0;
  double camTargetHeading_ = 129.0;
  double camTargetPitch_   = -45.0;
  double camTargetRoll_    = 0.0;
  bool   forceRenderNextFrame_ = true;

  // Metrics
  double fpsEma_       = 0.0;
  int    metricsTick_  = 0;
  double metricsFps_   = 0.0;
  int    metricsTilesRendered_ = 0;
  int    metricsTilesLoading_  = 0;
  int    metricsTilesVisited_  = 0;
  bool   metricsIonTokenConfigured_ = false;
  bool   metricsTilesetReady_ = false;
  std::string metricsCreditsPlainText_;
  std::string lastCreditHtmlJoined_;
};
