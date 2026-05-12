package com.margelo.nitro.reactnativecesium

import android.content.Context
import android.os.Handler
import android.os.HandlerThread
import android.os.Process
import android.os.SystemClock
import android.util.Log
import android.view.Choreographer
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import com.margelo.nitro.core.Promise
import java.io.File
import java.io.FileOutputStream
import java.util.concurrent.atomic.AtomicBoolean

class HybridCesiumView(private val appContext: Context) : HybridCesiumViewSpec() {
  private val surfaceView = SurfaceView(appContext)

  /// Bridge is only ever read on the render thread. The UI thread posts
  /// requests via `renderHandler`. Marked volatile so observers on the UI
  /// thread (`bridge != null`) see the latest reference; real work is always
  /// serialized through the render thread for config-style writes, and
  /// goes directly into CameraIntegrator (with its own mutex) for camera
  /// demand writes.
  @Volatile private var bridge: CesiumBridgeJNI? = null
  private var metricsFrameCounter = 0
  private var idleProbeAccumulator = 0.0
  private var renderLoopActive = false
  private val renderInFlight = AtomicBoolean(false)
  // Last System.nanoTime() when tilesLoading was > 0. Used to keep the render
  // loop at full speed for a grace period after loading finishes — prevents the
  // cascade of idle gaps between LOD refinement phases. 0 = not yet seen.
  private var lastLoadingTimeNs: Long = 0L

  /// Set in `surfaceDestroyed` before any teardown work begins. Render-thread
  /// callbacks and choreographer frame ticks check this flag and bail out so
  /// they don't try to emit `onMetrics` into a JS dispatcher that may already
  /// have been torn down by fast refresh / view unmount. Volatile so the
  /// render thread sees the write made on the UI thread.
  @Volatile private var isShutdown = false

  // Demand values that arrived before the bridge was created. Drained once
  // in surfaceCreated after the seed teleport.
  @Volatile private var pendingTeleport: CameraState? = null
  @Volatile private var pendingLat: Double? = null
  @Volatile private var pendingLon: Double? = null
  @Volatile private var pendingAltitude: Double? = null
  @Volatile private var pendingHeading: Double? = null
  @Volatile private var pendingPitch: Double? = null
  @Volatile private var pendingRoll: Double? = null
  @Volatile private var pendingVfov: Double? = null
  @Volatile private var pendingViewCorrection: Quaternion? = null

  /// Dedicated render thread — owns all CesiumEngine + GPU work.
  private var renderThread: HandlerThread? = null
  private var renderHandler: Handler? = null

  override var ionAccessToken: String = ""
    set(value) {
      field = value
      val assetId = ionAssetId.toLong()
      postToRender { bridge?.updateIonAccessToken(value, assetId) }
    }

  override var ionAssetId: Double = 1.0
    set(value) {
      field = value
      val token = ionAccessToken
      postToRender { bridge?.updateIonAccessToken(token, value.toLong()) }
    }

  override var initialCamera: CameraState = CameraState(
    latitude = 46.15,
    longitude = 7.35,
    altitude = 12000.0,
    heading = 129.0,
    pitch = -45.0,
    roll = 0.0,
    verticalFovDeg = 60.0,
  )

  override var pauseRendering: Boolean = false
    set(value) {
      field = value
      if (!value) {
        postToRender { bridge?.markNeedsRender() }
        scheduleNextFrame(immediate = true)
      }
    }

  override var maximumScreenSpaceError: Double = 32.0
    set(value) { field = value; postToRender { bridge?.setMaxSSE(value) } }

  override var maximumSimultaneousTileLoads: Double = 12.0
    set(value) { field = value; postToRender { bridge?.setMaxSimLoads(value.toInt()) } }

  override var loadingDescendantLimit: Double = 20.0
    set(value) { field = value; postToRender { bridge?.setLoadDescLim(value.toInt()) } }

  override var msaaSampleCount: Double = 1.0
    set(value) { field = value; postToRender { bridge?.setMsaa(value.toInt()) } }

  override var ionImageryAssetId: Double = 1.0
    set(value) { field = value; postToRender { bridge?.updateImageryAssetId(value.toLong()) } }

  // ── Optional perf / quality knobs ────────────────────────────────────────
  override var maximumCachedMiB: Double? = null
    set(value) { field = value; if (value != null) postToRender { bridge?.setMaximumCachedMiB(value.toInt()) } }
  override var preloadAncestors: Boolean? = null
    set(value) { field = value; if (value != null) postToRender { bridge?.setPreloadAncestors(value) } }
  override var preloadSiblings: Boolean? = null
    set(value) { field = value; if (value != null) postToRender { bridge?.setPreloadSiblings(value) } }
  override var forbidHoles: Boolean? = null
    set(value) { field = value; if (value != null) postToRender { bridge?.setForbidHoles(value) } }
  override var enableWaterMask: Boolean? = null
    set(value) { field = value; if (value != null) postToRender { bridge?.setEnableWaterMask(value) } }
  override var enableFogCulling: Boolean? = null
    set(value) { field = value; if (value != null) postToRender { bridge?.setEnableFogCulling(value) } }
  override var enforceCulledScreenSpaceError: Boolean? = null
    set(value) { field = value; if (value != null) postToRender { bridge?.setEnforceCulledScreenSpaceError(value) } }
  override var culledScreenSpaceError: Double? = null
    set(value) { field = value; if (value != null) postToRender { bridge?.setCulledScreenSpaceError(value) } }
  override var enableLodTransitionPeriod: Boolean? = null
    set(value) { field = value; if (value != null) postToRender { bridge?.setEnableLodTransitionPeriod(value) } }
  override var lodTransitionLength: Double? = null
    set(value) { field = value; if (value != null) postToRender { bridge?.setLodTransitionLength(value) } }
  override var sqliteCacheMaxRows: Double? = null
    set(value) { field = value; if (value != null) postToRender { bridge?.setSqliteCacheMaxRows(value.toInt()) } }
  override var taskProcessorThreads: Double? = null
    set(value) { field = value; if (value != null) postToRender { bridge?.setTaskProcessorThreads(value.toInt()) } }
  override var minAltitudeAboveTerrain: Double? = null
    set(value) { field = value; if (value != null) postToRender { bridge?.setMinAltitudeAboveTerrain(value.toFloat()) } }

  // ── Rate caps ────────────────────────────────────────────────────────────
  override var maxYawRateDegSec: Double?   = null
    set(value) { field = value; pushRateCaps() }
  override var maxPitchRateDegSec: Double? = null
    set(value) { field = value; pushRateCaps() }
  override var maxRollRateDegSec: Double?  = null
    set(value) { field = value; pushRateCaps() }
  override var maxClimbRateMps: Double?    = null
    set(value) { field = value; pushRateCaps() }
  override var maxGroundSpeedMps: Double?  = null
    set(value) { field = value; pushRateCaps() }

  private fun pushRateCaps() {
    val yaw   = maxYawRateDegSec ?: 0.0
    val pitch = maxPitchRateDegSec ?: 0.0
    val roll  = maxRollRateDegSec ?: 0.0
    val climb = maxClimbRateMps ?: 0.0
    val gnd   = maxGroundSpeedMps ?: 0.0
    postToRender { bridge?.setRateCaps(yaw, pitch, roll, climb, gnd) }
  }

  override var onMetrics: ((metrics: CesiumMetrics) -> Unit)? = null

  override var onActualCamera: ((camera: CameraState) -> Unit)? = null
  // Both accessed only on the render thread; no extra locking needed.
  private var actualCameraLastEmitSec: Double = 0.0
  private var actualCameraLastSent: CameraState? = null

  // Returns true when the two snapshots differ enough to warrant a JS dispatch.
  // Thresholds mirror EngineTunables.hpp kActualCameraCallbackInterval* constants.
  private fun actualCameraChanged(a: CameraState, b: CameraState): Boolean =
    Math.abs(a.latitude       - b.latitude)       > 1e-7  ||
    Math.abs(a.longitude      - b.longitude)      > 1e-7  ||
    Math.abs(a.altitude       - b.altitude)       > 0.05  ||
    Math.abs(a.heading        - b.heading)        > 0.01  ||
    Math.abs(a.pitch          - b.pitch)          > 0.01  ||
    Math.abs(a.roll           - b.roll)           > 0.01  ||
    Math.abs(a.verticalFovDeg - b.verticalFovDeg) > 0.01

  override val view: View get() = surfaceView

  private val frameCallback = Choreographer.FrameCallback {
    renderLoopActive = false
    if (isShutdown || pauseRendering || bridge == null) {
      return@FrameCallback
    }

    if (!renderInFlight.compareAndSet(false, true)) {
      scheduleNextFrame(immediate = true)
      return@FrameCallback
    }

    val rh = renderHandler
    if (rh == null) {
      renderInFlight.set(false)
      return@FrameCallback
    }
    val nowSeconds = SystemClock.elapsedRealtimeNanos() / 1_000_000_000.0
    rh.post { runFrameOnRenderThread(nowSeconds) }
  }

  /// Run on the render thread immediately after the bridge is created, before
  /// the first frame.
  private fun runFrameOnRenderThread(nowSeconds: Double) {
    val b = bridge
    if (b == null) {
      renderInFlight.set(false)
      return
    }

    // Post-loading grace period: keep the loop at full speed for 2 seconds
    // after the last non-zero loading frame so LOD transitions complete quickly
    // and the next refinement level is discovered without a multi-second idle gap.
    val nowNs = System.nanoTime()
    val inGracePeriod = lastLoadingTimeNs > 0L &&
        (nowNs - lastLoadingTimeNs) < 2_000_000_000L

    val shouldRender = b.shouldRenderNextFrame() || inGracePeriod
    val nextImmediate: Boolean
    if (shouldRender) {
      idleProbeAccumulator = 0.0
      b.renderFrame(nowSeconds)
      // Stamp after renderFrame so the freshly-updated tile counts are used.
      if (b.getMetricsTilesLoading() > 0) lastLoadingTimeNs = nowNs
      nextImmediate = true
    } else {
      // Approximate dt for the idle probe; not used for motion.
      idleProbeAccumulator += 1.0 / 60.0
      if (idleProbeAccumulator >= 0.25) {
        idleProbeAccumulator = 0.0
        b.markNeedsRender()
      }
      nextImmediate = false
    }

    metricsFrameCounter++
    if (metricsFrameCounter >= 20) {
      metricsFrameCounter = 0
      val cb = if (isShutdown) null else onMetrics
      if (cb != null) {
        cb(CesiumMetrics(
          fps = b.getMetricsFps(),
          tilesRendered = b.getMetricsTilesRendered().toDouble(),
          tilesLoading = b.getMetricsTilesLoading().toDouble(),
          tilesVisited = b.getMetricsTilesVisited().toDouble(),
          ionTokenConfigured = b.getMetricsIonTokenConfigured(),
          tlsConfigured = b.getMetricsTlsConfigured(),
          tilesetReady = b.getMetricsTilesetReady(),
          creditsPlainText = b.getMetricsCreditsPlainText(),
        ))
      }
    }

    // onActualCamera: poll at 5 Hz (kActualCameraCallbackIntervalSec = 0.2 s),
    // but only dispatch when at least one field has moved beyond its epsilon.
    val acCb = if (isShutdown) null else onActualCamera
    if (acCb != null && (nowSeconds - actualCameraLastEmitSec) >= 0.2) {
      actualCameraLastEmitSec = nowSeconds
      val candidate = CameraState(
        latitude     = b.getActualLatitude(),
        longitude    = b.getActualLongitude(),
        altitude     = b.getActualAltitude(),
        heading      = b.getActualHeading(),
        pitch        = b.getActualPitch(),
        roll         = b.getActualRoll(),
        verticalFovDeg = b.getActualVerticalFovDeg(),
      )
      val prev = actualCameraLastSent
      if (prev == null || actualCameraChanged(candidate, prev)) {
        actualCameraLastSent = candidate
        acCb(candidate)
      }
    }

    renderInFlight.set(false)

    surfaceView.post { scheduleNextFrame(immediate = nextImmediate) }
  }

  private val surfaceCallback = object : SurfaceHolder.Callback {
    override fun surfaceCreated(holder: SurfaceHolder) {
      val surface = holder.surface
      val w = surfaceView.width
      val h = surfaceView.height
      if (w <= 0 || h <= 0) return

      ensureRenderThread()
      val rh = renderHandler ?: return

      val cacheDir = appContext.cacheDir.absolutePath
      val cacertPath = extractCacert()
      rh.post {
        val b = CesiumBridgeJNI()
        b.init(surface, w, h, cacheDir, cacertPath)
        bridge = b
        syncBridgePropsOnRenderThread(b)

        val seed = pendingTeleport ?: initialCamera
        b.teleport(
          seed.latitude, seed.longitude, seed.altitude,
          seed.heading, seed.pitch, seed.roll, seed.verticalFovDeg,
        )
        pendingTeleport = null

        val la = pendingLat
        val lo = pendingLon
        if (la != null && lo != null) {
          b.setPosition(la, lo); pendingLat = null; pendingLon = null
        }
        pendingAltitude?.let { b.setAltitude(it);  pendingAltitude = null }
        pendingHeading?.let  { b.setHeading(it);   pendingHeading  = null }
        val p = pendingPitch; val r = pendingRoll
        if (p != null && r != null) {
          b.setAttitude(p, r); pendingPitch = null; pendingRoll = null
        }
        pendingVfov?.let { b.setVerticalFovDeg(it); pendingVfov = null }
        pendingViewCorrection?.let { q ->
          b.setViewCorrection(q.w, q.x, q.y, q.z); pendingViewCorrection = null
        }

        if (ionAccessToken.isNotEmpty()) {
          b.updateIonAccessToken(ionAccessToken, ionAssetId.toLong())
        }
        if (ionImageryAssetId != 1.0) {
          b.updateImageryAssetId(ionImageryAssetId.toLong())
        }
        b.setMsaa(msaaSampleCount.toInt())

        pushRateCaps()

        surfaceView.post { scheduleNextFrame(immediate = true) }
      }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
      postToRender {
        bridge?.resize(width, height)
        bridge?.markNeedsRender()
        surfaceView.post { scheduleNextFrame(immediate = true) }
      }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
      // Flip the shutdown gate and drop the JS callback before any teardown
      // work — any in-flight render-thread frame or queued choreographer tick
      // will then bail out without touching the bridge or emitting metrics
      // into a dying JS runtime.
      isShutdown = true
      onMetrics = null
      onActualCamera = null
      actualCameraLastSent = null

      val rh = renderHandler
      val ht = renderThread

      if (rh == null || ht == null) {
        bridge?.shutdown()
        bridge?.destroy()
        bridge = null
        return
      }

      val done = java.util.concurrent.CountDownLatch(1)
      rh.post {
        try {
          bridge?.shutdown()
          bridge?.destroy()
        } catch (t: Throwable) {
          Log.w("CesiumView", "render-thread shutdown threw", t)
        } finally {
          bridge = null
          done.countDown()
        }
      }
      if (!done.await(1500, java.util.concurrent.TimeUnit.MILLISECONDS)) {
        Log.w("CesiumView", "Render-thread shutdown did not complete in 1.5s; tearing down handler thread anyway.")
      }
      stopRenderThread()
    }
  }

  init {
    surfaceView.holder.addCallback(surfaceCallback)
  }

  // ── Render-thread helpers ────────────────────────────────────────────────

  private fun ensureRenderThread() {
    if (renderThread != null) return
    val ht = HandlerThread("CesiumRender", Process.THREAD_PRIORITY_DISPLAY)
    ht.start()
    renderHandler = Handler(ht.looper)
    renderThread = ht
  }

  private fun stopRenderThread() {
    val ht = renderThread ?: return
    renderHandler = null
    renderThread = null
    ht.quitSafely()
  }

  private fun postToRender(action: () -> Unit) {
    renderHandler?.post(action)
  }

  // ── Camera readback ─────────────────────────────────────────────────────
  override fun getActualCamera(): Promise<CameraState> {
    val b = bridge ?: return Promise.resolved(initialCamera)
    return Promise.resolved(CameraState(
      latitude = b.getActualLatitude(),
      longitude = b.getActualLongitude(),
      altitude = b.getActualAltitude(),
      heading = b.getActualHeading(),
      pitch = b.getActualPitch(),
      roll = b.getActualRoll(),
      verticalFovDeg = b.getActualVerticalFovDeg(),
    ))
  }

  override fun getDemandCamera(): Promise<CameraState> {
    val b = bridge ?: return Promise.resolved(initialCamera)
    return Promise.resolved(CameraState(
      latitude = b.getDemandLatitude(),
      longitude = b.getDemandLongitude(),
      altitude = b.getDemandAltitude(),
      heading = b.getDemandHeading(),
      pitch = b.getDemandPitch(),
      roll = b.getDemandRoll(),
      verticalFovDeg = b.getDemandVerticalFovDeg(),
    ))
  }

  override fun getViewCorrection(): Promise<Quaternion> {
    val b = bridge
      ?: return Promise.resolved(Quaternion(w = 1.0, x = 0.0, y = 0.0, z = 0.0))
    return Promise.resolved(
      Quaternion(
        w = b.getViewCorrectionW(),
        x = b.getViewCorrectionX(),
        y = b.getViewCorrectionY(),
        z = b.getViewCorrectionZ(),
      ),
    )
  }

  // ── Per-DoF camera setters ──────────────────────────────────────────────
  // These bypass the renderHandler hop: the underlying CameraIntegrator owns
  // its own mutex and handles concurrent writers safely. Keeping the hot
  // path off the render thread reduces latency for high-frequency
  // (Reanimated worklet, 50 Hz IMU) drivers.
  override fun setPosition(latitude: Double, longitude: Double) {
    val b = bridge
    if (b != null) {
      b.setPosition(latitude, longitude)
      scheduleNextFrame(immediate = true)
    } else {
      pendingLat = latitude; pendingLon = longitude
    }
  }

  override fun setAltitude(altitudeMeters: Double) {
    val b = bridge
    if (b != null) { b.setAltitude(altitudeMeters); scheduleNextFrame(immediate = true) }
    else           { pendingAltitude = altitudeMeters }
  }

  override fun setHeading(headingDeg: Double) {
    val b = bridge
    if (b != null) { b.setHeading(headingDeg); scheduleNextFrame(immediate = true) }
    else           { pendingHeading = headingDeg }
  }

  override fun setAttitude(pitchDeg: Double, rollDeg: Double) {
    val b = bridge
    if (b != null) { b.setAttitude(pitchDeg, rollDeg); scheduleNextFrame(immediate = true) }
    else           { pendingPitch = pitchDeg; pendingRoll = rollDeg }
  }

  override fun setViewCorrection(q: Quaternion) {
    val b = bridge
    if (b != null) { b.setViewCorrection(q.w, q.x, q.y, q.z); scheduleNextFrame(immediate = true) }
    else           { pendingViewCorrection = q }
  }

  override fun setVerticalFov(deg: Double) {
    val b = bridge
    if (b != null) { b.setVerticalFovDeg(deg); scheduleNextFrame(immediate = true) }
    else           { pendingVfov = deg }
  }

  override fun teleport(camera: CameraState) {
    val b = bridge
    if (b != null) {
      b.teleport(camera.latitude, camera.longitude, camera.altitude,
                 camera.heading, camera.pitch, camera.roll, camera.verticalFovDeg)
      scheduleNextFrame(immediate = true)
    } else {
      pendingTeleport = camera
    }
  }

  /// Render-thread setup, called once after the bridge is built.
  private fun syncBridgePropsOnRenderThread(b: CesiumBridgeJNI) {
    b.setMaxSSE(maximumScreenSpaceError)
    b.setMaxSimLoads(maximumSimultaneousTileLoads.toInt())
    b.setLoadDescLim(loadingDescendantLimit.toInt())
    maximumCachedMiB?.let { b.setMaximumCachedMiB(it.toInt()) }
    preloadAncestors?.let { b.setPreloadAncestors(it) }
    preloadSiblings?.let { b.setPreloadSiblings(it) }
    forbidHoles?.let { b.setForbidHoles(it) }
    enableWaterMask?.let { b.setEnableWaterMask(it) }
    enableFogCulling?.let { b.setEnableFogCulling(it) }
    enforceCulledScreenSpaceError?.let { b.setEnforceCulledScreenSpaceError(it) }
    culledScreenSpaceError?.let { b.setCulledScreenSpaceError(it) }
    enableLodTransitionPeriod?.let { b.setEnableLodTransitionPeriod(it) }
    lodTransitionLength?.let { b.setLodTransitionLength(it) }
    sqliteCacheMaxRows?.let { b.setSqliteCacheMaxRows(it.toInt()) }
    taskProcessorThreads?.let { b.setTaskProcessorThreads(it.toInt()) }
    minAltitudeAboveTerrain?.let { b.setMinAltitudeAboveTerrain(it.toFloat()) }
  }

  /**
   * Schedule the next frame callback.
   */
  private fun scheduleNextFrame(immediate: Boolean) {
    if (renderLoopActive || pauseRendering || bridge == null) return
    renderLoopActive = true
    if (immediate) {
      Choreographer.getInstance().postFrameCallback(frameCallback)
    } else {
      Choreographer.getInstance().postFrameCallbackDelayed(frameCallback, 250L)
    }
  }

  private fun extractCacert(): String {
    val outFile = File(appContext.cacheDir, "cacert.pem")
    try {
      appContext.assets.open("cacert.pem").use { input ->
        val assetSize = input.available().toLong()
        if (!outFile.exists() || outFile.length() != assetSize) {
          FileOutputStream(outFile).use { output -> input.copyTo(output) }
        }
      }
    } catch (_: Exception) {
      return if (outFile.exists()) outFile.absolutePath else ""
    }
    return outFile.absolutePath
  }
}
