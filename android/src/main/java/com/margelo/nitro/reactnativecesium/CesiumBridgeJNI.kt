package com.margelo.nitro.reactnativecesium

import android.view.Surface

/**
 * JNI bridge to the C++ CesiumBridgeAndroid. Each instance owns a native pointer
 * managed via [nativeCreate] / [nativeDestroy].
 */
class CesiumBridgeJNI {
  private var nativePtr: Long = nativeCreate()

  fun init(surface: Surface, width: Int, height: Int, cacheDir: String, cacertPath: String) {
    nativeInit(nativePtr, surface, width, height, cacheDir, cacertPath)
  }

  fun shutdown() = nativeShutdown(nativePtr)

  fun destroy() {
    nativeDestroy(nativePtr)
    nativePtr = 0
  }

  fun resize(w: Int, h: Int) = nativeResize(nativePtr, w, h)
  fun updateIonAccessToken(token: String, assetId: Long) = nativeUpdateIonAccessToken(nativePtr, token, assetId)
  fun updateImageryAssetId(assetId: Long) = nativeUpdateImageryAssetId(nativePtr, assetId)
  fun updateCamera(lat: Double, lon: Double, alt: Double, heading: Double, pitch: Double, roll: Double) =
    nativeUpdateCamera(nativePtr, lat, lon, alt, heading, pitch, roll)

  fun updateCameraQuaternion(
    lat: Double,
    lon: Double,
    alt: Double,
    heading: Double,
    pitch: Double,
    roll: Double,
    qw: Double,
    qx: Double,
    qy: Double,
    qz: Double,
  ) = nativeUpdateCameraQuaternion(nativePtr, lat, lon, alt, heading, pitch, roll, qw, qx, qy, qz)
  fun setVerticalFovDeg(deg: Double) = nativeSetVerticalFovDeg(nativePtr, deg)
  fun setMaxSSE(v: Double) = nativeSetMaxSSE(nativePtr, v)
  fun setMaxSimLoads(v: Int) = nativeSetMaxSimLoads(nativePtr, v)
  fun setLoadDescLim(v: Int) = nativeSetLoadDescLim(nativePtr, v)
  fun setMsaa(v: Int) = nativeSetMsaa(nativePtr, v)

  fun markNeedsRender() = nativeMarkNeedsRender(nativePtr)
  fun shouldRenderNextFrame(): Boolean = nativeShouldRenderNextFrame(nativePtr)
  fun renderFrame(dt: Double) = nativeRenderFrame(nativePtr, dt)

  fun getCameraLat(): Double = nativeGetCameraLat(nativePtr)
  fun getCameraLon(): Double = nativeGetCameraLon(nativePtr)
  fun getCameraAlt(): Double = nativeGetCameraAlt(nativePtr)
  fun getCameraHeading(): Double = nativeGetCameraHeading(nativePtr)
  fun getCameraPitch(): Double = nativeGetCameraPitch(nativePtr)
  fun getCameraRoll(): Double = nativeGetCameraRoll(nativePtr)
  fun getVerticalFovDeg(): Double = nativeGetVerticalFovDeg(nativePtr)

  fun getViewCorrectionW(): Double = nativeGetViewCorrectionW(nativePtr)
  fun getViewCorrectionX(): Double = nativeGetViewCorrectionX(nativePtr)
  fun getViewCorrectionY(): Double = nativeGetViewCorrectionY(nativePtr)
  fun getViewCorrectionZ(): Double = nativeGetViewCorrectionZ(nativePtr)

  fun getMetricsFps(): Double = nativeGetMetricsFps(nativePtr)
  fun getMetricsTilesRendered(): Int = nativeGetMetricsTilesRendered(nativePtr)
  fun getMetricsTilesLoading(): Int = nativeGetMetricsTilesLoading(nativePtr)
  fun getMetricsTilesVisited(): Int = nativeGetMetricsTilesVisited(nativePtr)
  fun getMetricsIonTokenConfigured(): Boolean = nativeGetMetricsIonTokenConfigured(nativePtr)
  fun getMetricsTilesetReady(): Boolean = nativeGetMetricsTilesetReady(nativePtr)
  fun getMetricsCreditsPlainText(): String = nativeGetMetricsCreditsPlainText(nativePtr)

  private external fun nativeCreate(): Long
  private external fun nativeInit(ptr: Long, surface: Surface, w: Int, h: Int, cacheDir: String, cacertPath: String)
  private external fun nativeShutdown(ptr: Long)
  private external fun nativeDestroy(ptr: Long)
  private external fun nativeResize(ptr: Long, w: Int, h: Int)
  private external fun nativeUpdateIonAccessToken(ptr: Long, token: String, assetId: Long)
  private external fun nativeUpdateImageryAssetId(ptr: Long, assetId: Long)
  private external fun nativeUpdateCamera(ptr: Long, lat: Double, lon: Double, alt: Double, heading: Double, pitch: Double, roll: Double)
  private external fun nativeUpdateCameraQuaternion(
    ptr: Long,
    lat: Double,
    lon: Double,
    alt: Double,
    heading: Double,
    pitch: Double,
    roll: Double,
    qw: Double,
    qx: Double,
    qy: Double,
    qz: Double,
  )
  private external fun nativeSetVerticalFovDeg(ptr: Long, deg: Double)
  private external fun nativeSetMaxSSE(ptr: Long, v: Double)
  private external fun nativeSetMaxSimLoads(ptr: Long, v: Int)
  private external fun nativeSetLoadDescLim(ptr: Long, v: Int)
  private external fun nativeSetMsaa(ptr: Long, v: Int)
  private external fun nativeMarkNeedsRender(ptr: Long)
  private external fun nativeShouldRenderNextFrame(ptr: Long): Boolean
  private external fun nativeRenderFrame(ptr: Long, dt: Double)
  private external fun nativeGetCameraLat(ptr: Long): Double
  private external fun nativeGetCameraLon(ptr: Long): Double
  private external fun nativeGetCameraAlt(ptr: Long): Double
  private external fun nativeGetCameraHeading(ptr: Long): Double
  private external fun nativeGetCameraPitch(ptr: Long): Double
  private external fun nativeGetCameraRoll(ptr: Long): Double
  private external fun nativeGetVerticalFovDeg(ptr: Long): Double
  private external fun nativeGetViewCorrectionW(ptr: Long): Double
  private external fun nativeGetViewCorrectionX(ptr: Long): Double
  private external fun nativeGetViewCorrectionY(ptr: Long): Double
  private external fun nativeGetViewCorrectionZ(ptr: Long): Double
  private external fun nativeGetMetricsFps(ptr: Long): Double
  private external fun nativeGetMetricsTilesRendered(ptr: Long): Int
  private external fun nativeGetMetricsTilesLoading(ptr: Long): Int
  private external fun nativeGetMetricsTilesVisited(ptr: Long): Int
  private external fun nativeGetMetricsIonTokenConfigured(ptr: Long): Boolean
  private external fun nativeGetMetricsTilesetReady(ptr: Long): Boolean
  private external fun nativeGetMetricsCreditsPlainText(ptr: Long): String
}
