package com.margelo.nitro.reactnativecesium

import android.content.Context
import android.view.Choreographer
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import com.margelo.nitro.core.Promise
import java.io.File
import java.io.FileOutputStream

class HybridCesiumView(private val appContext: Context) : HybridCesiumViewSpec() {
  private val surfaceView = SurfaceView(appContext)
  private var bridge: CesiumBridgeJNI? = null
  private var metricsFrameCounter = 0
  private var idleProbeAccumulator = 0.0
  private var usingLowRefreshRate = false
  private var lastFrameTimeNanos = 0L
  private var lastPushedCamera: CameraState? = null
  private var lastPushedViewCorrection: Quaternion? = null
  private var renderLoopActive = false

  override var ionAccessToken: String = ""
    set(value) {
      field = value
      bridge?.updateIonAccessToken(value, ionAssetId.toLong())
    }

  override var ionAssetId: Double = 1.0
    set(value) {
      field = value
      bridge?.updateIonAccessToken(ionAccessToken, value.toLong())
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
        bridge?.markNeedsRender()
        scheduleNextFrame()
      }
    }

  override var maximumScreenSpaceError: Double = 32.0
    set(value) { field = value; bridge?.setMaxSSE(value) }

  override var maximumSimultaneousTileLoads: Double = 12.0
    set(value) { field = value; bridge?.setMaxSimLoads(value.toInt()) }

  override var loadingDescendantLimit: Double = 20.0
    set(value) { field = value; bridge?.setLoadDescLim(value.toInt()) }

  override var msaaSampleCount: Double = 1.0
    set(value) { field = value; bridge?.setMsaa(value.toInt()) }

  override var ionImageryAssetId: Double = 1.0
    set(value) { field = value; bridge?.updateImageryAssetId(value.toLong()) }

  override var onMetrics: ((metrics: CesiumMetrics) -> Unit)? = null

  override val view: View get() = surfaceView

  private val frameCallback = Choreographer.FrameCallback { frameTimeNanos ->
    renderLoopActive = false
    if (pauseRendering || bridge == null) return@FrameCallback

    val dt = if (lastFrameTimeNanos > 0) {
      (frameTimeNanos - lastFrameTimeNanos).coerceAtLeast(1_000_000L) / 1_000_000_000.0
    } else {
      1.0 / 60.0
    }
    lastFrameTimeNanos = frameTimeNanos

    val shouldRender = bridge?.shouldRenderNextFrame() ?: false
    if (shouldRender) {
      usingLowRefreshRate = false
      idleProbeAccumulator = 0.0
      bridge?.renderFrame(dt)
    } else {
      usingLowRefreshRate = true
      idleProbeAccumulator += dt
      if (idleProbeAccumulator >= 0.25) {
        idleProbeAccumulator = 0.0
        bridge?.markNeedsRender()
      }
    }

    metricsFrameCounter++
    if (metricsFrameCounter >= 20) {
      metricsFrameCounter = 0
      val b = bridge ?: return@FrameCallback
      val cb = onMetrics ?: return@FrameCallback
      cb(CesiumMetrics(
        fps = b.getMetricsFps(),
        tilesRendered = b.getMetricsTilesRendered().toDouble(),
        tilesLoading = b.getMetricsTilesLoading().toDouble(),
        tilesVisited = b.getMetricsTilesVisited().toDouble(),
        ionTokenConfigured = b.getMetricsIonTokenConfigured(),
        tilesetReady = b.getMetricsTilesetReady(),
        creditsPlainText = b.getMetricsCreditsPlainText(),
      ))
    }

    scheduleNextFrame()
  }

  private val surfaceCallback = object : SurfaceHolder.Callback {
    override fun surfaceCreated(holder: SurfaceHolder) {
      val surface = holder.surface
      val w = surfaceView.width
      val h = surfaceView.height
      if (w <= 0 || h <= 0) return

      val b = CesiumBridgeJNI()
      val cacheDir = appContext.cacheDir.absolutePath
      val cacertPath = extractCacert()
      b.init(surface, w, h, cacheDir, cacertPath)
      bridge = b

      syncPropsTobridge()
      if (ionAccessToken.isNotEmpty()) {
        b.updateIonAccessToken(ionAccessToken, ionAssetId.toLong())
      }
      pushCameraIfChanged(initialCamera)
      if (ionImageryAssetId != 1.0) {
        b.updateImageryAssetId(ionImageryAssetId.toLong())
      }
      b.setMsaa(msaaSampleCount.toInt())

      lastFrameTimeNanos = 0L
      scheduleNextFrame()
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
      bridge?.resize(width, height)
      bridge?.markNeedsRender()
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
      bridge?.shutdown()
      bridge?.destroy()
      bridge = null
    }
  }

  init {
    surfaceView.holder.addCallback(surfaceCallback)
  }

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
    initialCamera = camera
    pushCameraIfChanged(camera)
  }

  override fun setCameraQuaternion(camera: CameraState, viewCorrection: Quaternion) {
    initialCamera = camera
    pushCameraQuaternionIfChanged(camera, viewCorrection)
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

  private fun syncPropsTobridge() {
    bridge?.setMaxSSE(maximumScreenSpaceError)
    bridge?.setMaxSimLoads(maximumSimultaneousTileLoads.toInt())
    bridge?.setLoadDescLim(loadingDescendantLimit.toInt())
  }

  private fun scheduleNextFrame() {
    if (renderLoopActive || pauseRendering || bridge == null) return
    renderLoopActive = true
    Choreographer.getInstance().postFrameCallback(frameCallback)
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
