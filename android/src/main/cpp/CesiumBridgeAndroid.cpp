#include "CesiumBridgeAndroid.h"

#include "engine/EngineTunables.hpp"
#include "engine/GeoidConverter.hpp"
#include "vulkan/VulkanBackend.h"

#include <android/native_window_jni.h>
#include <android/log.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

#define LOG_TAG "CesiumBridge"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

CesiumBridgeAndroid::~CesiumBridgeAndroid() {
  // shutdown() is idempotent — calling it from the dtor in addition to the
  // explicit nativeShutdown JNI path is safe and guarantees the
  // ANativeWindow ref is released even if the lifecycle hooks misfire.
  shutdown();
}

void CesiumBridgeAndroid::init(JNIEnv* env, jobject surface, int width, int height,
                               const std::string& cacheDir,
                               const std::string& cacertPath,
                               const std::string& ionAccessToken,
                               int64_t ionAssetId) {
  config_.cacheDatabasePath = cacheDir.empty() ? "" : cacheDir + "/cesium_cache.db";
  config_.tlsCaBundlePath   = cacertPath;
  viewportWidth_  = width;
  viewportHeight_ = height;

  // Seed the ion credentials before buildEngine so createTileset() can
  // immediately start the async tileset.json HTTP round-trip, letting it
  // overlap with Vulkan PSO compilation rather than run serially after it.
  if (!ionAccessToken.empty()) {
    config_.ionAccessToken = ionAccessToken;
    config_.ionAssetId     = ionAssetId;
  }

  // ANativeWindow_fromSurface() acquires one strong reference on the
  // returned window; we own it now and must call ANativeWindow_release() in
  // shutdown() (or before re-acquiring) to avoid leaking it on every view
  // remount.
  nativeWindow_ = ANativeWindow_fromSurface(env, surface);
  if (!nativeWindow_) {
    LOGE("Failed to get ANativeWindow from Surface");
    return;
  }

  vulkanBackend_ = std::make_unique<reactnativecesium::VulkanBackend>();
  vulkanBackend_->setCacheDir(cacheDir);
  vulkanBackend_->initialize(nativeWindow_, width, height);

  frameResult_ = std::make_unique<reactnativecesium::FrameResult>();
  buildEngine();
  initialized_ = true;
  forceRender_.store(true, std::memory_order_release);
}

void CesiumBridgeAndroid::buildEngine() {
  engine_ = std::make_unique<reactnativecesium::CesiumEngine>();
  engine_->initialize(config_);

  // Seed the integrator from the engine's initial camera so the first frame
  // is not a "snap from default" jump.
  integrator_ = std::make_unique<reactnativecesium::CameraIntegrator>(
      engine_->camera().getParams());

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
  forceRender_.store(true, std::memory_order_release);
}

void CesiumBridgeAndroid::shutdown() {
  // Mark uninitialised up-front so any racing prop setters become no-ops
  // instead of touching half-destroyed state.
  initialized_ = false;

  if (engine_) engine_->shutdown();
  if (vulkanBackend_) vulkanBackend_->shutdown();

  // Release the engine (and therefore its SqliteCache) here rather than
  // waiting for ~CesiumBridgeAndroid. During fast refresh the next
  // CesiumView can mount and open the same cesium_cache.db file before
  // this JNI peer is destroyed; freeing the unique_ptr now closes the
  // SQLite handle synchronously and avoids "database is locked"
  // warnings from the new engine's SqliteCache.
  integrator_.reset();
  engine_.reset();
  vulkanBackend_.reset();
  frameResult_.reset();

  // Release the ANativeWindow ref acquired in init(). Order matters: the
  // Vulkan backend's vkDestroySurfaceKHR must have run first (which it has,
  // inside vulkanBackend_->shutdown() above) before we drop our reference.
  if (nativeWindow_) {
    ANativeWindow_release(nativeWindow_);
    nativeWindow_ = nullptr;
  }
}

void CesiumBridgeAndroid::resize(int width, int height) {
  viewportWidth_  = width;
  viewportHeight_ = height;
  if (vulkanBackend_) vulkanBackend_->resize(width, height);
  forceRender_.store(true, std::memory_order_release);
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
    forceRender_.store(true, std::memory_order_release);
  }
}

// ── Per-DoF camera demand setters ─────────────────────────────────────────

void CesiumBridgeAndroid::setPosition(double lat, double lon) {
  if (!initialized_ || !integrator_) return;
  integrator_->setPosition(lat, lon);
  forceRender_.store(true, std::memory_order_release);
}

void CesiumBridgeAndroid::setAltitude(double alt) {
  if (!initialized_ || !integrator_) return;
  integrator_->setAltitude(alt);
  forceRender_.store(true, std::memory_order_release);
}

void CesiumBridgeAndroid::setHeading(double headingDeg) {
  if (!initialized_ || !integrator_) return;
  integrator_->setHeading(headingDeg);
  forceRender_.store(true, std::memory_order_release);
}

void CesiumBridgeAndroid::setAttitude(double pitchDeg, double rollDeg) {
  if (!initialized_ || !integrator_) return;
  integrator_->setAttitude(pitchDeg, rollDeg);
  forceRender_.store(true, std::memory_order_release);
}

void CesiumBridgeAndroid::setViewCorrection(double qw, double qx, double qy, double qz) {
  if (!initialized_ || !integrator_) return;
  integrator_->setViewCorrection(glm::dquat(qw, qx, qy, qz));
  forceRender_.store(true, std::memory_order_release);
}

void CesiumBridgeAndroid::teleport(double lat, double lon, double alt,
                                   double heading, double pitch, double roll,
                                   double vfov) {
  if (!initialized_ || !integrator_) return;
  reactnativecesium::CameraParams p;
  p.latitude       = lat;
  p.longitude      = lon;
  p.altitude       = alt;
  p.heading        = heading;
  p.pitch          = pitch;
  p.roll           = roll;
  p.verticalFov    = vfov;
  p.viewCorrection = integrator_->getActual().viewCorrection;
  integrator_->teleport(p);
  engine_->camera().setParams(p);
  forceRender_.store(true, std::memory_order_release);
}

void CesiumBridgeAndroid::setVerticalFovDeg(double degrees) {
  if (!initialized_ || !integrator_) return;
  integrator_->setVerticalFov(degrees);
  forceRender_.store(true, std::memory_order_release);
}

void CesiumBridgeAndroid::setRateCaps(double yawDegSec, double pitchDegSec,
                                      double rollDegSec, double climbMps,
                                      double groundMps) {
  if (!initialized_ || !integrator_) return;
  reactnativecesium::CameraRateCaps caps;
  caps.maxYawRateDegSec   = yawDegSec;
  caps.maxPitchRateDegSec = pitchDegSec;
  caps.maxRollRateDegSec  = rollDegSec;
  caps.maxClimbRateMps    = climbMps;
  caps.maxGroundSpeedMps  = groundMps;
  integrator_->setRateCaps(caps);
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
  forceRender_.store(true, std::memory_order_release);
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
  config_.sqliteCacheMaxRows = std::max(64, v);
}
void CesiumBridgeAndroid::setTaskProcessorThreads(int32_t v) {
  config_.taskProcessorThreads = std::max(0, v);
}
void CesiumBridgeAndroid::setMinAltitudeAboveTerrain(float v) {
  config_.minAltitudeAboveTerrain = std::max(0.0f, v);
  if (initialized_) applyEngineConfig();
}

void CesiumBridgeAndroid::markNeedsRender() {
  forceRender_.store(true, std::memory_order_release);
}

bool CesiumBridgeAndroid::shouldRenderNextFrame() {
  if (!initialized_ || !engine_ || !frameResult_) return false;
  if (forceRender_.load(std::memory_order_acquire))            return true;
  if (frameResult_->tilesLoading > 0 || !frameResult_->tilesetActive) return true;
  // Keep rendering at full speed until we have actual terrain tiles loaded.
  // This ensures tiles start loading immediately instead of falling into idle
  // mode (250ms intervals) before any content appears.
  if (frameResult_->tilesRendered < 5)                         return true;
  if (integrator_ && integrator_->isActive())                  return true;
  return false;
}

void CesiumBridgeAndroid::renderFrame(double nowSeconds) {
  if (!initialized_ || !integrator_) return;
  if (!engine_ || !vulkanBackend_ || !frameResult_) return;

  forceRender_.store(false, std::memory_order_release);

  // The integrator owns its own clock — see CameraIntegrator::step().
  // `nowSeconds` is still used below for the metrics-only dt EMA.
  auto actual = integrator_->step();

  // ── Terrain floor clamp ───────────────────────────────────────────────
  const float minAbove = engine_->getConfig().minAltitudeAboveTerrain;
  if (minAbove > 0.0f) {
    const double geoidOffset =
        reactnativecesium::mslToEllipsoidMeters(actual.latitude, actual.longitude, 0.0);
    const double camEllipsoid =
        reactnativecesium::mslToEllipsoidMeters(actual.latitude, actual.longitude, actual.altitude);
    const double minEllipsoid =
        static_cast<double>(engine_->terrainFloorEllipsoidMeters()) + minAbove;
    if (camEllipsoid < minEllipsoid) {
      const double clampedMsl = minEllipsoid - geoidOffset;
      integrator_->clampActualAltitude(clampedMsl);
      actual.altitude = clampedMsl;
    }
  }

  engine_->camera().setParams(actual);

  engine_->updateFrame(viewportWidth_, viewportHeight_, *frameResult_);
  // Metrics-only dt; integrator already advanced by wall clock above.
  double dt = (lastTickSeconds_ > 0.0) ? (nowSeconds - lastTickSeconds_) : (1.0 / 60.0);
  lastTickSeconds_ = nowSeconds;
  if (dt > 0.5 || dt <= 0.0) dt = 1.0 / 60.0;
  metrics_.tick(dt, *frameResult_, !config_.tlsCaBundlePath.empty());

  reactnativecesium::FrameParams params;
  vulkanBackend_->beginFrame(params);
  vulkanBackend_->drawScene(*frameResult_);
  vulkanBackend_->endFrame();
}

// ── Actual camera readback ─────────────────────────────────────────────────

double CesiumBridgeAndroid::readActualLatitude() {
  return integrator_ ? integrator_->getActual().latitude : 0.0;
}
double CesiumBridgeAndroid::readActualLongitude() {
  return integrator_ ? integrator_->getActual().longitude : 0.0;
}
double CesiumBridgeAndroid::readActualAltitude() {
  return integrator_ ? integrator_->getActual().altitude : 0.0;
}
double CesiumBridgeAndroid::readActualHeading() {
  return integrator_ ? integrator_->getActual().heading : 0.0;
}
double CesiumBridgeAndroid::readActualPitch() {
  return integrator_ ? integrator_->getActual().pitch : 0.0;
}
double CesiumBridgeAndroid::readActualRoll() {
  return integrator_ ? integrator_->getActual().roll : 0.0;
}
double CesiumBridgeAndroid::readActualVerticalFovDeg() {
  return integrator_ ? integrator_->getActual().verticalFov : 60.0;
}

// ── Demand camera readback ─────────────────────────────────────────────────

double CesiumBridgeAndroid::readDemandLatitude() {
  return integrator_ ? integrator_->getDemand().latitude : 0.0;
}
double CesiumBridgeAndroid::readDemandLongitude() {
  return integrator_ ? integrator_->getDemand().longitude : 0.0;
}
double CesiumBridgeAndroid::readDemandAltitude() {
  return integrator_ ? integrator_->getDemand().altitude : 0.0;
}
double CesiumBridgeAndroid::readDemandHeading() {
  return integrator_ ? integrator_->getDemand().heading : 0.0;
}
double CesiumBridgeAndroid::readDemandPitch() {
  return integrator_ ? integrator_->getDemand().pitch : 0.0;
}
double CesiumBridgeAndroid::readDemandRoll() {
  return integrator_ ? integrator_->getDemand().roll : 0.0;
}
double CesiumBridgeAndroid::readDemandVerticalFovDeg() {
  return integrator_ ? integrator_->getDemand().verticalFov : 60.0;
}

double CesiumBridgeAndroid::readViewCorrectionW() {
  return integrator_ ? integrator_->getActual().viewCorrection.w : 1.0;
}
double CesiumBridgeAndroid::readViewCorrectionX() {
  return integrator_ ? integrator_->getActual().viewCorrection.x : 0.0;
}
double CesiumBridgeAndroid::readViewCorrectionY() {
  return integrator_ ? integrator_->getActual().viewCorrection.y : 0.0;
}
double CesiumBridgeAndroid::readViewCorrectionZ() {
  return integrator_ ? integrator_->getActual().viewCorrection.z : 0.0;
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
    jstring cacheDir, jstring cacertPath, jstring ionAccessToken, jlong ionAssetId) {
  const char* cacheDirC = env->GetStringUTFChars(cacheDir, nullptr);
  const char* cacertC   = env->GetStringUTFChars(cacertPath, nullptr);
  const char* tokenC    = env->GetStringUTFChars(ionAccessToken, nullptr);
  getBridge(ptr)->init(env, surface, w, h, cacheDirC, cacertC,
                       tokenC ? tokenC : "", static_cast<int64_t>(ionAssetId));
  env->ReleaseStringUTFChars(cacheDir, cacheDirC);
  env->ReleaseStringUTFChars(cacertPath, cacertC);
  env->ReleaseStringUTFChars(ionAccessToken, tokenC);
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

// ── Per-DoF camera setters ─────────────────────────────────────────────────
JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeSetPosition(
    JNIEnv*, jobject, jlong ptr, jdouble lat, jdouble lon) {
  getBridge(ptr)->setPosition(lat, lon);
}
JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeSetAltitude(
    JNIEnv*, jobject, jlong ptr, jdouble alt) {
  getBridge(ptr)->setAltitude(alt);
}
JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeSetHeading(
    JNIEnv*, jobject, jlong ptr, jdouble headingDeg) {
  getBridge(ptr)->setHeading(headingDeg);
}
JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeSetAttitude(
    JNIEnv*, jobject, jlong ptr, jdouble pitchDeg, jdouble rollDeg) {
  getBridge(ptr)->setAttitude(pitchDeg, rollDeg);
}
JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeSetViewCorrection(
    JNIEnv*, jobject, jlong ptr, jdouble qw, jdouble qx, jdouble qy, jdouble qz) {
  getBridge(ptr)->setViewCorrection(qw, qx, qy, qz);
}
JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeTeleport(
    JNIEnv*, jobject, jlong ptr,
    jdouble lat, jdouble lon, jdouble alt,
    jdouble heading, jdouble pitch, jdouble roll, jdouble vfov) {
  getBridge(ptr)->teleport(lat, lon, alt, heading, pitch, roll, vfov);
}

JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeSetVerticalFovDeg(
    JNIEnv*, jobject, jlong ptr, jdouble deg) {
  getBridge(ptr)->setVerticalFovDeg(deg);
}

JNIEXPORT void JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeSetRateCaps(
    JNIEnv*, jobject, jlong ptr,
    jdouble yawDegSec, jdouble pitchDegSec, jdouble rollDegSec,
    jdouble climbMps, jdouble groundMps) {
  getBridge(ptr)->setRateCaps(yawDegSec, pitchDegSec, rollDegSec,
                              climbMps, groundMps);
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
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeRenderFrame(JNIEnv*, jobject, jlong ptr, jdouble nowSeconds) {
  getBridge(ptr)->renderFrame(nowSeconds);
}

// ── Actual / demand readbacks ──────────────────────────────────────────────
JNIEXPORT jdouble JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetActualLatitude(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->readActualLatitude();
}
JNIEXPORT jdouble JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetActualLongitude(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->readActualLongitude();
}
JNIEXPORT jdouble JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetActualAltitude(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->readActualAltitude();
}
JNIEXPORT jdouble JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetActualHeading(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->readActualHeading();
}
JNIEXPORT jdouble JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetActualPitch(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->readActualPitch();
}
JNIEXPORT jdouble JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetActualRoll(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->readActualRoll();
}
JNIEXPORT jdouble JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetActualVerticalFovDeg(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->readActualVerticalFovDeg();
}

JNIEXPORT jdouble JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetDemandLatitude(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->readDemandLatitude();
}
JNIEXPORT jdouble JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetDemandLongitude(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->readDemandLongitude();
}
JNIEXPORT jdouble JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetDemandAltitude(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->readDemandAltitude();
}
JNIEXPORT jdouble JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetDemandHeading(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->readDemandHeading();
}
JNIEXPORT jdouble JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetDemandPitch(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->readDemandPitch();
}
JNIEXPORT jdouble JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetDemandRoll(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->readDemandRoll();
}
JNIEXPORT jdouble JNICALL
Java_com_margelo_nitro_reactnativecesium_CesiumBridgeJNI_nativeGetDemandVerticalFovDeg(JNIEnv*, jobject, jlong ptr) {
  return getBridge(ptr)->readDemandVerticalFovDeg();
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
