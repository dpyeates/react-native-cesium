#include "CesiumBridgeAndroid.h"

#include "engine/CameraSmoother.hpp"
#include "engine/EngineTunables.hpp"
#include "engine/GeoidConverter.hpp"
#include "vulkan/VulkanBackend.h"

#include <android/native_window_jni.h>
#include <android/log.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

#define LOG_TAG "CesiumBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

void CesiumBridgeAndroid::init(JNIEnv* env, jobject surface, int width, int height,
                               const std::string& cacheDir,
                               const std::string& cacertPath) {
  config_.cacheDatabasePath = cacheDir.empty() ? "" : cacheDir + "/cesium_cache.db";
  config_.tlsCaBundlePath   = cacertPath;
  viewportWidth_  = width;
  viewportHeight_ = height;

  ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
  if (!window) {
    LOGE("Failed to get ANativeWindow from Surface");
    return;
  }

  vulkanBackend_ = std::make_unique<reactnativecesium::VulkanBackend>();
  vulkanBackend_->setCacheDir(cacheDir);
  vulkanBackend_->initialize(window, width, height);

  frameResult_ = std::make_unique<reactnativecesium::FrameResult>();
  buildEngine();
  initialized_ = true;
  target_.requestForceRender();
}

void CesiumBridgeAndroid::buildEngine() {
  engine_ = std::make_unique<reactnativecesium::CesiumEngine>();
  engine_->initialize(config_);

  // Seed the demand target from the engine's initial camera so the first
  // frame is not a "snap from default" jump.
  target_.setAll(engine_->camera().getParams());

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

void CesiumBridgeAndroid::applyEngineConfig() {
  if (!engine_) return;
  engine_->updateConfig(config_);
  target_.requestForceRender();
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
  target_.requestForceRender();
}

void CesiumBridgeAndroid::updateIonAccessToken(const std::string& token,
                                               int64_t assetId) {
  config_.ionAccessToken = token;
  config_.ionAssetId     = assetId;
  if (initialized_) applyEngineConfig();
}

void CesiumBridgeAndroid::updateImageryAssetId(int64_t assetId) {
  config_.ionImageryAssetId = (assetId <= 0) ? 1 : assetId;
  if (initialized_ && engine_) {
    engine_->setImageryAssetId(config_.ionImageryAssetId);
    target_.requestForceRender();
  }
}

void CesiumBridgeAndroid::updateCamera(double lat, double lon, double alt,
                                       double heading, double pitch, double roll) {
  if (!initialized_) return;
  target_.setHpr(lat, lon, alt, heading, pitch, roll);
}

void CesiumBridgeAndroid::updateCameraQuaternion(
    double lat, double lon, double alt, double heading, double pitch,
    double roll, double qw, double qx, double qy, double qz) {
  if (!initialized_) return;
  target_.setHpr(lat, lon, alt, heading, pitch, roll);
  const double ql2 = qw * qw + qx * qx + qy * qy + qz * qz;
  glm::dquat q;
  if (ql2 < 1e-20) {
    q = glm::dquat(1.0, 0.0, 0.0, 0.0);
  } else {
    const double inv = 1.0 / std::sqrt(ql2);
    q = glm::dquat(qw * inv, qx * inv, qy * inv, qz * inv);
  }
  target_.setViewCorrection(q);
}

void CesiumBridgeAndroid::setVerticalFovDeg(double degrees) {
  if (!initialized_) return;
  engine_->camera().setVerticalFovDegrees(degrees);
  target_.requestForceRender();
}

void CesiumBridgeAndroid::setMaximumScreenSpaceError(double v) {
  config_.maximumScreenSpaceError = v;
  if (initialized_) applyEngineConfig();
}
void CesiumBridgeAndroid::setMaximumSimultaneousTileLoads(int32_t v) {
  config_.maximumSimultaneousTileLoads = v;
  if (initialized_) applyEngineConfig();
}
void CesiumBridgeAndroid::setLoadingDescendantLimit(int32_t v) {
  config_.loadingDescendantLimit = v;
  if (initialized_) applyEngineConfig();
}

void CesiumBridgeAndroid::setMsaaSampleCount(int samples) {
  if (vulkanBackend_) vulkanBackend_->setMsaaSampleCount(samples);
  target_.requestForceRender();
}

void CesiumBridgeAndroid::setMaximumCachedMiB(int32_t v) {
  config_.maximumCachedBytes =
      static_cast<int64_t>(std::max(16, v)) * 1024LL * 1024LL;
  if (initialized_) applyEngineConfig();
}
void CesiumBridgeAndroid::setPreloadAncestors(bool v) {
  config_.preloadAncestors = v;
  if (initialized_) applyEngineConfig();
}
void CesiumBridgeAndroid::setPreloadSiblings(bool v) {
  config_.preloadSiblings = v;
  if (initialized_) applyEngineConfig();
}
void CesiumBridgeAndroid::setForbidHoles(bool v) {
  config_.forbidHoles = v;
  if (initialized_) applyEngineConfig();
}
void CesiumBridgeAndroid::setEnableWaterMask(bool v) {
  config_.enableWaterMask = v;
  if (initialized_) applyEngineConfig();
}
void CesiumBridgeAndroid::setEnableFogCulling(bool v) {
  config_.enableFogCulling = v;
  if (initialized_) applyEngineConfig();
}
void CesiumBridgeAndroid::setEnforceCulledScreenSpaceError(bool v) {
  config_.enforceCulledScreenSpaceError = v;
  if (initialized_) applyEngineConfig();
}
void CesiumBridgeAndroid::setCulledScreenSpaceError(double v) {
  config_.culledScreenSpaceError = v;
  if (initialized_) applyEngineConfig();
}
void CesiumBridgeAndroid::setEnableLodTransitionPeriod(bool v) {
  config_.enableLodTransitionPeriod = v;
  if (initialized_) applyEngineConfig();
}
void CesiumBridgeAndroid::setLodTransitionLength(double v) {
  config_.lodTransitionLength = v;
  if (initialized_) applyEngineConfig();
}
void CesiumBridgeAndroid::setSqliteCacheMaxRows(int32_t v) {
  // Re-creating SqliteCache requires a tileset rebuild, so we only honour the
  // value on next init.
  config_.sqliteCacheMaxRows = std::max(64, v);
}
void CesiumBridgeAndroid::setTaskProcessorThreads(int32_t v) {
  // Worker pool is sized once per CesiumEngine. We mirror it for the next
  // initialize().
  config_.taskProcessorThreads = std::max(0, v);
}
void CesiumBridgeAndroid::setMinAltitudeAboveTerrain(float v) {
  config_.minAltitudeAboveTerrain = std::max(0.0f, v);
  if (initialized_) applyEngineConfig();
}

void CesiumBridgeAndroid::markNeedsRender() { target_.requestForceRender(); }

bool CesiumBridgeAndroid::shouldRenderNextFrame() {
  if (!initialized_ || !engine_) return false;

  if (target_.consumeForceRender()) {
    // Force-render request observed: keep it sticky for one frame so we make
    // it through to the actual GPU encode below.
    target_.requestForceRender();
    return true;
  }
  if (frameResult_->tilesLoading > 0 || !frameResult_->tilesetActive) {
    return true;
  }
  // Keep rendering at full speed until we have actual terrain tiles loaded.
  // This ensures tiles start loading immediately instead of falling into idle
  // mode (250ms intervals) before any content appears.
  if (frameResult_->tilesRendered < 5) {
    return true;
  }
  if (target_.isDirty()) {
    const auto cur = engine_->camera().getParams();
    const auto tgt = target_.snapshot();
    if (reactnativecesium::CameraTargetState::deltaExceedsEpsilon(cur, tgt)) {
      return true;
    }
    target_.clearDirty();
  }
  return false;
}

void CesiumBridgeAndroid::renderFrame(double dt) {
  if (!initialized_) return;

  // Consume the force-render flag here (it may have been re-armed by
  // shouldRenderNextFrame; either way we're rendering this frame).
  (void)target_.consumeForceRender();

  const auto cur    = engine_->camera().getParams();
  const auto tgt    = target_.snapshot();
  auto smooth = reactnativecesium::CameraSmoother::step(cur, tgt, dt);

  // ── Terrain floor clamp ───────────────────────────────────────────────
  const float minAbove = engine_->getConfig().minAltitudeAboveTerrain;
  if (minAbove > 0.0f) {
    const double geoidOffset =
        reactnativecesium::mslToEllipsoidMeters(smooth.latitude, smooth.longitude, 0.0);
    const double camEllipsoid =
        reactnativecesium::mslToEllipsoidMeters(smooth.latitude, smooth.longitude, smooth.altitude);
    const double minEllipsoid =
        static_cast<double>(engine_->terrainFloorEllipsoidMeters()) + minAbove;
    if (camEllipsoid < minEllipsoid) {
      smooth.altitude = minEllipsoid - geoidOffset;
    }
  }

  engine_->camera().setParams(smooth);

  // If we converged, clear the dirty flag so the next idle vsync can early-out.
  if (!reactnativecesium::CameraTargetState::deltaExceedsEpsilon(smooth, tgt)) {
    target_.clearDirty();
  }

  engine_->updateFrame(viewportWidth_, viewportHeight_, *frameResult_);
  metrics_.tick(dt, *frameResult_, !config_.tlsCaBundlePath.empty());

  reactnativecesium::FrameParams params;
  vulkanBackend_->beginFrame(params);
  vulkanBackend_->drawScene(*frameResult_);
  vulkanBackend_->endFrame();
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
double CesiumBridgeAndroid::readViewCorrectionW() {
  return engine_ ? engine_->camera().getParams().viewCorrection.w : 1.0;
}
double CesiumBridgeAndroid::readViewCorrectionX() {
  return engine_ ? engine_->camera().getParams().viewCorrection.x : 0.0;
}
double CesiumBridgeAndroid::readViewCorrectionY() {
  return engine_ ? engine_->camera().getParams().viewCorrection.y : 0.0;
}
double CesiumBridgeAndroid::readViewCorrectionZ() {
  return engine_ ? engine_->camera().getParams().viewCorrection.z : 0.0;
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
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeUpdateCameraQuaternion(
    JNIEnv*, jobject, jlong ptr, jdouble lat, jdouble lon, jdouble alt,
    jdouble heading, jdouble pitch, jdouble roll,
    jdouble qw, jdouble qx, jdouble qy, jdouble qz) {
  getBridge(ptr)->updateCameraQuaternion(lat, lon, alt, heading, pitch, roll, qw, qx, qy, qz);
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
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetViewCorrectionW(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->readViewCorrectionW();
}
JNIEXPORT jdouble JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetViewCorrectionX(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->readViewCorrectionX();
}
JNIEXPORT jdouble JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetViewCorrectionY(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->readViewCorrectionY();
}
JNIEXPORT jdouble JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetViewCorrectionZ(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->readViewCorrectionZ();
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

JNIEXPORT jboolean JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetMetricsTlsConfigured(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->metricsTlsConfigured() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetMetricsCreditsPlainText(JNIEnv* env, jobject, jlong ptr) {
  return env->NewStringUTF(getBridge(ptr)->metricsCreditsPlainText().c_str());
}

JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeSetMaximumCachedMiB(JNIEnv*, jobject, jlong ptr, jint v) {
  getBridge(ptr)->setMaximumCachedMiB(v);
}
JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeSetPreloadAncestors(JNIEnv*, jobject, jlong ptr, jboolean v) {
  getBridge(ptr)->setPreloadAncestors(v == JNI_TRUE);
}
JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeSetPreloadSiblings(JNIEnv*, jobject, jlong ptr, jboolean v) {
  getBridge(ptr)->setPreloadSiblings(v == JNI_TRUE);
}
JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeSetForbidHoles(JNIEnv*, jobject, jlong ptr, jboolean v) {
  getBridge(ptr)->setForbidHoles(v == JNI_TRUE);
}
JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeSetEnableWaterMask(JNIEnv*, jobject, jlong ptr, jboolean v) {
  getBridge(ptr)->setEnableWaterMask(v == JNI_TRUE);
}
JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeSetEnableFogCulling(JNIEnv*, jobject, jlong ptr, jboolean v) {
  getBridge(ptr)->setEnableFogCulling(v == JNI_TRUE);
}
JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeSetEnforceCulledScreenSpaceError(JNIEnv*, jobject, jlong ptr, jboolean v) {
  getBridge(ptr)->setEnforceCulledScreenSpaceError(v == JNI_TRUE);
}
JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeSetCulledScreenSpaceError(JNIEnv*, jobject, jlong ptr, jdouble v) {
  getBridge(ptr)->setCulledScreenSpaceError(v);
}
JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeSetEnableLodTransitionPeriod(JNIEnv*, jobject, jlong ptr, jboolean v) {
  getBridge(ptr)->setEnableLodTransitionPeriod(v == JNI_TRUE);
}
JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeSetLodTransitionLength(JNIEnv*, jobject, jlong ptr, jdouble v) {
  getBridge(ptr)->setLodTransitionLength(v);
}
JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeSetSqliteCacheMaxRows(JNIEnv*, jobject, jlong ptr, jint v) {
  getBridge(ptr)->setSqliteCacheMaxRows(v);
}
JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeSetTaskProcessorThreads(JNIEnv*, jobject, jlong ptr, jint v) {
  getBridge(ptr)->setTaskProcessorThreads(v);
}
JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeSetMinAltitudeAboveTerrain(JNIEnv*, jobject, jlong ptr, jfloat v) {
  getBridge(ptr)->setMinAltitudeAboveTerrain(v);
}

} // extern "C"
