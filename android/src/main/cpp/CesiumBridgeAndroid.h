#pragma once

#include <atomic>
#include <jni.h>
#include <memory>
#include <string>

#include "engine/CameraIntegrator.hpp"
#include "engine/CesiumEngine.hpp"
#include "engine/MetricsAggregator.hpp"

struct ANativeWindow;

namespace reactnativecesium {
class VulkanBackend;
struct FrameResult;
} // namespace reactnativecesium

// Thin Android shell. All cross-platform logic (per-DoF camera integrator,
// FPS / metrics aggregation, credit HTML stripping) lives in shared C++ next
// to CesiumEngine. This class only owns:
//   - the JNI/window surface,
//   - the Vulkan backend instance, and
//   - the EngineConfig snapshot mirrored from JS props.
struct CesiumBridgeAndroid {
  CesiumBridgeAndroid() = default;
  // Defensive: if Kotlin ever forgets to call shutdown() before destroy() the
  // destructor still tears down the engine and releases the ANativeWindow
  // reference acquired in init(), preventing native handle / SQLite handle
  // leaks.
  ~CesiumBridgeAndroid();

  CesiumBridgeAndroid(const CesiumBridgeAndroid&) = delete;
  CesiumBridgeAndroid& operator=(const CesiumBridgeAndroid&) = delete;
  CesiumBridgeAndroid(CesiumBridgeAndroid&&) = delete;
  CesiumBridgeAndroid& operator=(CesiumBridgeAndroid&&) = delete;

  void init(JNIEnv* env, jobject surface, int width, int height,
            const std::string& cacheDir, const std::string& cacertPath,
            const std::string& ionAccessToken, int64_t ionAssetId);
  void shutdown();

  void resize(int width, int height);

  void updateIonAccessToken(const std::string& token, int64_t assetId);
  void updateImageryAssetId(int64_t assetId);

  // ── Per-DoF camera demand setters ──────────────────────────────────────
  void setPosition(double lat, double lon);
  void setAltitude(double alt);
  void setHeading(double headingDeg);
  void setAttitude(double pitchDeg, double rollDeg);
  void setViewCorrection(double qw, double qx, double qy, double qz);
  void teleport(double lat, double lon, double alt,
                double heading, double pitch, double roll, double vfov);

  void setVerticalFovDeg(double degrees);
  void setRateCaps(double yawDegSec, double pitchDegSec, double rollDegSec,
                   double climbMps, double groundMps);

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
  void setMinAltitudeAboveTerrain(float v);

  void markNeedsRender();
  bool shouldRenderNextFrame();
  // Wall-clock seconds (steady_clock), used by the integrator to extrapolate
  // every DoF forward to the time we are about to render.
  void renderFrame(double nowSeconds);

  // Actual camera readback (post-integration; what was rendered).
  double readActualLatitude();
  double readActualLongitude();
  double readActualAltitude();
  double readActualHeading();
  double readActualPitch();
  double readActualRoll();
  double readActualVerticalFovDeg();
  // Demand camera readback (what the consumer last asked for).
  double readDemandLatitude();
  double readDemandLongitude();
  double readDemandAltitude();
  double readDemandHeading();
  double readDemandPitch();
  double readDemandRoll();
  double readDemandVerticalFovDeg();

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
  // Push the current `config_` state to the live engine.
  void applyEngineConfig();

  std::unique_ptr<reactnativecesium::VulkanBackend>     vulkanBackend_;
  std::unique_ptr<reactnativecesium::CesiumEngine>      engine_;
  std::unique_ptr<reactnativecesium::FrameResult>       frameResult_;
  std::unique_ptr<reactnativecesium::CameraIntegrator>  integrator_;

  // ANativeWindow* acquired via ANativeWindow_fromSurface in init(). The
  // bridge owns one strong reference and must release it in shutdown() once
  // the Vulkan backend (which renders into it) is gone. Leaving this dangling
  // leaks one ANativeWindow per view create/destroy cycle.
  ANativeWindow*                                        nativeWindow_ = nullptr;

  int  viewportWidth_  = 0;
  int  viewportHeight_ = 0;
  bool initialized_    = false;

  // Force-render flag — set on every demand change so the render loop knows
  // to dispatch at least one frame.
  std::atomic<bool> forceRender_{true};

  reactnativecesium::EngineConfig      config_;
  reactnativecesium::MetricsAggregator metrics_;
  double                               lastTickSeconds_ = 0.0;
};
