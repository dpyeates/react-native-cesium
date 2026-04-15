#include "CesiumBridgeAndroid.h"

#include "engine/CesiumEngine.hpp"
#include "vulkan/VulkanBackend.h"

#include <android/native_window_jni.h>
#include <android/log.h>

#include <cmath>
#include <regex>
#include <sstream>

#define LOG_TAG "CesiumBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Per-DOF smoothing constants (match iOS CesiumBridge.mm)
static const double kSmoothAlt   = 25.0;
static const double kSmoothHdg   = 30.0;
static const double kSmoothRoll  = 50.0;
static const double kSmoothPitch = 50.0;
static const double kEpsLatLon   = 1e-7;
static const double kEpsAlt      = 0.1;
static const double kEpsAngleDeg = 0.05;

static double lerpAngleDeg(double a, double b, double t) {
  double diff = std::fmod(b - a + 540.0, 360.0) - 180.0;
  return a + diff * t;
}

static double angleDeltaAbsDeg(double a, double b) {
  return std::abs(std::fmod(b - a + 540.0, 360.0) - 180.0);
}

static std::string stripHtmlToPlain(const std::string& html) {
  if (html.empty()) return "";
  static const std::regex tagRx("<[^>]+>");
  std::string plain = std::regex_replace(html, tagRx, " ");
  static const std::regex wsRx("\\s+");
  plain = std::regex_replace(plain, wsRx, " ");
  // Trim
  size_t start = plain.find_first_not_of(" \t\n\r");
  size_t end   = plain.find_last_not_of(" \t\n\r");
  if (start == std::string::npos) return "";
  return plain.substr(start, end - start + 1);
}

void CesiumBridgeAndroid::init(JNIEnv* env, jobject surface, int width, int height,
                                const std::string& cacheDir, const std::string& cacertPath) {
  cacheDir_   = cacheDir;
  cacertPath_ = cacertPath;
  viewportWidth_  = width;
  viewportHeight_ = height;

  ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
  if (!window) {
    LOGE("Failed to get ANativeWindow from Surface");
    return;
  }

  vulkanBackend_ = std::make_unique<reactnativecesium::VulkanBackend>();
  vulkanBackend_->initialize(window, width, height);

  frameResult_ = std::make_unique<reactnativecesium::FrameResult>();
  buildEngine();
  initialized_ = true;
  forceRenderNextFrame_ = true;

  LOGI("CesiumBridgeAndroid initialized %dx%d", width, height);
}

void CesiumBridgeAndroid::buildEngine() {
  reactnativecesium::EngineConfig cfg;
  cfg.ionAccessToken = ionAccessToken_;
  cfg.ionAssetId     = ionTilesetAssetId_;
  cfg.cacheDatabasePath = cacheDir_.empty() ? "" : cacheDir_ + "/cesium_cache.db";
  cfg.tlsCaBundlePath   = cacertPath_;
  cfg.maximumScreenSpaceError      = std::max(1.0, maxSSE_);
  cfg.maximumSimultaneousTileLoads = static_cast<int32_t>(std::max(0.0, std::round(maxSimLoads_)));
  cfg.loadingDescendantLimit       = static_cast<int32_t>(std::max(1.0, std::round(loadDescLim_)));

  engine_ = std::make_unique<reactnativecesium::CesiumEngine>();
  engine_->initialize(cfg);

  auto params = engine_->camera().getParams();
  camTargetLat_     = params.latitude;
  camTargetLon_     = params.longitude;
  camTargetAlt_     = params.altitude;
  camTargetHeading_ = params.heading;
  camTargetPitch_   = params.pitch;
  camTargetRoll_    = params.roll;

  auto* backendPtr = vulkanBackend_.get();
  engine_->getResourcePreparer()->setGPUTextureCreator(
      [backendPtr](const uint8_t* pixels, int32_t w, int32_t h) -> void* {
        return backendPtr->createRasterTexture(pixels, w, h);
      });
  engine_->getResourcePreparer()->setGPUTextureDeleter(
      [backendPtr](void* tex) {
        if (tex) backendPtr->freeRasterTexture(tex);
      });
  engine_->getResourcePreparer()->setWaterMaskTextureCreator(
      [backendPtr](const uint8_t* pixels, int32_t w, int32_t h) -> void* {
        return backendPtr->createWaterMaskTexture(pixels, w, h);
      });
  engine_->getResourcePreparer()->setWaterMaskTextureDeleter(
      [backendPtr](void* tex) {
        if (tex) backendPtr->freeWaterMaskTexture(tex);
      });
}

void CesiumBridgeAndroid::shutdown() {
  if (engine_) engine_->shutdown();
  if (vulkanBackend_) vulkanBackend_->shutdown();
  initialized_ = false;
}

void CesiumBridgeAndroid::resize(int width, int height) {
  viewportWidth_  = width;
  viewportHeight_ = height;
  if (vulkanBackend_) vulkanBackend_->resize(width, height);
  forceRenderNextFrame_ = true;
}

void CesiumBridgeAndroid::updateIonAccessToken(const std::string& token, int64_t assetId) {
  if (!initialized_) return;
  ionAccessToken_    = token;
  ionTilesetAssetId_ = assetId;

  reactnativecesium::EngineConfig cfg;
  cfg.ionAccessToken = ionAccessToken_;
  cfg.ionAssetId     = ionTilesetAssetId_;
  cfg.cacheDatabasePath = cacheDir_.empty() ? "" : cacheDir_ + "/cesium_cache.db";
  cfg.tlsCaBundlePath   = cacertPath_;
  cfg.maximumScreenSpaceError      = std::max(1.0, maxSSE_);
  cfg.maximumSimultaneousTileLoads = static_cast<int32_t>(std::max(0.0, std::round(maxSimLoads_)));
  cfg.loadingDescendantLimit       = static_cast<int32_t>(std::max(1.0, std::round(loadDescLim_)));

  engine_->updateConfig(cfg);
  forceRenderNextFrame_ = true;
}

void CesiumBridgeAndroid::updateImageryAssetId(int64_t assetId) {
  if (!initialized_) return;
  engine_->setImageryAssetId(assetId);
  forceRenderNextFrame_ = true;
}

void CesiumBridgeAndroid::updateCamera(double lat, double lon, double alt,
                                        double heading, double pitch, double roll) {
  if (!initialized_) return;
  camTargetLat_     = lat;
  camTargetLon_     = lon;
  camTargetAlt_     = alt;
  camTargetHeading_ = heading;
  camTargetPitch_   = pitch;
  camTargetRoll_    = roll;
  forceRenderNextFrame_ = true;
}

void CesiumBridgeAndroid::setVerticalFovDeg(double degrees) {
  if (!initialized_) return;
  engine_->camera().setVerticalFovDegrees(degrees);
  forceRenderNextFrame_ = true;
}

void CesiumBridgeAndroid::setMaximumScreenSpaceError(double v) {
  maxSSE_ = v;
  if (!initialized_) return;
  reactnativecesium::EngineConfig cfg;
  cfg.ionAccessToken = ionAccessToken_;
  cfg.ionAssetId     = ionTilesetAssetId_;
  cfg.cacheDatabasePath = cacheDir_.empty() ? "" : cacheDir_ + "/cesium_cache.db";
  cfg.tlsCaBundlePath   = cacertPath_;
  cfg.maximumScreenSpaceError      = std::max(1.0, maxSSE_);
  cfg.maximumSimultaneousTileLoads = static_cast<int32_t>(std::max(0.0, std::round(maxSimLoads_)));
  cfg.loadingDescendantLimit       = static_cast<int32_t>(std::max(1.0, std::round(loadDescLim_)));
  engine_->updateConfig(cfg);
  forceRenderNextFrame_ = true;
}

void CesiumBridgeAndroid::setMaximumSimultaneousTileLoads(int v) {
  maxSimLoads_ = static_cast<double>(v);
  if (!initialized_) return;
  setMaximumScreenSpaceError(maxSSE_); // reuses config rebuild path
}

void CesiumBridgeAndroid::setLoadingDescendantLimit(int v) {
  loadDescLim_ = static_cast<double>(v);
  if (!initialized_) return;
  setMaximumScreenSpaceError(maxSSE_);
}

void CesiumBridgeAndroid::setMsaaSampleCount(int samples) {
  if (vulkanBackend_) vulkanBackend_->setMsaaSampleCount(samples);
  forceRenderNextFrame_ = true;
}

void CesiumBridgeAndroid::markNeedsRender() {
  forceRenderNextFrame_ = true;
}

bool CesiumBridgeAndroid::shouldRenderNextFrame() {
  if (!initialized_ || !engine_) return false;

  const auto cur = engine_->camera().getParams();
  const bool cameraDirty =
      std::abs(camTargetLat_ - cur.latitude) > kEpsLatLon ||
      std::abs(camTargetLon_ - cur.longitude) > kEpsLatLon ||
      std::abs(camTargetAlt_ - cur.altitude) > kEpsAlt ||
      angleDeltaAbsDeg(cur.heading, camTargetHeading_) > kEpsAngleDeg ||
      angleDeltaAbsDeg(cur.pitch, camTargetPitch_) > kEpsAngleDeg ||
      angleDeltaAbsDeg(cur.roll, camTargetRoll_) > kEpsAngleDeg;

  return forceRenderNextFrame_ ||
         frameResult_->tilesLoading > 0 ||
         !frameResult_->tilesetActive ||
         cameraDirty;
}

void CesiumBridgeAndroid::renderFrame(double dt) {
  if (!initialized_) return;

  auto cur = engine_->camera().getParams();

  // Smooth follower: track camera target per DOF (same as iOS)
  cur.latitude  = camTargetLat_;
  cur.longitude = camTargetLon_;

  const double aAlt   = 1.0 - std::exp(-kSmoothAlt * dt);
  const double aHdg   = 1.0 - std::exp(-kSmoothHdg * dt);
  const double aPitch = 1.0 - std::exp(-kSmoothPitch * dt);
  const double aRoll  = 1.0 - std::exp(-kSmoothRoll * dt);

  cur.altitude = cur.altitude + aAlt * (camTargetAlt_ - cur.altitude);
  cur.heading  = lerpAngleDeg(cur.heading, camTargetHeading_, aHdg);
  cur.pitch    = lerpAngleDeg(cur.pitch, camTargetPitch_, aPitch);
  cur.roll     = lerpAngleDeg(cur.roll, camTargetRoll_, aRoll);

  if (std::abs(camTargetAlt_ - cur.altitude) <= kEpsAlt)
    cur.altitude = camTargetAlt_;
  if (angleDeltaAbsDeg(cur.heading, camTargetHeading_) <= kEpsAngleDeg)
    cur.heading = camTargetHeading_;
  if (angleDeltaAbsDeg(cur.pitch, camTargetPitch_) <= kEpsAngleDeg)
    cur.pitch = camTargetPitch_;
  if (angleDeltaAbsDeg(cur.roll, camTargetRoll_) <= kEpsAngleDeg)
    cur.roll = camTargetRoll_;

  engine_->camera().setParams(cur);
  engine_->updateFrame(viewportWidth_, viewportHeight_, *frameResult_);

  const double instFps = (dt > 1e-6) ? (1.0 / dt) : 0.0;
  fpsEma_ = (fpsEma_ <= 1e-6) ? instFps : (fpsEma_ * 0.85 + instFps * 0.15);

  if (++metricsTick_ >= 20) {
    metricsTick_ = 0;
    metricsFps_  = fpsEma_;
    metricsTilesRendered_      = frameResult_->tilesRendered;
    metricsTilesLoading_       = frameResult_->tilesLoading;
    metricsTilesVisited_       = frameResult_->tilesVisited;
    metricsIonTokenConfigured_ = frameResult_->ionTokenConfigured;
    metricsTilesetReady_       = frameResult_->tilesetActive;

    if (!frameResult_->creditHtmlLines.empty()) {
      std::string joined;
      for (const auto& html : frameResult_->creditHtmlLines) {
        if (!joined.empty()) joined += "|";
        joined += html;
      }
      if (joined != lastCreditHtmlJoined_) {
        lastCreditHtmlJoined_ = joined;
        std::string credits;
        for (const auto& html : frameResult_->creditHtmlLines) {
          std::string chunk = stripHtmlToPlain(html);
          if (chunk.empty()) continue;
          if (!credits.empty()) credits += " · ";
          credits += chunk;
        }
        metricsCreditsPlainText_ = credits;
      }
    } else {
      lastCreditHtmlJoined_.clear();
      metricsCreditsPlainText_.clear();
    }
  }

  reactnativecesium::FrameParams params;
  vulkanBackend_->beginFrame(params);
  vulkanBackend_->drawScene(*frameResult_);
  vulkanBackend_->endFrame();
  forceRenderNextFrame_ = false;
}

double CesiumBridgeAndroid::readCameraLatitude() {
  return engine_ ? engine_->camera().getParams().latitude : 0.0;
}
double CesiumBridgeAndroid::readCameraLongitude() {
  return engine_ ? engine_->camera().getParams().longitude : 0.0;
}
double CesiumBridgeAndroid::readCameraAltitude() {
  return engine_ ? engine_->camera().getParams().altitude : 0.0;
}
double CesiumBridgeAndroid::readCameraHeading() {
  return engine_ ? engine_->camera().getParams().heading : 0.0;
}
double CesiumBridgeAndroid::readCameraPitch() {
  return engine_ ? engine_->camera().getParams().pitch : 0.0;
}
double CesiumBridgeAndroid::readCameraRoll() {
  return engine_ ? engine_->camera().getParams().roll : 0.0;
}
double CesiumBridgeAndroid::readVerticalFovDeg() {
  return engine_ ? engine_->camera().getVerticalFovDegrees() : 60.0;
}

// ── JNI native method implementations ──────────────────────────────────────────

static CesiumBridgeAndroid* getBridge(jlong ptr) {
  return reinterpret_cast<CesiumBridgeAndroid*>(ptr);
}

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeCreate(JNIEnv*, jobject) {
  return reinterpret_cast<jlong>(new CesiumBridgeAndroid());
}

JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeInit(
    JNIEnv* env, jobject, jlong ptr, jobject surface, jint w, jint h,
    jstring cacheDir, jstring cacertPath) {
  const char* cacheDirC = env->GetStringUTFChars(cacheDir, nullptr);
  const char* cacertC   = env->GetStringUTFChars(cacertPath, nullptr);
  getBridge(ptr)->init(env, surface, w, h, cacheDirC, cacertC);
  env->ReleaseStringUTFChars(cacheDir, cacheDirC);
  env->ReleaseStringUTFChars(cacertPath, cacertC);
}

JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeShutdown(JNIEnv*, jobject, jlong ptr) {
  getBridge(ptr)->shutdown();
}

JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeDestroy(JNIEnv*, jobject, jlong ptr) {
  delete getBridge(ptr);
}

JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeResize(JNIEnv*, jobject, jlong ptr, jint w, jint h) {
  getBridge(ptr)->resize(w, h);
}

JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeUpdateIonAccessToken(
    JNIEnv* env, jobject, jlong ptr, jstring token, jlong assetId) {
  const char* tokenC = env->GetStringUTFChars(token, nullptr);
  getBridge(ptr)->updateIonAccessToken(tokenC, assetId);
  env->ReleaseStringUTFChars(token, tokenC);
}

JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeUpdateImageryAssetId(
    JNIEnv*, jobject, jlong ptr, jlong assetId) {
  getBridge(ptr)->updateImageryAssetId(assetId);
}

JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeUpdateCamera(
    JNIEnv*, jobject, jlong ptr, jdouble lat, jdouble lon, jdouble alt,
    jdouble heading, jdouble pitch, jdouble roll) {
  getBridge(ptr)->updateCamera(lat, lon, alt, heading, pitch, roll);
}

JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeSetVerticalFovDeg(
    JNIEnv*, jobject, jlong ptr, jdouble deg) {
  getBridge(ptr)->setVerticalFovDeg(deg);
}

JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeSetMaxSSE(JNIEnv*, jobject, jlong ptr, jdouble v) {
  getBridge(ptr)->setMaximumScreenSpaceError(v);
}

JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeSetMaxSimLoads(JNIEnv*, jobject, jlong ptr, jint v) {
  getBridge(ptr)->setMaximumSimultaneousTileLoads(v);
}

JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeSetLoadDescLim(JNIEnv*, jobject, jlong ptr, jint v) {
  getBridge(ptr)->setLoadingDescendantLimit(v);
}

JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeSetMsaa(JNIEnv*, jobject, jlong ptr, jint v) {
  getBridge(ptr)->setMsaaSampleCount(v);
}

JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeMarkNeedsRender(JNIEnv*, jobject, jlong ptr) {
  getBridge(ptr)->markNeedsRender();
}

JNIEXPORT jboolean JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeShouldRenderNextFrame(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->shouldRenderNextFrame() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeRenderFrame(JNIEnv*, jobject, jlong ptr, jdouble dt) {
  getBridge(ptr)->renderFrame(dt);
}

JNIEXPORT jdouble JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetCameraLat(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->readCameraLatitude();
}

JNIEXPORT jdouble JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetCameraLon(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->readCameraLongitude();
}

JNIEXPORT jdouble JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetCameraAlt(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->readCameraAltitude();
}

JNIEXPORT jdouble JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetCameraHeading(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->readCameraHeading();
}

JNIEXPORT jdouble JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetCameraPitch(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->readCameraPitch();
}

JNIEXPORT jdouble JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetCameraRoll(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->readCameraRoll();
}

JNIEXPORT jdouble JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetVerticalFovDeg(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->readVerticalFovDeg();
}

JNIEXPORT jdouble JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetMetricsFps(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->metricsFps();
}

JNIEXPORT jint JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetMetricsTilesRendered(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->metricsTilesRendered();
}

JNIEXPORT jint JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetMetricsTilesLoading(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->metricsTilesLoading();
}

JNIEXPORT jint JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetMetricsTilesVisited(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->metricsTilesVisited();
}

JNIEXPORT jboolean JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetMetricsIonTokenConfigured(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->metricsIonTokenConfigured() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetMetricsTilesetReady(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->metricsTilesetReady() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetMetricsCreditsPlainText(JNIEnv* env, jobject, jlong ptr) {
  return env->NewStringUTF(getBridge(ptr)->metricsCreditsPlainText().c_str());
}

} // extern "C"
