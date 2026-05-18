package com.margelo.nitro.reactnativecesium

import android.view.Surface

/**
 * JNI bridge to the C++ CesiumBridgeAndroid. Each instance owns a native pointer
 * managed via [nativeCreate] / [nativeDestroy].
 */
class CesiumBridgeJNI {
  private var nativePtr: Long = nativeCreate()

  fun init(surface: Surface, width: Int, height: Int, cacheDir: String, cacertPath: String,
           ionAccessToken: String = "", ionAssetId: Long = 1L) {
    nativeInit(nativePtr, surface, width, height, cacheDir, cacertPath, ionAccessToken, ionAssetId)
  }

  fun shutdown() = nativeShutdown(nativePtr)

  fun destroy() {
    nativeDestroy(nativePtr)
    nativePtr = 0
  }

  fun resize(w: Int, h: Int) = nativeResize(nativePtr, w, h)
  fun updateIonAccessToken(token: String, assetId: Long) = nativeUpdateIonAccessToken(nativePtr, token, assetId)
  fun updateImageryAssetId(assetId: Long) = nativeUpdateImageryAssetId(nativePtr, assetId)

  // ── Per-DoF camera setters ───────────────────────────────────────────────
  fun setPosition(lat: Double, lon: Double) = nativeSetPosition(nativePtr, lat, lon)
  fun setAltitude(alt: Double) = nativeSetAltitude(nativePtr, alt)
  fun setHeading(headingDeg: Double) = nativeSetHeading(nativePtr, headingDeg)
  fun setAttitude(pitchDeg: Double, rollDeg: Double) = nativeSetAttitude(nativePtr, pitchDeg, rollDeg)
  fun setViewCorrection(qw: Double, qx: Double, qy: Double, qz: Double) =
    nativeSetViewCorrection(nativePtr, qw, qx, qy, qz)
  fun teleport(
    lat: Double, lon: Double, alt: Double,
    heading: Double, pitch: Double, roll: Double, vfov: Double,
  ) = nativeTeleport(nativePtr, lat, lon, alt, heading, pitch, roll, vfov)

  fun setVerticalFovDeg(deg: Double) = nativeSetVerticalFovDeg(nativePtr, deg)
  fun setRateCaps(
    yawDegSec: Double, pitchDegSec: Double, rollDegSec: Double,
    climbMps: Double, groundMps: Double,
  ) = nativeSetRateCaps(nativePtr, yawDegSec, pitchDegSec, rollDegSec, climbMps, groundMps)

  fun setMaxSSE(v: Double) = nativeSetMaxSSE(nativePtr, v)
  fun setMaxSimLoads(v: Int) = nativeSetMaxSimLoads(nativePtr, v)
  fun setLoadDescLim(v: Int) = nativeSetLoadDescLim(nativePtr, v)
  fun setMsaa(v: Int) = nativeSetMsaa(nativePtr, v)

  fun markNeedsRender() = nativeMarkNeedsRender(nativePtr)
  fun shouldRenderNextFrame(): Boolean = nativeShouldRenderNextFrame(nativePtr)
  fun renderFrame(nowSeconds: Double) = nativeRenderFrame(nativePtr, nowSeconds)

  // ── Actual camera readback ───────────────────────────────────────────────
  fun getActualLatitude(): Double = nativeGetActualLatitude(nativePtr)
  fun getActualLongitude(): Double = nativeGetActualLongitude(nativePtr)
  fun getActualAltitude(): Double = nativeGetActualAltitude(nativePtr)
  fun getActualHeading(): Double = nativeGetActualHeading(nativePtr)
  fun getActualPitch(): Double = nativeGetActualPitch(nativePtr)
  fun getActualRoll(): Double = nativeGetActualRoll(nativePtr)
  fun getActualVerticalFovDeg(): Double = nativeGetActualVerticalFovDeg(nativePtr)

  // ── Demand camera readback ───────────────────────────────────────────────
  fun getDemandLatitude(): Double = nativeGetDemandLatitude(nativePtr)
  fun getDemandLongitude(): Double = nativeGetDemandLongitude(nativePtr)
  fun getDemandAltitude(): Double = nativeGetDemandAltitude(nativePtr)
  fun getDemandHeading(): Double = nativeGetDemandHeading(nativePtr)
  fun getDemandPitch(): Double = nativeGetDemandPitch(nativePtr)
  fun getDemandRoll(): Double = nativeGetDemandRoll(nativePtr)
  fun getDemandVerticalFovDeg(): Double = nativeGetDemandVerticalFovDeg(nativePtr)

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
  fun getMetricsTlsConfigured(): Boolean = nativeGetMetricsTlsConfigured(nativePtr)
  fun getMetricsCreditsPlainText(): String = nativeGetMetricsCreditsPlainText(nativePtr)

  fun setMaximumCachedMiB(v: Int) = nativeSetMaximumCachedMiB(nativePtr, v)
  fun setPreloadAncestors(v: Boolean) = nativeSetPreloadAncestors(nativePtr, v)
  fun setPreloadSiblings(v: Boolean) = nativeSetPreloadSiblings(nativePtr, v)
  fun setForbidHoles(v: Boolean) = nativeSetForbidHoles(nativePtr, v)
  fun setEnableWaterMask(v: Boolean) = nativeSetEnableWaterMask(nativePtr, v)
  fun setEnableFogCulling(v: Boolean) = nativeSetEnableFogCulling(nativePtr, v)
  fun setEnforceCulledScreenSpaceError(v: Boolean) = nativeSetEnforceCulledScreenSpaceError(nativePtr, v)
  fun setCulledScreenSpaceError(v: Double) = nativeSetCulledScreenSpaceError(nativePtr, v)
  fun setEnableLodTransitionPeriod(v: Boolean) = nativeSetEnableLodTransitionPeriod(nativePtr, v)
  fun setLodTransitionLength(v: Double) = nativeSetLodTransitionLength(nativePtr, v)
  fun setSqliteCacheMaxRows(v: Int) = nativeSetSqliteCacheMaxRows(nativePtr, v)
  fun setTaskProcessorThreads(v: Int) = nativeSetTaskProcessorThreads(nativePtr, v)
  fun setMinAltitudeAboveTerrain(v: Float) = nativeSetMinAltitudeAboveTerrain(nativePtr, v)

  private external fun nativeCreate(): Long
  private external fun nativeInit(ptr: Long, surface: Surface, w: Int, h: Int, cacheDir: String, cacertPath: String, ionAccessToken: String, ionAssetId: Long)
  private external fun nativeShutdown(ptr: Long)
  private external fun nativeDestroy(ptr: Long)
  private external fun nativeResize(ptr: Long, w: Int, h: Int)
  private external fun nativeUpdateIonAccessToken(ptr: Long, token: String, assetId: Long)
  private external fun nativeUpdateImageryAssetId(ptr: Long, assetId: Long)

  private external fun nativeSetPosition(ptr: Long, lat: Double, lon: Double)
  private external fun nativeSetAltitude(ptr: Long, alt: Double)
  private external fun nativeSetHeading(ptr: Long, headingDeg: Double)
  private external fun nativeSetAttitude(ptr: Long, pitchDeg: Double, rollDeg: Double)
  private external fun nativeSetViewCorrection(ptr: Long, qw: Double, qx: Double, qy: Double, qz: Double)
  private external fun nativeTeleport(
    ptr: Long, lat: Double, lon: Double, alt: Double,
    heading: Double, pitch: Double, roll: Double, vfov: Double,
  )

  private external fun nativeSetVerticalFovDeg(ptr: Long, deg: Double)
  private external fun nativeSetRateCaps(
    ptr: Long, yawDegSec: Double, pitchDegSec: Double, rollDegSec: Double,
    climbMps: Double, groundMps: Double,
  )
  private external fun nativeSetMaxSSE(ptr: Long, v: Double)
  private external fun nativeSetMaxSimLoads(ptr: Long, v: Int)
  private external fun nativeSetLoadDescLim(ptr: Long, v: Int)
  private external fun nativeSetMsaa(ptr: Long, v: Int)
  private external fun nativeMarkNeedsRender(ptr: Long)
  private external fun nativeShouldRenderNextFrame(ptr: Long): Boolean
  private external fun nativeRenderFrame(ptr: Long, nowSeconds: Double)

  private external fun nativeGetActualLatitude(ptr: Long): Double
  private external fun nativeGetActualLongitude(ptr: Long): Double
  private external fun nativeGetActualAltitude(ptr: Long): Double
  private external fun nativeGetActualHeading(ptr: Long): Double
  private external fun nativeGetActualPitch(ptr: Long): Double
  private external fun nativeGetActualRoll(ptr: Long): Double
  private external fun nativeGetActualVerticalFovDeg(ptr: Long): Double

  private external fun nativeGetDemandLatitude(ptr: Long): Double
  private external fun nativeGetDemandLongitude(ptr: Long): Double
  private external fun nativeGetDemandAltitude(ptr: Long): Double
  private external fun nativeGetDemandHeading(ptr: Long): Double
  private external fun nativeGetDemandPitch(ptr: Long): Double
  private external fun nativeGetDemandRoll(ptr: Long): Double
  private external fun nativeGetDemandVerticalFovDeg(ptr: Long): Double

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
  private external fun nativeGetMetricsTlsConfigured(ptr: Long): Boolean
  private external fun nativeGetMetricsCreditsPlainText(ptr: Long): String

  private external fun nativeSetMaximumCachedMiB(ptr: Long, v: Int)
  private external fun nativeSetPreloadAncestors(ptr: Long, v: Boolean)
  private external fun nativeSetPreloadSiblings(ptr: Long, v: Boolean)
  private external fun nativeSetForbidHoles(ptr: Long, v: Boolean)
  private external fun nativeSetEnableWaterMask(ptr: Long, v: Boolean)
  private external fun nativeSetEnableFogCulling(ptr: Long, v: Boolean)
  private external fun nativeSetEnforceCulledScreenSpaceError(ptr: Long, v: Boolean)
  private external fun nativeSetCulledScreenSpaceError(ptr: Long, v: Double)
  private external fun nativeSetEnableLodTransitionPeriod(ptr: Long, v: Boolean)
  private external fun nativeSetLodTransitionLength(ptr: Long, v: Double)
  private external fun nativeSetSqliteCacheMaxRows(ptr: Long, v: Int)
  private external fun nativeSetTaskProcessorThreads(ptr: Long, v: Int)
  private external fun nativeSetMinAltitudeAboveTerrain(ptr: Long, v: Float)
}
