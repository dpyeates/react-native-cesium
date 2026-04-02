package com.margelo.nitro.reactnativecesium

import android.content.Context
import android.view.SurfaceView
import android.view.View
import com.margelo.nitro.core.Promise
/**
 * Android stub: rendering is not wired yet. Props and methods exist so Nitro
 * codegen matches iOS; all operations are no-ops or return safe defaults.
 */
class HybridCesiumView(context: Context) : HybridCesiumViewSpec() {
  private val surfaceView = SurfaceView(context)

  override var ionAccessToken: String = ""
  override var ionAssetId: Double = 1.0
  override var cameraLatitude: Double = 46.15
  override var cameraLongitude: Double = 7.35
  override var cameraAltitude: Double = 12000.0
  override var cameraHeading: Double = 129.0
  override var cameraPitch: Double = -45.0
  override var cameraRoll: Double = 0.0
  override var cameraVerticalFovDeg: Double = 60.0
  override var debugOverlay: Boolean = false
  override var pauseRendering: Boolean = false
  override var maximumScreenSpaceError: Double = 32.0
  override var maximumSimultaneousTileLoads: Double = 12.0
  override var loadingDescendantLimit: Double = 20.0
  override var msaaSampleCount: Double = 1.0
  override var showCredits: Boolean = true
  override var ionImageryAssetId: Double = 1.0
  override var onMetrics: ((metrics: CesiumMetrics) -> Unit)? = null

  override val view: View get() = surfaceView

  override fun setJoystickRates(pitchRate: Double, rollRate: Double) {}

  override fun getCameraState(): Promise<CameraState> {
    return Promise.resolved(
      CameraState(
        latitude = 0.0,
        longitude = 0.0,
        altitude = 0.0,
        heading = 0.0,
        pitch = 0.0,
        roll = 0.0,
        verticalFovDeg = 60.0,
      )
    )
  }

  override fun flyTo(
    latitude: Double,
    longitude: Double,
    altitude: Double,
    heading: Double,
    pitch: Double,
    roll: Double,
    durationSeconds: Double,
  ) {}

  override fun lookAt(
    targetLatitude: Double,
    targetLongitude: Double,
    targetAltitude: Double,
    durationSeconds: Double,
  ) {}
}
