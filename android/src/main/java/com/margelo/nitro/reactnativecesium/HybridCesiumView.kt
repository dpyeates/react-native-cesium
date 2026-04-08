package com.margelo.nitro.reactnativecesium

import android.content.Context
import android.view.SurfaceView
import android.view.View
import com.margelo.nitro.core.Promise
/**
 * Android stub: rendering is not wired yet. `initialCamera` is only the
 * creation-time seed; runtime camera updates flow through `setCamera()`.
 */
class HybridCesiumView(context: Context) : HybridCesiumViewSpec() {
  private val surfaceView = SurfaceView(context)

  override var ionAccessToken: String = ""
  override var ionAssetId: Double = 1.0
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
  override var maximumScreenSpaceError: Double = 32.0
  override var maximumSimultaneousTileLoads: Double = 12.0
  override var loadingDescendantLimit: Double = 20.0
  override var msaaSampleCount: Double = 1.0
  override var ionImageryAssetId: Double = 1.0
  override var onMetrics: ((metrics: CesiumMetrics) -> Unit)? = null

  override val view: View get() = surfaceView

  override fun getCameraState(): Promise<CameraState> {
    return Promise.resolved(initialCamera)
  }

  override fun setCamera(camera: CameraState) {
    // Android rendering path is currently a stub; keep latest demand locally.
    initialCamera = camera
  }

}
