package com.margelo.nitro.reactnativecesium

import android.content.Context
import android.os.Handler
import android.os.HandlerThread
import android.os.Process
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
  /// requests via `renderHandler`. Marked volatile so init/shutdown observers
  /// (`bridge != null`) on the UI thread see the latest reference, but real
  /// work is always serialized through the render thread.
  @Volatile private var bridge: CesiumBridgeJNI? = null
  private var metricsFrameCounter = 0
  private var idleProbeAccumulator = 0.0
  private var lastFrameTimeNanos = 0L
  /// Last-pushed camera state — read/written on the render thread (via
  /// `renderHandler.post`), never directly on the UI thread.
  private var lastPushedCamera: CameraState? = null
  private var lastPushedViewCorrection: Quaternion? = null
  /// Camera state requested by `setCamera` before the bridge was ready.
  /// Updated on the UI thread, drained on the render thread.
  @Volatile private var pendingRuntimeCamera: CameraState? = null
  @Volatile private var pendingRuntimeViewCorrection: Quaternion? = null
  /// True while a Choreographer frame callback is pending. Mutated on the UI
  /// thread inside the callback / scheduleNextFrame.
  private var renderLoopActive = false
  /// Drops Choreographer ticks while the render thread is still finishing the
  /// previous frame. Set on UI thread when posting; cleared on render thread
  /// after the bridge call completes.
  private val renderInFlight = AtomicBoolean(false)

  /// Dedicated render thread — owns all CesiumEngine + GPU work. Created lazily
  /// when the first SurfaceHolder.Callback fires. The Choreographer (UI thread)
  /// posts render requests here; bridge config setters likewise post here so
  /// they serialize with rendering and never race with engine state.
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

  override var onMetrics: ((metrics: CesiumMetrics) -> Unit)? = null

  override val view: View get() = surfaceView

  private val frameCallback = Choreographer.FrameCallback { frameTimeNanos ->
    renderLoopActive = false
    if (pauseRendering || bridge == null) {
      return@FrameCallback
    }

    val dt = if (lastFrameTimeNanos > 0) {
      (frameTimeNanos - lastFrameTimeNanos).coerceAtLeast(1_000_000L) / 1_000_000_000.0
    } else {
      1.0 / 60.0
    }
    lastFrameTimeNanos = frameTimeNanos

    // Drop the tick if the previous frame's render hasn't finished — keeps the
    // render thread from queuing up an unbounded backlog under stutter.
    if (!renderInFlight.compareAndSet(false, true)) {
      // Re-arm so we try again next vsync.
      scheduleNextFrame(immediate = true)
      return@FrameCallback
    }

    val rh = renderHandler
    if (rh == null) {
      renderInFlight.set(false)
      return@FrameCallback
    }
    rh.post { runFrameOnRenderThread(dt) }
  }

  /// Called on the render thread. Owns ALL CesiumEngine + GPU access; the UI
  /// thread only signals demand here. Safe to read `onMetrics` because Nitro
  /// callbacks dispatch internally to the JS thread.
  private fun runFrameOnRenderThread(dt: Double) {
    val b = bridge
    if (b == null) {
      renderInFlight.set(false)
      return
    }

    val shouldRender = b.shouldRenderNextFrame()
    val nextImmediate: Boolean
    if (shouldRender) {
      idleProbeAccumulator = 0.0
      b.renderFrame(dt)
      nextImmediate = true
    } else {
      idleProbeAccumulator += dt
      if (idleProbeAccumulator >= 0.25) {
        idleProbeAccumulator = 0.0
        b.markNeedsRender()
      }
      nextImmediate = false
    }

    metricsFrameCounter++
    if (metricsFrameCounter >= 20) {
      metricsFrameCounter = 0
      val cb = onMetrics
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

    renderInFlight.set(false)

    // Re-arm on the UI thread so Choreographer remains the source of truth
    // for vsync timing. Posting a no-op runnable to surfaceView keeps us off
    // the render thread for that small piece of bookkeeping.
    surfaceView.post { scheduleNextFrame(immediate = nextImmediate) }
  }

  private val surfaceCallback = object : SurfaceHolder.Callback {
    override fun surfaceCreated(holder: SurfaceHolder) {
      val surface = holder.surface
      val w = surfaceView.width
      val h = surfaceView.height
      if (w <= 0 || h <= 0) return

      // Spin up the render thread on first surfaceCreated. If we previously
      // tore it down (e.g. on backgrounding), re-spawn cleanly.
      ensureRenderThread()
      val rh = renderHandler ?: return

      val cacheDir = appContext.cacheDir.absolutePath
      val cacertPath = extractCacert()
      // Init runs on the render thread because Vulkan instance/device creation
      // and Cesium Native init both touch state we'd rather only ever poke
      // from one thread.
      rh.post {
        val b = CesiumBridgeJNI()
        b.init(surface, w, h, cacheDir, cacertPath)
        bridge = b
        syncBridgePropsOnRenderThread(b)

        b.updateCamera(
          initialCamera.latitude, initialCamera.longitude, initialCamera.altitude,
          initialCamera.heading, initialCamera.pitch, initialCamera.roll,
        )
        b.setVerticalFovDeg(initialCamera.verticalFovDeg)
        lastPushedCamera = initialCamera

        val pending = pendingRuntimeCamera
        if (pending != null) {
          val q = pendingRuntimeViewCorrection
          if (q != null) {
            b.updateCameraQuaternion(
              pending.latitude, pending.longitude, pending.altitude,
              pending.heading, pending.pitch, pending.roll,
              q.w, q.x, q.y, q.z,
            )
            lastPushedViewCorrection = q
          } else {
            b.updateCamera(
              pending.latitude, pending.longitude, pending.altitude,
              pending.heading, pending.pitch, pending.roll,
            )
          }
          b.setVerticalFovDeg(pending.verticalFovDeg)
          lastPushedCamera = pending
        }

        if (ionAccessToken.isNotEmpty()) {
          b.updateIonAccessToken(ionAccessToken, ionAssetId.toLong())
        }
        if (ionImageryAssetId != 1.0) {
          b.updateImageryAssetId(ionImageryAssetId.toLong())
        }
        b.setMsaa(msaaSampleCount.toInt())

        // Start the rendering loop here, after the bridge is fully
        // initialized. scheduleNextFrame must be invoked on the UI thread
        // because Choreographer.postFrameCallback() requires it.
        surfaceView.post { scheduleNextFrame(immediate = true) }
      }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
      postToRender {
        bridge?.resize(width, height)
        bridge?.markNeedsRender()
        // Re-arm the loop in case surfaceChanged fires before the bridge was
        // created (rare, but possible on cold starts). Idempotent: scheduleNextFrame
        // early-returns if the loop is already active.
        surfaceView.post { scheduleNextFrame(immediate = true) }
      }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
      val rh = renderHandler
      val ht = renderThread

      if (rh == null || ht == null) {
        // Nothing to drain; clear bridge reference if it somehow exists.
        bridge?.shutdown()
        bridge?.destroy()
        bridge = null
        return
      }

      // Block the UI thread (briefly) until the render thread has shut the
      // bridge down — surfaceDestroyed contract requires the surface to be
      // released before this returns. We cap our wait so a stuck GPU teardown
      // can't hang the UI thread indefinitely (matches our 1s shutdown
      // semaphore policy on the Vulkan / Metal side).
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
        Log.w("CesiumView", "Render-thread shutdown did not complete in 1.5s; "
                + "tearing down handler thread anyway.")
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

  /// Posts an action to the render thread. When the render thread isn't
  /// running yet (early prop assignment, or after teardown), we silently drop
  /// the action — bridges always take their initial config from the
  /// `syncBridgePropsOnRenderThread` step inside surfaceCreated.
  private fun postToRender(action: () -> Unit) {
    renderHandler?.post(action)
  }

  /// Camera reads cross the JNI from the JS thread (Nitro promise resolution).
  /// They read snapshot scalar values through CesiumBridgeAndroid → engine →
  /// GlobeCamera, none of which is mutated outside the render thread, so the
  /// worst that can happen is observing a value from a frame slightly behind
  /// what the render thread is encoding right now — acceptable for a UI-driven
  /// camera read.
  override fun getCameraState(): Promise<CameraState> {
    val b = bridge ?: return Promise.resolved(initialCamera)
    return Promise.resolved(CameraState(
      latitude = b.getCameraLat(),
      longitude = b.getCameraLon(),
      altitude = b.getCameraAlt(),
      heading = b.getCameraHeading(),
      pitch = b.getCameraPitch(),
      roll = b.getCameraRoll(),
      verticalFovDeg = b.getVerticalFovDeg(),
    ))
  }

  override fun setCamera(camera: CameraState) {
    // Runtime camera control. Does NOT mutate `initialCamera` (which is
    // construction-time only).
    pendingRuntimeCamera = camera
    pendingRuntimeViewCorrection = null
    postToRender {
      pushCameraIfChanged(camera)
      bridge?.markNeedsRender()
    }
    scheduleNextFrame(immediate = true)
  }

  override fun setCameraQuaternion(camera: CameraState, viewCorrection: Quaternion) {
    pendingRuntimeCamera = camera
    pendingRuntimeViewCorrection = viewCorrection
    postToRender {
      pushCameraQuaternionIfChanged(camera, viewCorrection)
      bridge?.markNeedsRender()
    }
    scheduleNextFrame(immediate = true)
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

  private fun pushCameraIfChanged(camera: CameraState) {
    val b = bridge ?: return
    val last = lastPushedCamera
    if (last != null &&
        last.latitude == camera.latitude &&
        last.longitude == camera.longitude &&
        last.altitude == camera.altitude &&
        last.heading == camera.heading &&
        last.pitch == camera.pitch &&
        last.roll == camera.roll &&
        last.verticalFovDeg == camera.verticalFovDeg) {
      return
    }
    lastPushedCamera = camera
    b.updateCamera(camera.latitude, camera.longitude, camera.altitude,
                   camera.heading, camera.pitch, camera.roll)
    b.setVerticalFovDeg(camera.verticalFovDeg)
  }

  private fun pushCameraQuaternionIfChanged(camera: CameraState, viewCorrection: Quaternion) {
    val b = bridge ?: return
    val last = lastPushedCamera
    val lastQ = lastPushedViewCorrection
    if (last != null && lastQ != null &&
      last.latitude == camera.latitude &&
      last.longitude == camera.longitude &&
      last.altitude == camera.altitude &&
      last.heading == camera.heading &&
      last.pitch == camera.pitch &&
      last.roll == camera.roll &&
      last.verticalFovDeg == camera.verticalFovDeg &&
      lastQ.w == viewCorrection.w &&
      lastQ.x == viewCorrection.x &&
      lastQ.y == viewCorrection.y &&
      lastQ.z == viewCorrection.z
    ) {
      return
    }
    lastPushedCamera = camera
    lastPushedViewCorrection = viewCorrection
    b.updateCameraQuaternion(
      camera.latitude,
      camera.longitude,
      camera.altitude,
      camera.heading,
      camera.pitch,
      camera.roll,
      viewCorrection.w,
      viewCorrection.x,
      viewCorrection.y,
      viewCorrection.z,
    )
    b.setVerticalFovDeg(camera.verticalFovDeg)
  }

  /// Run on the render thread immediately after the bridge is created, before
  /// the first frame. Matches Swift's syncBridgeOptionsFromProps and pushes
  /// every prop into the freshly-built CesiumEngine so the consumer doesn't
  /// have to re-set them after the surface comes up.
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
  }

  /**
   * Schedule the next frame callback.
   *
   * - `immediate = true`: the engine wants to render this vsync (active pan,
   *   tile loading, prop change). Re-post on the next available vsync.
   * - `immediate = false`: the engine is idle. Re-post in 250ms — matches the
   *   iOS low-refresh range and keeps the main thread mostly asleep while
   *   still picking up late tile completions / network responses.
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
        // Re-extract if the cached copy is missing or a different size to the
        // bundled asset (handles the file being updated across app versions).
        val assetSize = input.available().toLong()
        if (!outFile.exists() || outFile.length() != assetSize) {
          FileOutputStream(outFile).use { output -> input.copyTo(output) }
        }
      }
    } catch (_: Exception) {
      // cacert.pem not bundled as asset — libcurl will use system certs
      return if (outFile.exists()) outFile.absolutePath else ""
    }
    return outFile.absolutePath
  }
}
