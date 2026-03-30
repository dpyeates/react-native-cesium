package com.margelo.nitro.reactnativecesium

import android.content.Context
import android.view.SurfaceView
import android.view.View
import com.margelo.nitro.reactnativecesium.HybridCesiumViewSpec
import com.margelo.nitro.reactnativecesium.views.HybridCesiumViewManager

class HybridCesiumView(context: Context) : HybridCesiumViewSpec() {
  private val surfaceView = SurfaceView(context)

  override var ionAccessToken: String = ""
  override var ionAssetId: Double = 1.0
  // Swiss Valais: aerial default toward Matterhorn
  override var cameraLatitude: Double = 46.15
  override var cameraLongitude: Double = 7.35
  override var cameraAltitude: Double = 12000.0
  override var cameraHeading: Double = 129.0
  override var cameraPitch: Double = -45.0
  override var cameraRoll: Double = 0.0
  override var debugOverlay: Boolean = false

  override val view: View get() = surfaceView
}
